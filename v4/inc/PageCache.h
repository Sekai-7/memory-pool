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
    Span* allocateDirect(size_t);

    void deallocate(Span*);
    void deallocateDirect(Span*);

public:
    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;
    PageCache(PageCache&&) = delete;
    PageCache& operator=(PageCache&&) = delete;

private:
    static constexpr size_t kScavengeThresholdPages = 4 * MAX_PAGES_IN_SPAN;
    static constexpr size_t kChunkReleaseThresholdPages = 8 * MAX_PAGES_IN_SPAN;
    static constexpr size_t kMinReleaseSpanPages = 8;

    PageCache() = default;
    ~PageCache() = default;

    Span* requestFromOS(size_t);
    void pushFreeSpan(Span*);
    void removeFreeSpan(Span*);
    void refreshOsChunkHead(Span*);
    bool releaseSpanToOS(Span*);
    bool releaseChunkToOS(Span*);
    void scavenge();

private:
    std::array<SpanList, MAX_PAGES_IN_SPAN> spanLists_;
    size_t freePages_{0};
    size_t releasedPages_{0};

    std::mutex page_mutex_;
};

}

#endif
