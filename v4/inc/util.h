#ifndef _UTIL_H_
#define _UTIL_H_

#include <cstddef>
#include <cstdint>

namespace memorypool {

constexpr size_t ALIGNLEN = sizeof(void*);

constexpr size_t DEFAULT_THRESHOLD = 8;

constexpr size_t FREE_LIST_SIZE = 1024;

inline size_t align(size_t size) {
    return (size + ALIGNLEN - 1) & ~(ALIGNLEN - 1);
}

inline uint8_t getListIndex(size_t size) {
    // return align(size) / ALIGNLEN - 1;
    auto alignSize = align(size);
    if (alignSize == 8) {
        return 0;
    } else if (alignSize == 16) {
        return 1;
    } else if (alignSize == 32) {
        return 2;
    } else if (alignSize == 64) {
        return 3;
    } else if (alignSize == 128) {
        return 4;
    } else if (alignSize == 256) {
        return 5;
    } else if (alignSize == 512) {
        return 6;
    } else if (alignSize == 1024) {
        return 7;
    }
}

}

#endif