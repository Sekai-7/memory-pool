#ifndef _THREAD_CACHE_H_
#define _THREAD_CACHE_H_

#include <array>
#include <cstddef>

namespace memorypool {

constexpr size_t FREE_LIST_SIZE = 1024;

class ThreadCache {
public:
    static ThreadCache& getInstance() {
        static thread_local ThreadCache instance;
        return instance;
    }

    void* allocate(size_t size);
    void deallocate(void* ptr, size_t size);

private:
    ThreadCache() = default;
    ~ThreadCache();
    ThreadCache(const ThreadCache&) = delete;
    ThreadCache& operator=(const ThreadCache&) = delete;
    ThreadCache(ThreadCache&&) = delete;
    ThreadCache& operator=(ThreadCache&&) = delete;

private:
    std::array<std::byte*, FREE_LIST_SIZE> freeList = {};
    std::array<size_t, FREE_LIST_SIZE> freeListSize = {};
};

}

#endif