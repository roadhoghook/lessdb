/**
 * Copyright (C) 2016, Wu Tao. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include <functional>
#include <iterator>
#include <random>
#include <cassert>
#include <atomic>

#include "Disallowcopying.h"

namespace lessdb {

// SkipLists are a probabilistic balanced data structure.
// This implementation is based on the paper
// [Skip Lists: A Probabilistic Alternative to Balanced Trees] by William Pugh.
//
// NOTE: Allocated nodes are never deleted until the SkipList is destroyed.

// Compare takes two element keys as arguments and returns a bool.
// The expression comp(a,b), where comp is an object of this type
// and a and b are key values, shall return true if a is considered to go
// before b in the strict weak ordering the function defines.

// SkipList is designed to be used in the situations where only single writer is
// running, with multiple readers reading concurrently.
//
template<class T, class Compare = std::less<T> >
class SkipList {
  __DISALLOW_COPYING__(SkipList);

  struct Node;

  static const unsigned MaxLevel = 12;

 public:

  struct Iterator {
   private:
    // intentionally copyable
    const Node *node_;

   public:

    Iterator() :
        node_(nullptr) {
    }

    explicit Iterator(const Node *n) :
        node_(n) {
    }

    // prefix
    Iterator &operator++() {
      node_ = node_->Next(0);
      return (*this);
    }

    // postfix
    Iterator operator++(int) {
      Iterator ret(node_);
      node_ = node_->Next(0);
      return ret;
    }

    const T &operator*() const {
      return node_->key;
    }

    const T *operator->() const {
      return &(node_->key);
    }

    friend bool operator==(const Iterator &x, const Iterator &y) noexcept {
      return x.node_ == y.node_;
    }

    friend bool operator!=(const Iterator &x, const Iterator &y) noexcept {
      return x.node_ != y.node_;
    }
  };

 public:

  explicit SkipList(const Compare &compare = Compare());

  ~SkipList() noexcept;

  // Insert does not require external synchronization with
  // only single writer running, it's safe with concurrent
  // readers.
  Iterator Insert(const T &key);

  bool Empty() const noexcept {
    return Begin() == End();
  }

  // Returns an iterator pointing to the first element
  // in the range [first,last) which does not compare less than val.
  Iterator LowerBound(const T &key) const;

  // Returns an iterator pointing to the first element
  // in the range [first,last) which compares greater than val.
  Iterator UpperBound(const T &key) const;

  Iterator Find(const T &key) const;

//  bool Size() const noexcept;

  const Iterator Begin() const noexcept {
    return Iterator(head_->Next(0));
  }

  const Iterator End() const noexcept {
    return Iterator(nullptr);
  }

 private:

  int getHeight() const;

  int randomLevel();

  int random();

  bool keyEq(const T &k1, const T &k2) const {
    // k1 <= k2 && k2 >= k1 ----> !(k1 > k2) && !(k1 < k2)
    return !compare_(k2, k1) && !compare_(k1, k2);
  }

 private:
  Node *const head_;

  std::atomic<int> height_;
  Compare compare_;

  // random number ranges in [0, 3]
  std::default_random_engine gen_;
  std::uniform_int_distribution<int> distrib_;
};

template<class T, class Compare>
struct SkipList<T, Compare>::Node {
  const T key;
  std::atomic<Node *> *forward;

  Node(const T &k) :
      key(k) {
  }

  ~Node() {
    delete forward;
  }

  Node *Next(int level) const {
    // observe an fully initialized version of the pointer.
    return forward[level].load(std::memory_order_acquire);
  }

  void SetNext(Node *next, int level) {
    forward[level].store(next, std::memory_order_release);
  }

  // NoSync means there's no synchronization on the
  // atomic operation, and therefore no guarantee or
  // protection is on the ordering and visibility of
  // data in concurrent situation.
  Node *NoSyncNext(int level) const {
    return forward[level].load(std::memory_order_relaxed);
  }

  void NoSyncSetNext(Node *next, int level) {
    forward[level].store(next, std::memory_order_relaxed);
  }

  static Node *Make(const T &key, int height) {
    Node *ret = new Node(key);
    ret->forward = new std::atomic<Node *>[height];
    return ret;
  }
};

template<class T, class Compare>
inline int SkipList<T, Compare>::randomLevel() {
  int height = 1;
  while (height < MaxLevel && random() == 0) height++;
  return height;
}

template<class T, class Compare>
inline int SkipList<T, Compare>::random() {
  return distrib_(gen_);
}

template<class T, class Compare>
typename SkipList<T, Compare>::Iterator
SkipList<T, Compare>::Insert(const T &key) {
  Node *update[MaxLevel];
  Node *x = head_;

  for (int level = getHeight() - 1; level >= 0; level--) {
    Node *next = x->Next(level);
    while (next != nullptr && compare_(next->key, key)) {
      x = next;
      next = x->Next(level);
    }
    // next == nullptr or x->key < key <= next->key
    update[level] = x;
  }

  x = x->Next(0); // x->key >= key or x == nullptr
  if (x != nullptr && keyEq(key, x->key)) {
    return Iterator(x);
  }

  assert(Find(key) == End());

  int level = randomLevel();
  if (level > getHeight()) {
    for (int i = getHeight(); i < level; i++) {
      update[i] = head_;
    }

    // It is ok to mutate height_ without any synchronization
    // with concurrent readers.  A concurrent reader that observes
    // the new value of height_ will see either the old value of
    // new level pointers from head_ (NULL), or a new value set in
    // the loop below.  In the former case the reader will
    // immediately drop to the next level since NULL sorts after all
    // keys.  In the latter case the reader will use the new node.
    height_.store(level, std::memory_order_relaxed);
  }

  x = Node::Make(key, level);

  // Intentionally repeat from bottom to top.
  for (int i = 0; i < level; i++) {
    // It's not necessary to do synchronization here, since x is
    // ok to be a dangling node until we linked update[i] to it.
    x->NoSyncSetNext(update[i]->NoSyncNext(i), i);
    update[i]->SetNext(x, i);
  }

  return Iterator(x);
}

template<class T, class Compare>
inline int SkipList<T, Compare>::getHeight() const {
  // It's ok to load height_ without synchronization.
  return height_.load(std::memory_order_relaxed);
}

template<class T, class Compare>
typename SkipList<T, Compare>::Iterator
SkipList<T, Compare>::LowerBound(const T &key) const {
  Node *x = head_;
  int height = getHeight() - 1;

  for (int level = height; level >= 0; level--) {
    Node *next = x->Next(level);
    while (next != nullptr && compare_(next->key, key)) {
      x = next;
      next = x->Next(level);
    }
    // x->key < key <= next->key
    if (level == 0)
      return Iterator(next);
  }
  assert(0);
}

template<class T, class Compare>
typename SkipList<T, Compare>::Iterator
SkipList<T, Compare>::UpperBound(const T &key) const {
  Node *x = head_;
  int height = getHeight() - 1;

  for (int level = height; level >= 0; level--) {
    Node *next = x->Next(level);
    // key <= next->key ==> !(key > next->key)
    while (next != nullptr && !compare_(key, next->key)) {
      x = next;
      next = x->Next(level);
    }
    // x->key <= key < next->key
    if (level == 0)
      return Iterator(next);
  }
  assert(0);
}

template<class T, class Compare>
SkipList<T, Compare>::~SkipList() noexcept {
  Node *x = head_, *prev;
  while (x != nullptr) {
    prev = x;
    x = x->Next(0);
    delete prev;
  }
}

template<class T, class Compare>
SkipList<T, Compare>::SkipList(const Compare &compare) :
    distrib_(0, 3),
    head_(Node::Make(0, MaxLevel)),
    height_(1),
    compare_(compare) {
  for (int i = 0; i < MaxLevel; i++)
    head_->SetNext(nullptr, i);
}

template<class T, class Compare>
typename SkipList<T, Compare>::Iterator
SkipList<T, Compare>::Find(const T &key) const {
  Node *x = head_;

  for (int level = getHeight() - 1; level >= 0; level--) {
    Node *next = x->Next(level);
    while (next != nullptr && compare_(next->key, key)) {
      x = next;
      next = x->Next(level);
    }
    // next == nullptr or x->key < key <= next->key
  }

  x = x->Next(0); // x->key >= key or x == nullptr
  if (x != nullptr && compare_(key, x->key))
    return End();
  return Iterator(x);
}

} // namespace lessdb