#ifndef _CENTRAL_CACHE_H_
#define _CENTRAL_CACHE_H_

#include "util.h"

#include <cstddef>
#include <array>
#include <memory>
#include <cstdint>
#include <atomic>

namespace memorypool {

class CentralCache {
public:
    static CentralCache& getInstance() {
        static CentralCache instance;
        return instance;
    }

    std::byte* allocate(size_t size, size_t& count);
    void deallocate(std::byte* list_head, size_t size, size_t count);

public:
    CentralCache(const CentralCache&) = delete;
    CentralCache& operator=(const CentralCache&) = delete;
    CentralCache(CentralCache&&) = delete;
    CentralCache& operator=(CentralCache&&) = delete;

private:
    CentralCache() {
        for (auto& lock : locks_) {
            lock.clear();
        }
    }
    ~CentralCache() = default;

    Span* fetchSpanFromPageCache(int index, size_t objSize);

private:
    std::array<SpanList, FREE_LIST_SIZE> nonempty_;

    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;
};

}

#endif