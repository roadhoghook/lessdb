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
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "Block.h"
#include "TableFormat.h"
#include "Coding.h"
#include "DataView.h"
#include "Comparator.h"

namespace lessdb {

Block::Block(const BlockContent &content, const Comparator *comp)
    : data_(content.data.RawData()), size_(content.data.Len()), comp_(comp) {
  num_restart_ = ConstDataView(data_ + size_ - 4).ReadNum<uint32_t>();
  assert(size_ >= 4 * (num_restart_ + 1));
  data_end_ = data_ + size_ - 4 * (num_restart_ + 1);
}

Block::ConstIterator Block::find(const Slice &target) const {
  auto it = lower_bound(target);
  return comp_->Compare(it.Key(), target) != 0 ? end() : it;
}

Block::ConstIterator Block::begin() const {
  return ConstIterator(data_, this, 0);
}

Block::ConstIterator Block::end() const {
  // TODO: restart_pos of end iterator.
  return ConstIterator(data_end_, this, 0);
}

uint32_t Block::restartPoint(int id) const {
  assert(id <= num_restart_);
  uint32_t r = ConstDataView(data_)
                   .ReadNum<uint32_t>(size_ - 4 * (num_restart_ - id + 1));
  assert(r <= data_end_ - data_);
  return r;
}

Slice Block::keyAtRestartPoint(int id) const {
  uint32_t shared, unshared, value_len;
  uint32_t pos = restartPoint(id);

  Slice buf(data_ + pos, data_end_ - data_ - pos);
  coding::GetVar32(&buf, &shared);
  assert(shared == 0);  // no shared bytes at restart point
  coding::GetVar32(&buf, &unshared);
  coding::GetVar32(&buf, &value_len);
  return Slice(buf.RawData(), unshared);
}

Block::ConstIterator Block::lower_bound(const Slice &target) const {
  // Binary search in restart array to find the lastest restart point
  // with a key < target

  // in range [0, num_restart)
  int lb = 0, rb = num_restart_, mid = lb;
  while (rb - lb > 1) {
    mid = (lb + rb) / 2;
    if (comp_->Compare(keyAtRestartPoint(mid), target) >= 0) {
      rb = mid;
    } else {
      lb = mid;
    }
  }

  if (comp_->Compare(keyAtRestartPoint(lb), target) > 0) {
    assert(lb == 0 && rb == lb + 1);
    return end();
  }

  // Searches from the restart point.

  uint32_t pos = restartPoint(lb);
  auto it = ConstIterator(data_ + pos, this, pos);
  for (; it != end(); it++) {
    if (comp_->Compare(it.Key(), target) >= 0)
      break;
  }
  return it;
}

// After construction, buf_ must points at the start of key_delta.
// @param p points at the start of the entry.
BlockConstIterator::BlockConstIterator(const char *p, const Block *block,
                                       uint32_t restart)
    : last_key_(nullptr), restart_pos_(restart), block_(block) {
  init(p);
}

// lazy construction of key, concatenate the shared part and unshared part only
// when necessary.
Slice BlockConstIterator::Key() const {
  // must not an end iterator.
  assert(buf_ != block_->data_end_);
  if (!key_.empty())
    return key_;

  if (!last_key_) {
    auto it =
        BlockConstIterator(block_->data_ + restart_pos_, block_, restart_pos_);
    while (it.buf_ < buf_)
      it++;
    assert(it.buf_ == buf_);
  }

  key_.reserve(shared_ + unshared_);
  key_.append(last_key_, shared_);
  key_.append(buf_, unshared_);
  return key_;
}

Slice BlockConstIterator::Value() const {
  return Slice(buf_ + unshared_, value_len_);
}

void BlockConstIterator::increment() {
  Slice buf(buf_, buf_len_);
  last_key_ = buf_;
  buf.Skip(unshared_);
  buf.Skip(value_len_);
  init(buf.RawData());
  key_.clear();
}

bool BlockConstIterator::equal(const Block::ConstIterator &other) const {
  return buf_ == other.buf_ && buf_len_ == other.buf_len_;
}

void BlockConstIterator::init(const char *p) {
  size_t len = (block_->data_end_ - p);
  assert(len >= 0);
  if (len > 0) {
    Slice buf(p, len);
    try {
      coding::GetVar32(&buf, &shared_);
      coding::GetVar32(&buf, &unshared_);
      coding::GetVar32(&buf, &value_len_);
    } catch (std::exception &e) {
      stat_ = Status::Corruption("BlockConstIterator::init(): ")
              << std::string(e.what());
      return;
    }

    // buf is now pointed at key_delta
    buf_ = buf.RawData();
    buf_len_ = buf.Len();

    // restart_pos doesn't have to be exactly pointing at a restart point, if
    // the unshared is 0, then a traversal can start from here.
    if (unshared_ == 0) {
      // restart position is the start of the entry
      restart_pos_ = static_cast<uint32_t>(p - block_->data_);
    }
  } else {
    // end iterator, initiate for equality comparison.
    buf_ = p;
    buf_len_ = 0;
  }
}

}  // namespace lessdb
