#ifndef _CENTRAL_CACHE_H_
#define _CENTRAL_CACHE_H_

#include "util.h"

#include <sys/mman.h>
#include <cstddef>
#include <array>
#include <memory>
#include <cstdint>
#include <atomic>
#include <new>
#include <mutex>

namespace memorypool {

constexpr size_t PAGE_SHIFT = 12;
constexpr size_t BITS_PER_LEVEL = 12;
constexpr size_t LEVEL_LENGTH = 1ULL << BITS_PER_LEVEL;

class MetaDataAllocator {
public:
    static MetaDataAllocator& getInstance() {
        static MetaDataAllocator instance;
        return instance;
    }

    // 模板分配接口
    template<typename T>
    T* allocateNode() {
        size_t alloc_size = sizeof(T);
        
        std::lock_guard<std::mutex> lock(meta_mutex_);
        
        // 检查当前 chunk 是否还有足够空间
        if (current_chunk_ == nullptr || (chunk_offset_ + alloc_size > CHUNK_SIZE)) {
            allocateNewChunk();
        }

        // 线性分配 (Bump-pointer allocation)
        void* ptr = current_chunk_ + chunk_offset_;
        chunk_offset_ += alloc_size;

        // 使用 placement new 触发构造函数（如果是原子数组等需要初始化的结构）
        return new(ptr) T(); 
    }

private:
    MetaDataAllocator() = default;
    ~MetaDataAllocator() = default; // 永不析构，交由进程退出时 OS 回收

    void allocateNewChunk() {
        // 向系统直接申请大块内存 (例如 2MB)，规避 malloc
        // PROT_READ | PROT_WRITE: 读写权限
        // MAP_PRIVATE | MAP_ANONYMOUS: 私有匿名映射（不基于文件）
        void* memory = mmap(nullptr, CHUNK_SIZE, 
                            PROT_READ | PROT_WRITE, 
                            MAP_PRIVATE | MAP_ANONYMOUS, 
                            -1, 0);
                            
        if (memory == MAP_FAILED) {
            throw std::bad_alloc(); // 极端物理内存耗尽情况
        }

        current_chunk_ = static_cast<char*>(memory);
        chunk_offset_ = 0;
    }

private:
    // 每次向系统申请 2MB 内存（与 Transparent Huge Page 亲和性较好）
    static constexpr size_t CHUNK_SIZE = 2 * 1024 * 1024; 
    
    char* current_chunk_{nullptr};
    size_t chunk_offset_{0};
    std::mutex meta_mutex_; // 分配节点是罕见操作（冷路径），使用 mutex 保护即可
};

class Span {
public:
    void* ptr;
    size_t size;
    std::atomic<int> freeSize; 
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
public:
    RadixTreePageMap() {
        // 初始化根数组为空
        for (size_t i = 0; i < LEVEL_LENGTH; ++i) {
            root_[i].store(nullptr, std::memory_order_relaxed);
        }
    }
    // ... 方法见下文

    void push(uintptr_t, Span*);
    Span* getSpan(uintptr_t);

private:
    std::atomic<Node*> root_[LEVEL_LENGTH];
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

    RadixTreePageMap radixTreePageMap_;
};

}

#endif