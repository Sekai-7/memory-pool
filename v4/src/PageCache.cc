#include "PageCache.h"

namespace memorypool {

Span* PageCache::allocate(size_t) {
    return nullptr;
}

void PageCache::deallocate(void*) {
    return;
}

PageCache::~PageCache() {
    return;
}
}