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