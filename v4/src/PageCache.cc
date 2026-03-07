#include "PageCache.h"

namespace memorypool {
    void* PageCache::allocate() {
        return nullptr;
    }

    void PageCache::deallocate(void*) {
        return;
    }

    ~PageCache() {
        return;
    }
}