// From Trials HD Recompiled by snufflyyy (MIT, see THIRD_PARTY_NOTICES.md): src/flat_hash_map.h.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <utility>
#include <vector>

namespace trials_hd {

template <typename Key, typename Value, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
class FlatHashMap {
 public:
  using key_type = Key;
  using mapped_type = Value;
  using value_type = std::pair<Key, Value>;

 private:
  struct Slot {
    value_type item{};
    uint32_t dib = 0; // Distance from initial bucket. 0 means empty (we store dib+1 when occupied).
  };

  std::vector<Slot> slots_;
  size_t size_ = 0;
  size_t mask_ = 0;
  Hash hasher_{};
  KeyEqual key_equal_{};

 public:
  class iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::pair<Key, Value>;
    using difference_type = std::ptrdiff_t;
    using pointer = value_type*;
    using reference = value_type&;

    iterator() : map_(nullptr), index_(0) {}
    iterator(FlatHashMap* map, size_t index) : map_(map), index_(index) {
      advance_to_occupied();
    }

    reference operator*() const { return map_->slots_[index_].item; }
    pointer operator->() const { return &map_->slots_[index_].item; }

    iterator& operator++() {
      ++index_;
      advance_to_occupied();
      return *this;
    }

    iterator operator++(int) {
      iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    bool operator==(const iterator& other) const {
      return map_ == other.map_ && index_ == other.index_;
    }
    bool operator!=(const iterator& other) const { return !(*this == other); }

    size_t index() const { return index_; }

   private:
    void advance_to_occupied() {
      if (!map_) return;
      while (index_ < map_->slots_.size() && map_->slots_[index_].dib == 0) {
        ++index_;
      }
    }

    FlatHashMap* map_;
    size_t index_;
  };

  class const_iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = const std::pair<Key, Value>;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type*;
    using reference = const value_type&;

    const_iterator() : map_(nullptr), index_(0) {}
    const_iterator(const FlatHashMap* map, size_t index) : map_(map), index_(index) {
      advance_to_occupied();
    }
    const_iterator(const iterator& it) : map_(it.map_), index_(it.index_) {}

    reference operator*() const { return map_->slots_[index_].item; }
    pointer operator->() const { return &map_->slots_[index_].item; }

    const_iterator& operator++() {
      ++index_;
      advance_to_occupied();
      return *this;
    }

    const_iterator operator++(int) {
      const_iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    bool operator==(const const_iterator& other) const {
      return map_ == other.map_ && index_ == other.index_;
    }
    bool operator!=(const const_iterator& other) const { return !(*this == other); }

   private:
    void advance_to_occupied() {
      if (!map_) return;
      while (index_ < map_->slots_.size() && map_->slots_[index_].dib == 0) {
        ++index_;
      }
    }

    const FlatHashMap* map_;
    size_t index_;
  };

  FlatHashMap() { init(8); }
  explicit FlatHashMap(size_t initial_capacity) {
    size_t cap = 8;
    while (cap < initial_capacity) cap <<= 1;
    init(cap);
  }

  iterator begin() { return iterator(this, 0); }
  iterator end() { return iterator(this, slots_.size()); }
  const_iterator begin() const { return const_iterator(this, 0); }
  const_iterator end() const { return const_iterator(this, slots_.size()); }

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  void clear() {
    slots_.clear();
    size_ = 0;
    mask_ = 0;
    init(8);
  }

  iterator find(const Key& key) {
    if (size_ == 0) return end();
    size_t idx = hasher_(key) & mask_;
    uint32_t dib = 1;
    while (true) {
      Slot& slot = slots_[idx];
      if (slot.dib == 0 || dib > slot.dib) return end();
      if (key_equal_(slot.item.first, key)) return iterator(this, idx);
      idx = (idx + 1) & mask_;
      ++dib;
    }
  }

  const_iterator find(const Key& key) const {
    if (size_ == 0) return end();
    size_t idx = hasher_(key) & mask_;
    uint32_t dib = 1;
    while (true) {
      const Slot& slot = slots_[idx];
      if (slot.dib == 0 || dib > slot.dib) return end();
      if (key_equal_(slot.item.first, key)) return const_iterator(this, idx);
      idx = (idx + 1) & mask_;
      ++dib;
    }
  }

  bool contains(const Key& key) const { return find(key) != end(); }

  Value& operator[](const Key& key) {
    if (size_ + 1 > slots_.size() * 0.75) {
      rehash(slots_.size() * 2);
    }
    size_t idx = hasher_(key) & mask_;
    uint32_t dib = 1;
    Slot current{value_type(key, Value{}), 0};
    bool inserting = false;

    while (true) {
      Slot& slot = slots_[idx];
      if (slot.dib == 0) {
        if (!inserting) {
          slot.item.first = key;
          slot.item.second = Value{};
          slot.dib = dib;
          ++size_;
          return slot.item.second;
        } else {
          current.dib = dib;
          slot = std::move(current);
          ++size_;
          return find(key)->second;
        }
      }
      if (!inserting && key_equal_(slot.item.first, key)) {
        return slot.item.second;
      }
      if (dib > slot.dib) {
        if (!inserting) {
          current.dib = dib;
          std::swap(current, slot);
          inserting = true;
        } else {
          current.dib = dib;
          std::swap(current, slot);
          dib = current.dib;
        }
      }
      idx = (idx + 1) & mask_;
      ++dib;
    }
  }

  std::pair<iterator, bool> emplace(const Key& key, const Value& val) {
    return insert_or_assign(key, val);
  }

  std::pair<iterator, bool> insert_or_assign(const Key& key, const Value& val) {
    if (size_ + 1 > slots_.size() * 0.75) {
      rehash(slots_.size() * 2);
    }
    size_t idx = hasher_(key) & mask_;
    uint32_t dib = 1;
    Slot current{value_type(key, val), 0};
    bool inserting = false;
    size_t inserted_idx = 0;

    while (true) {
      Slot& slot = slots_[idx];
      if (slot.dib == 0) {
        current.dib = dib;
        slot = std::move(current);
        ++size_;
        return {iterator(this, inserting ? find(key).index() : idx), true};
      }
      if (!inserting && key_equal_(slot.item.first, key)) {
        slot.item.second = val;
        return {iterator(this, idx), false};
      }
      if (dib > slot.dib) {
        if (!inserting) {
          inserted_idx = idx;
          current.dib = dib;
          std::swap(current, slot);
          inserting = true;
        } else {
          current.dib = dib;
          std::swap(current, slot);
          dib = current.dib;
        }
      }
      idx = (idx + 1) & mask_;
      ++dib;
    }
  }

  size_t erase(const Key& key) {
    auto it = find(key);
    if (it == end()) return 0;
    erase(it);
    return 1;
  }

  iterator erase(iterator it) {
    if (it == end()) return end();
    size_t idx = it.index();
    // Backward shift deletion for Robin Hood hashing
    size_t next_idx = (idx + 1) & mask_;
    while (slots_[next_idx].dib > 1) {
      slots_[idx] = std::move(slots_[next_idx]);
      --slots_[idx].dib;
      idx = next_idx;
      next_idx = (next_idx + 1) & mask_;
    }
    slots_[idx] = Slot{};
    --size_;
    return iterator(this, it.index());
  }

 private:
  void init(size_t cap) {
    slots_.clear();
    slots_.resize(cap);
    mask_ = cap - 1;
  }

  void rehash(size_t new_cap) {
    std::vector<Slot> old_slots = std::move(slots_);
    init(new_cap);
    size_ = 0;
    for (auto& s : old_slots) {
      if (s.dib != 0) {
        insert_or_assign(s.item.first, std::move(s.item.second));
      }
    }
  }
};

template <typename Key, typename Value, typename Hash, typename KeyEqual, typename Pred>
size_t erase_if(FlatHashMap<Key, Value, Hash, KeyEqual>& map, Pred pred) {
  size_t count = 0;
  for (auto it = map.begin(); it != map.end(); ) {
    if (pred(*it)) {
      it = map.erase(it);
      ++count;
    } else {
      ++it;
    }
  }
  return count;
}

}  // namespace trials_hd

namespace std {
template <typename Key, typename Value, typename Hash, typename KeyEqual, typename Pred>
size_t erase_if(trials_hd::FlatHashMap<Key, Value, Hash, KeyEqual>& map, Pred pred) {
  return trials_hd::erase_if(map, pred);
}
}  // namespace std
