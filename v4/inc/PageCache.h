#ifndef __PAGE_CACHE_H__
#define __PAGE_CACHE_H__

#include "util.h"

#include <mutex>
#include <array>

namespace memorypool {

class PageCache {
public:
    static PageCache& getInstance() {
        static PageCache instance;
        return instance;
    }

    Span* allocate(size_t);

    void deallocate(Span*);

    Span* requestFromOS(size_t);
public:
    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;
    PageCache(PageCache&&) = delete;
    PageCache& operator=(PageCache&&) = delete;

private:
    PageCache() = default;
    ~PageCache() = default;

private:
    std::array<SpanList, MAX_PAGES> spanLists_;

    std::mutex page_mutex_;
};

}

#endif