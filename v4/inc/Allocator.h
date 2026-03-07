#ifndef _ALLOCATOR_H_
#define _ALLOCATOR_H_

#include "ThreadCache.h"

#include <cstddef>

namespace memorypool {
    inline void* allocate(size_t size) {
        return ThreadCache::getInstance().allocate(size);
    }

    inline void deallocate(void* ptr, size_t size) {
        ThreadCache::getInstance().deallocate(ptr, size);
        return;
    }
}

#endif