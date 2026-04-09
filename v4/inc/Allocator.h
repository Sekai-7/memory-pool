#ifndef _ALLOCATOR_H_
#define _ALLOCATOR_H_

#include "ThreadCache.h"
#include "PageCache.h"

#include <cstddef>

namespace memorypool {
    inline void* allocate(size_t size) {
        if (size > MAX_POOL_BYTES) {
            int pageCount = (size + PAGE_SIZE - 1) / PAGE_SIZE;
            auto* span = PageCache::getInstance().requestFromOS(pageCount);
        }
        return ThreadCache::getInstance().allocate(size);
    }

    inline void deallocate(void* ptr, size_t size) {
        ThreadCache::getInstance().deallocate(ptr, size);
        return;
    }
}

#endif