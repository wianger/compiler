#pragma once
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sysy {
template<class T> class NodeArena;

// A stable, typed node identity. Handles do not own subtrees: one compilation
// owns the arenas, so deleting a deep expression never recurses down its tree.
template<class T> struct NodeRef {
  NodeArena<T> *arena = nullptr;
  uint32_t index = UINT32_MAX;
  T *get() const { return arena ? &arena->at(index) : nullptr; }
  T *operator->() const { return get(); }
  T &operator*() const { return *get(); }
  explicit operator bool() const { return arena != nullptr; }
  NodeRef &operator=(std::nullptr_t) { arena = nullptr; index = UINT32_MAX; return *this; }
  void reset() { *this = nullptr; }
};

template<class T> class NodeArena {
  static constexpr size_t blockSize = 128;
  struct Block { alignas(T) unsigned char data[sizeof(T)*blockSize]; };
  std::vector<std::unique_ptr<Block>> blocks;
  uint32_t count = 0;
public:
  NodeArena() = default;
  NodeArena(const NodeArena &) = delete;
  NodeArena &operator=(const NodeArena &) = delete;
  ~NodeArena() { for (uint32_t n = 0; n < count; ++n) at(n).~T(); }
  T &at(uint32_t id) const {
    if (id >= count) throw std::logic_error("arena node ID out of range");
    return *std::launder(reinterpret_cast<T *>(blocks[id/blockSize]->data + sizeof(T)*(id%blockSize)));
  }
  template<class... Args> NodeRef<T> make(Args &&...args) {
    if (count == UINT32_MAX) throw std::overflow_error("node arena exceeds 32-bit IDs");
    if (count%blockSize == 0) blocks.push_back(std::unique_ptr<Block>(new Block));
    auto storage = blocks[count/blockSize]->data + sizeof(T)*(count%blockSize);
    new (storage) T(std::forward<Args>(args)...);
    return {this, count++};
  }
  size_t size() const { return count; }
};
} // namespace sysy
