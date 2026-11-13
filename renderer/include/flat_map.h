/**
 * Sleeping Dogs port (SDNX): native renderer, a flat hash map for 64-bit hash keys.
 *
 * The content-hashed geometry is looked up several times per draw (index buffer, every vertex binding) in a table
 * of ~20-30k entries. std::unordered_map chases a node pointer per lookup (two cache misses, a heap node per entry);
 * here the keys sit in one array probed linearly, the values in a deque so that a pointer returned to a draw stays
 * valid even when the table grows during the same draw. Keys are already hashes (XXH3), used as they are.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace rex::graphics::sdnx {

template <typename V>
class FlatMap64 {
 public:
  V* find(uint64_t key) {
    if (!count_) return nullptr;
    size_t mask = keys_.size() - 1;
    for (size_t i = size_t(key) & mask;; i = (i + 1) & mask) {
      uint8_t s = states_[i];
      if (s == kEmpty) return nullptr;
      if (s == kFull && keys_[i] == key) return &values_[slots_[i]];
    }
  }

  // The value for `key`, default-constructed if new; second is true when it was inserted.
  std::pair<V*, bool> try_emplace(uint64_t key) {
    if ((count_ + tombstones_ + 1) * 2 > keys_.size()) Rehash(keys_.empty() ? 1024 : (count_ + 1) * 4);
    size_t mask = keys_.size() - 1;
    size_t grave = SIZE_MAX;
    for (size_t i = size_t(key) & mask;; i = (i + 1) & mask) {
      uint8_t s = states_[i];
      if (s == kFull) {
        if (keys_[i] == key) return {&values_[slots_[i]], false};
        continue;
      }
      if (s == kTombstone) {
        if (grave == SIZE_MAX) grave = i;
        continue;
      }
      // Empty: the key is absent. Reuse the first tombstone met on the way, if any.
      size_t at = grave != SIZE_MAX ? grave : i;
      if (grave != SIZE_MAX) --tombstones_;
      uint32_t slot = NewSlot();
      keys_[at] = key;
      slots_[at] = slot;
      states_[at] = kFull;
      ++count_;
      return {&values_[slot], true};
    }
  }

  void erase(uint64_t key) {
    if (!count_) return;
    size_t mask = keys_.size() - 1;
    for (size_t i = size_t(key) & mask;; i = (i + 1) & mask) {
      uint8_t s = states_[i];
      if (s == kEmpty) return;
      if (s == kFull && keys_[i] == key) {
        EraseAt(i);
        return;
      }
    }
  }

  // Erases every entry for which f(key, value) returns true.
  template <typename F>
  void erase_if(F f) {
    for (size_t i = 0; i < keys_.size(); ++i) {
      if (states_[i] == kFull && f(keys_[i], values_[slots_[i]])) EraseAt(i);
    }
  }

  size_t size() const { return count_; }

  void clear() {
    keys_.clear();
    slots_.clear();
    states_.clear();
    values_.clear();
    free_.clear();
    count_ = tombstones_ = 0;
  }

 private:
  static constexpr uint8_t kEmpty = 0, kFull = 1, kTombstone = 2;

  uint32_t NewSlot() {
    if (!free_.empty()) {
      uint32_t slot = free_.back();
      free_.pop_back();
      values_[slot] = V();
      return slot;
    }
    values_.emplace_back();
    return uint32_t(values_.size() - 1);
  }

  void EraseAt(size_t i) {
    free_.push_back(slots_[i]);
    states_[i] = kTombstone;
    --count_;
    ++tombstones_;
  }

  void Rehash(size_t wanted) {
    size_t capacity = 1024;
    while (capacity < wanted) capacity <<= 1;
    std::vector<uint64_t> keys(capacity);
    std::vector<uint32_t> slots(capacity);
    std::vector<uint8_t> states(capacity, kEmpty);
    size_t mask = capacity - 1;
    for (size_t j = 0; j < keys_.size(); ++j) {
      if (states_[j] != kFull) continue;
      size_t i = size_t(keys_[j]) & mask;
      while (states[i] == kFull) i = (i + 1) & mask;
      keys[i] = keys_[j];
      slots[i] = slots_[j];
      states[i] = kFull;
    }
    keys_.swap(keys);
    slots_.swap(slots);
    states_.swap(states);
    tombstones_ = 0;
  }

  std::vector<uint64_t> keys_;
  std::vector<uint32_t> slots_;
  std::vector<uint8_t> states_;
  std::deque<V> values_;
  std::vector<uint32_t> free_;
  size_t count_ = 0, tombstones_ = 0;
};

}  // namespace rex::graphics::sdnx
