#ifndef _UTIL_H_
#define _UTIL_H_

#include <sys/mman.h>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>

namespace memorypool {

constexpr size_t MAX_PAGES = 128;

constexpr size_t ALIGNLEN = sizeof(void*);

constexpr size_t DEFAULT_THRESHOLD = 8;

constexpr size_t FREE_LIST_SIZE = 1024;

constexpr size_t PAGE_SIZE = 4096;

constexpr size_t SPAN_SIZE = 8;

constexpr size_t PAGE_SHIFT = 12;

constexpr size_t BITS_PER_LEVEL = 12;

constexpr size_t LEVEL_LENGTH = 1ULL << BITS_PER_LEVEL;

inline size_t align(size_t size) {
    return (size + ALIGNLEN - 1) & ~(ALIGNLEN - 1);
}

inline uint16_t getListIndex(size_t size) {
    // return align(size) / ALIGNLEN - 1;
    auto alignSize = align(size);
    if (alignSize == 8) {
        return 0;
    } else if (alignSize == 16) {
        return 1;
    } else if (alignSize == 32) {
        return 2;
    } else if (alignSize == 64) {
        return 3;
    } else if (alignSize == 128) {
        return 4;
    } else if (alignSize == 256) {
        return 5;
    } else if (alignSize == 512) {
        return 6;
    } else if (alignSize == 1024) {
        return 7;
    }
    return FREE_LIST_SIZE;
}

template<typename T, size_t ChunkSize = 64 * 1024>
class MetaDataAllocator {
public:
    static MetaDataAllocator<T, ChunkSize>& getInstance() {
        static MetaDataAllocator<T, ChunkSize> instance;
        return instance;
    }
    T* allocate() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (freeList_ != nullptr) {
            std::byte* ret = freeList_;
            freeList_ = *(reinterpret_cast<std::byte**>(freeList_));
            return new(ret) T();
        }

        static constexpr size_t allocSize = (sizeof(T) + alignof(T) - 1) & ~(alignof(T) - 1);

        if (currentChunk_ == nullptr || (chunkOffSet_ + allocSize) > ChunkSize) {
            auto ret = allocateNewChunk();
            if (ret == false) {
                return nullptr;
            }
        }

        std::byte* ptr = currentChunk_ + chunkOffSet_;
        chunkOffSet_ += allocSize;

        return new(ptr) T();
    }

    void deallocate(T* ptr) {
        if (ptr == nullptr) {
            return;
        }

        ptr->~T();

        std::lock_guard<std::mutex> lock(mutex_);

        *reinterpret_cast<std::byte**>(ptr) = freeList_;

        freeList_ = reinterpret_cast<std::byte*>(ptr);
    }

public:
    MetaDataAllocator(const MetaDataAllocator&) = delete;
    MetaDataAllocator& operator=(const MetaDataAllocator&) = delete;
    MetaDataAllocator(MetaDataAllocator&&) = delete;
    MetaDataAllocator& operator=(MetaDataAllocator&&) = delete;

private:
    bool allocateNewChunk() {
        void* memory = mmap(nullptr, ChunkSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (memory == MAP_FAILED) {
            return false;
        }

        currentChunk_ = static_cast<std::byte*>(memory);
        chunkOffSet_ = 0;
        return true;
    }

    MetaDataAllocator() = default;
    ~MetaDataAllocator() = default;

private:
    std::byte* currentChunk_ = nullptr;
    size_t chunkOffSet_ = 0;

    std::byte* freeList_ = nullptr;

    std::mutex mutex_;
};

class Span {
public:
    void* ptr{nullptr};         // Span 管理的起始物理页地址
    size_t pageCount{0};        // Span 包含的连续物理页数量
    size_t objSize{0};          // 切分的目标对象大小 (Size Class)
    
    size_t useCount{0};         // 核心状态：已分配给 ThreadCache 的对象数量
    std::byte* freeList{nullptr}; // 核心状态：内部尚未分配（或已归还）的空闲对象单向链表
    
    // 侵入式双向链表指针
    Span* prev{nullptr};
    Span* next{nullptr};
    // void* ptr;
    // size_t blockSize;
    // size_t pageSize;
    // size_t totalSize;
    // std::atomic<int> freeSize; 
};

class SpanList {
public:
    SpanList() {
        // 哑节点 (Dummy Node) 初始化为循环指向自己
        head_.prev = &head_;
        head_.next = &head_;
    }

    // 禁用拷贝与移动
    SpanList(const SpanList&) = delete;
    SpanList& operator=(const SpanList&) = delete;

    // 头插法
    void pushFront(Span* span) {
        insert(&head_, span);
    }

    // O(1) 移除指定 Span
    void remove(Span* span) {
        span->prev->next = span->next;
        span->next->prev = span->prev;
        span->prev = nullptr;
        span->next = nullptr;
    }

    // 检查链表是否为空
    bool empty() const {
        return head_.next == &head_;
    }

    // 获取第一个有效的 Span
    Span* front() const {
        return empty() ? nullptr : head_.next;
    }

private:
    void insert(Span* pos, Span* span) {
        span->next = pos->next;
        span->prev = pos;
        pos->next->prev = span;
        pos->next = span;
    }

private:
    Span head_; // 哑节点，规避边界条件检查的分支预测惩罚
};

class LeafNode {
public:
    std::atomic<Span*> spans[LEVEL_LENGTH];
};

class Node {
public:
    std::atomic<LeafNode*> leaves[LEVEL_LENGTH];
};

class RadixTreePageMap {
public:
    static RadixTreePageMap& getInstance() {
        static RadixTreePageMap instance;
        return instance;
    }

    void setSpan(uintptr_t, Span*);
    Span* getSpan(uintptr_t);

private:
    RadixTreePageMap() {
        // 初始化根数组为空
        for (size_t i = 0; i < LEVEL_LENGTH; ++i) {
            root_[i].store(nullptr, std::memory_order_relaxed);
        }
    }

private:
    std::atomic<Node*> root_[LEVEL_LENGTH];
};

using SpanAllocator = MetaDataAllocator<Span, 2 * 1024 * 1024>;

using NodeAllocator = MetaDataAllocator<Node, 1024 * 1024>;

using LeafNodeAllocator = MetaDataAllocator<LeafNode, 1024 * 1024>;

}

#endif