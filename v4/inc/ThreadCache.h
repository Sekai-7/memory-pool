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
    void deallocate(void* ptr, Span* span);

public:
    ThreadCache(const ThreadCache&) = delete;
    ThreadCache& operator=(const ThreadCache&) = delete;
    ThreadCache(ThreadCache&&) = delete;
    ThreadCache& operator=(ThreadCache&&) = delete;

private:
    void refillFromCentralCache(size_t normalizedSize, size_t index);
    void flushHalfToCentralCache(size_t index);

    ThreadCache() {
        for (auto& bucket : buckets_) {
            bucket.threshold = DEFAULT_THRESHOLD;
        }
    }
    ~ThreadCache();

private:
    struct LocalBucket {
        std::byte* freeList = nullptr;
        size_t freeListSize = 0;
        size_t threshold = DEFAULT_THRESHOLD;
    };

    std::array<LocalBucket, FREE_LIST_SIZE> buckets_ = {};
};

}

#endif
