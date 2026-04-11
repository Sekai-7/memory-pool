#ifndef _THREAD_CACHE_H_
#define _THREAD_CACHE_H_

#include "util.h"

#include <array>
#include <cstddef>

namespace memorypool {

class ThreadCache {
public:
    static ThreadCache& getInstance() {
        static thread_local ThreadCache instance;
        return instance;
    }

    void* allocate(size_t normalizedSize);
    void deallocate(void* ptr);

public:
    ThreadCache(const ThreadCache&) = delete;
    ThreadCache& operator=(const ThreadCache&) = delete;
    ThreadCache(ThreadCache&&) = delete;
    ThreadCache& operator=(ThreadCache&&) = delete;

    ThreadCache() = default;
    ~ThreadCache();

private:
    std::array<std::byte*, FREE_LIST_SIZE> freeList_ = {};
    std::array<size_t, FREE_LIST_SIZE> freeListSize_ = {};
};

}

#endif
