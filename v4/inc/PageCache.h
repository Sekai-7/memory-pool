#ifndef __PAGE_CACHE_H__
#define __PAGE_CACHE_H__

#include "util.h"

#include <sys/mman.h>
#include <cstddef>
#include <atomic>
#include <mutex>

namespace memorypool {

class PageCache {
public:
    static PageCache& getInstance() {
        static PageCache instance;
        return instance;
    }

    Span* allocate(size_t);

    void deallocate(void*);

public:
    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;
    PageCache(PageCache&&) = delete;
    PageCache& operator=(PageCache&&) = delete;

private:
    PageCache() = default;
    ~PageCache();
};

}

#endif