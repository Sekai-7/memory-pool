#ifndef _CENTRAL_CACHE_H_
#define _CENTRAL_CACHE_H_

#include "util.h"

#include <cstddef>
#include <span>
#include <memory>

namespace memorypool {

constexpr size_t PAGE_SHIFT = 12;
constexpr size_t BITS_PER_LEVEL = 12;
constexpr size_t LEVEL_LENGTH = 1ULL << BITS_PER_LEVEL;

class Span {

};

class LeafNode {
public:
    std::atomic<Span*> spans[LEVEL_LENGTH];
};

// 内部节点 (L2)
class Node {
public:
    std::atomic<LeafNode*> leaves[LEVEL_LENGTH];
};

// 根节点 (L1)
class RadixTreePageMap {
private:
    std::atomic<Node*> root_[LEVEL_LENGTH];

public:
    RadixTreePageMap() {
        // 初始化根数组为空
        for (size_t i = 0; i < LEVEL_LENGTH; ++i) {
            root_[i].store(nullptr, std::memory_order_relaxed);
        }
    }
    // ... 方法见下文
};

class CentralCache {
public:
    static CentralCache& getInstance() {
        static CentralCache instance;
        return instance;
    }

    std::byte* allocate(size_t, size_t);
    void deallocate(std::byte*, size_t);

public:
    CentralCache(const CentralCache&) = delete;
    CentralCache& operator=(const CentralCache&) = delete;
    CentralCache(CentralCache&&) = delete;
    CentralCache& operator=(CentralCache&&) = delete;

private:
    CentralCache() = default;
    ~CentralCache();

private:
    std::array<std::byte*, FREE_LIST_SIZE> centralFreeList_;
    std::array<size_t, FREE_LIST_SIZE> centralFreeListSize_;
    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;
};

}

#endif