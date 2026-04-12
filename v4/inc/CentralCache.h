#ifndef _CENTRAL_CACHE_H_
#define _CENTRAL_CACHE_H_

#include "util.h"

#include <cstddef>
#include <array>
#include <cstdint>
#include <atomic>

namespace memorypool {

class CentralCache {
public:
    static constexpr size_t kCacheLineSize = 64;

    static CentralCache& getInstance() {
        static CentralCache instance;
        return instance;
    }

    std::byte* allocate(size_t size, size_t idx, size_t& count);
    void deallocate(std::byte* list_head, size_t size, size_t count);

public:
    CentralCache(const CentralCache&) = delete;
    CentralCache& operator=(const CentralCache&) = delete;
    CentralCache(CentralCache&&) = delete;
    CentralCache& operator=(CentralCache&&) = delete;

private:
    struct alignas(kCacheLineSize) Bucket {
        SpanList nonempty;
        std::atomic_flag lock = ATOMIC_FLAG_INIT;
    };

    CentralCache() {
        for (auto& bucket : buckets_) {
            bucket.lock.clear();
        }
    }
    ~CentralCache() = default;

    Span* fetchSpanFromPageCache(size_t objSize, size_t index);

private:
    std::array<Bucket, FREE_LIST_SIZE> buckets_;
};

}

#endif
