//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//

#include "util/hash_linklist_rep.h"

#include "rocksdb/memtablerep.h"
#include "rocksdb/arena.h"
#include "rocksdb/slice.h"
#include "rocksdb/slice_transform.h"
#include "port/port.h"
#include "port/atomic_pointer.h"
#include "util/murmurhash.h"
#include "db/memtable.h"
#include "db/skiplist.h"

namespace rocksdb {
namespace {

typedef const char* Key;

struct Node {
  explicit Node(const Key& k) :
      key(k) {
  }

  Key const key;

  // Accessors/mutators for links.  Wrapped in methods so we can
  // add the appropriate barriers as necessary.
  Node* Next() {
    // Use an 'acquire load' so that we observe a fully initialized
    // version of the returned Node.
    return reinterpret_cast<Node*>(next_.Acquire_Load());
  }
  void SetNext(Node* x) {
    // Use a 'release store' so that anybody who reads through this
    // pointer observes a fully initialized version of the inserted node.
    next_.Release_Store(x);
  }

  // No-barrier variants that can be safely used in a few locations.
  Node* NoBarrier_Next() {
    return reinterpret_cast<Node*>(next_.NoBarrier_Load());
  }
  void NoBarrier_SetNext(Node* x) {
    next_.NoBarrier_Store(x);
  }

private:
  port::AtomicPointer next_;
};

class HashLinkListRep : public MemTableRep {
 public:
  HashLinkListRep(MemTableRep::KeyComparator& compare, Arena* arena,
                  const SliceTransform* transform, size_t bucket_size);

  virtual void Insert(const char* key) override;

  virtual bool Contains(const char* key) const override;

  virtual size_t ApproximateMemoryUsage() override;

  virtual ~HashLinkListRep();

  virtual MemTableRep::Iterator* GetIterator() override;

  virtual MemTableRep::Iterator* GetIterator(const Slice& slice) override;

  virtual MemTableRep::Iterator* GetPrefixIterator(const Slice& prefix)
      override;

  virtual MemTableRep::Iterator* GetDynamicPrefixIterator() override;

 private:
  friend class DynamicIterator;
  typedef SkipList<const char*, MemTableRep::KeyComparator&> FullList;

  size_t bucket_size_;

  // Maps slices (which are transformed user keys) to buckets of keys sharing
  // the same transform.
  port::AtomicPointer* buckets_;

  // The user-supplied transform whose domain is the user keys.
  const SliceTransform* transform_;

  MemTableRep::KeyComparator& compare_;
  // immutable after construction
  Arena* const arena_;

  bool BucketContains(Node* head, const Key& key) const;

  size_t GetHash(const Slice& slice) const {
    return MurmurHash(slice.data(), slice.size(), 0) % bucket_size_;
  }

  Node* GetBucket(size_t i) const {
    return static_cast<Node*>(buckets_[i].Acquire_Load());
  }

  Node* GetBucket(const Slice& slice) const {
    return GetBucket(GetHash(slice));
  }

  Node* NewNode(const Key& key) {
    char* mem = arena_->AllocateAligned(sizeof(Node));
    return new (mem) Node(key);
  }

  bool Equal(const Key& a, const Key& b) const { return (compare_(a, b) == 0); }

  bool KeyIsAfterNode(const Key& key, const Node* n) const {
    // nullptr n is considered infinite
    return (n != nullptr) && (compare_(n->key, key) < 0);
  }

  Node* FindGreaterOrEqualInBucket(Node* head, const Key& key) const;

  class FullListIterator : public MemTableRep::Iterator {
   public:
    explicit FullListIterator(FullList* list)
      : iter_(list), full_list_(list) {}

    virtual ~FullListIterator() {
    }

    // Returns true iff the iterator is positioned at a valid node.
    virtual bool Valid() const {
      return iter_.Valid();
    }

    // Returns the key at the current position.
    // REQUIRES: Valid()
    virtual const char* key() const {
      assert(Valid());
      return iter_.key();
    }

    // Advances to the next position.
    // REQUIRES: Valid()
    virtual void Next() {
      assert(Valid());
      iter_.Next();
    }

    // Advances to the previous position.
    // REQUIRES: Valid()
    virtual void Prev() {
      assert(Valid());
      iter_.Prev();
    }

    // Advance to the first entry with a key >= target
    virtual void Seek(const Slice& internal_key, const char* memtable_key) {
      const char* encoded_key =
          (memtable_key != nullptr) ?
              memtable_key : EncodeKey(&tmp_, internal_key);
      iter_.Seek(encoded_key);
    }

    // Position at the first entry in collection.
    // Final state of iterator is Valid() iff collection is not empty.
    virtual void SeekToFirst() {
      iter_.SeekToFirst();
    }

    // Position at the last entry in collection.
    // Final state of iterator is Valid() iff collection is not empty.
    virtual void SeekToLast() {
      iter_.SeekToLast();
    }
   private:
    FullList::Iterator iter_;
    // To destruct with the iterator.
    std::unique_ptr<FullList> full_list_;
    std::string tmp_;       // For passing to EncodeKey
  };

  class Iterator : public MemTableRep::Iterator {
   public:
    explicit Iterator(const HashLinkListRep* const hash_link_list_rep,
                      Node* head) :
        hash_link_list_rep_(hash_link_list_rep), head_(head), node_(nullptr) {
    }

    virtual ~Iterator() {
    }

    // Returns true iff the iterator is positioned at a valid node.
    virtual bool Valid() const {
      return node_ != nullptr;
    }

    // Returns the key at the current position.
    // REQUIRES: Valid()
    virtual const char* key() const {
      assert(Valid());
      return node_->key;
    }

    // Advances to the next position.
    // REQUIRES: Valid()
    virtual void Next() {
      assert(Valid());
      node_ = node_->Next();
    }

    // Advances to the previous position.
    // REQUIRES: Valid()
    virtual void Prev() {
      // Prefix iterator does not support total order.
      // We simply set the iterator to invalid state
      Reset(nullptr);
    }

    // Advance to the first entry with a key >= target
    virtual void Seek(const Slice& internal_key, const char* memtable_key) {
      const char* encoded_key =
          (memtable_key != nullptr) ?
              memtable_key : EncodeKey(&tmp_, internal_key);
      node_ = hash_link_list_rep_->FindGreaterOrEqualInBucket(head_,
                                                              encoded_key);
    }

    // Position at the first entry in collection.
    // Final state of iterator is Valid() iff collection is not empty.
    virtual void SeekToFirst() {
      // Prefix iterator does not support total order.
      // We simply set the iterator to invalid state
      Reset(nullptr);
    }

    // Position at the last entry in collection.
    // Final state of iterator is Valid() iff collection is not empty.
    virtual void SeekToLast() {
      // Prefix iterator does not support total order.
      // We simply set the iterator to invalid state
      Reset(nullptr);
    }

   protected:
    void Reset(Node* head) {
      head_ = head;
      node_ = nullptr;
    }
   private:
    friend class HashLinkListRep;
    const HashLinkListRep* const hash_link_list_rep_;
    Node* head_;
    Node* node_;
    std::string tmp_;       // For passing to EncodeKey

    virtual void SeekToHead() {
      node_ = head_;
    }
  };

  class DynamicIterator : public HashLinkListRep::Iterator {
   public:
    explicit DynamicIterator(HashLinkListRep& memtable_rep)
      : HashLinkListRep::Iterator(&memtable_rep, nullptr),
        memtable_rep_(memtable_rep) {}

    // Advance to the first entry with a key >= target
    virtual void Seek(const Slice& k, const char* memtable_key) {
      auto transformed = memtable_rep_.transform_->Transform(k);
      Reset(memtable_rep_.GetBucket(transformed));
      HashLinkListRep::Iterator::Seek(k, memtable_key);
    }

   private:
    // the underlying memtable
    const HashLinkListRep& memtable_rep_;
  };

  class EmptyIterator : public MemTableRep::Iterator {
    // This is used when there wasn't a bucket. It is cheaper than
    // instantiating an empty bucket over which to iterate.
   public:
    EmptyIterator() { }
    virtual bool Valid() const {
      return false;
    }
    virtual const char* key() const {
      assert(false);
      return nullptr;
    }
    virtual void Next() { }
    virtual void Prev() { }
    virtual void Seek(const Slice& user_key, const char* memtable_key) { }
    virtual void SeekToFirst() { }
    virtual void SeekToLast() { }
   private:
  };
};

HashLinkListRep::HashLinkListRep(MemTableRep::KeyComparator& compare,
                                 Arena* arena, const SliceTransform* transform,
                                 size_t bucket_size)
  : bucket_size_(bucket_size),
    transform_(transform),
    compare_(compare),
    arena_(arena) {
  char* mem = arena_->AllocateAligned(
      sizeof(port::AtomicPointer) * bucket_size);

  buckets_ = new (mem) port::AtomicPointer[bucket_size];

  for (size_t i = 0; i < bucket_size_; ++i) {
    buckets_[i].NoBarrier_Store(nullptr);
  }
}

HashLinkListRep::~HashLinkListRep() {
}

void HashLinkListRep::Insert(const char* key) {
  assert(!Contains(key));
  auto transformed = transform_->Transform(UserKey(key));
  auto& bucket = buckets_[GetHash(transformed)];
  Node* head = static_cast<Node*>(bucket.Acquire_Load());

  if (!head) {
    Node* x = NewNode(key);
    // NoBarrier_SetNext() suffices since we will add a barrier when
    // we publish a pointer to "x" in prev[i].
    x->NoBarrier_SetNext(nullptr);
    bucket.Release_Store(static_cast<void*>(x));
    return;
  }

  Node* cur = head;
  Node* prev = nullptr;
  while (true) {
    if (cur == nullptr) {
      break;
    }
    Node* next = cur->Next();
    // Make sure the lists are sorted.
    // If x points to head_ or next points nullptr, it is trivially satisfied.
    assert((cur == head) || (next == nullptr) ||
           KeyIsAfterNode(next->key, cur));
    if (KeyIsAfterNode(key, cur)) {
      // Keep searching in this list
      prev = cur;
      cur = next;
    } else {
      break;
    }
  }

  // Our data structure does not allow duplicate insertion
  assert(cur == nullptr || !Equal(key, cur->key));

  Node* x = NewNode(key);

  // NoBarrier_SetNext() suffices since we will add a barrier when
  // we publish a pointer to "x" in prev[i].
  x->NoBarrier_SetNext(cur);

  if (prev) {
    prev->SetNext(x);
  } else {
    bucket.Release_Store(static_cast<void*>(x));
  }
}

bool HashLinkListRep::Contains(const char* key) const {
  auto transformed = transform_->Transform(UserKey(key));
  auto bucket = GetBucket(transformed);
  if (bucket == nullptr) {
    return false;
  }
  return BucketContains(bucket, key);
}

size_t HashLinkListRep::ApproximateMemoryUsage() {
  // Memory is always allocated from the arena.
  return 0;
}

MemTableRep::Iterator* HashLinkListRep::GetIterator() {
  auto list = new FullList(compare_, arena_);
  for (size_t i = 0; i < bucket_size_; ++i) {
    auto bucket = GetBucket(i);
    if (bucket != nullptr) {
      Iterator itr(this, bucket);
      for (itr.SeekToHead(); itr.Valid(); itr.Next()) {
        list->Insert(itr.key());
      }
    }
  }
  return new FullListIterator(list);
}

MemTableRep::Iterator* HashLinkListRep::GetPrefixIterator(
  const Slice& prefix) {
  auto bucket = GetBucket(prefix);
  if (bucket == nullptr) {
    return new EmptyIterator();
  }
  return new Iterator(this, bucket);
}

MemTableRep::Iterator* HashLinkListRep::GetIterator(const Slice& slice) {
  return GetPrefixIterator(transform_->Transform(slice));
}

MemTableRep::Iterator* HashLinkListRep::GetDynamicPrefixIterator() {
  return new DynamicIterator(*this);
}

bool HashLinkListRep::BucketContains(Node* head, const Key& key) const {
  Node* x = FindGreaterOrEqualInBucket(head, key);
  return (x != nullptr && Equal(key, x->key));
}

Node* HashLinkListRep::FindGreaterOrEqualInBucket(Node* head,
                                                  const Key& key) const {
  Node* x = head;
  while (true) {
    if (x == nullptr) {
      return x;
    }
    Node* next = x->Next();
    // Make sure the lists are sorted.
    // If x points to head_ or next points nullptr, it is trivially satisfied.
    assert((x == head) || (next == nullptr) || KeyIsAfterNode(next->key, x));
    if (KeyIsAfterNode(key, x)) {
      // Keep searching in this list
      x = next;
    } else {
      break;
    }
  }
  return x;
}

} // anon namespace

MemTableRep* HashLinkListRepFactory::CreateMemTableRep(
    MemTableRep::KeyComparator& compare, Arena* arena) {
  return new HashLinkListRep(compare, arena, transform_, bucket_count_);
}

MemTableRepFactory* NewHashLinkListRepFactory(
    const SliceTransform* transform, size_t bucket_count) {
  return new HashLinkListRepFactory(transform, bucket_count);
}

} // namespace rocksdb
