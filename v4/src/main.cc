#include "Allocator.h"

#include <iostream>

using namespace memorypool;

int main() {
    uintptr_t sum = 0;
    for (int i = 0; i < 100000000; ++i) {
        auto* ptr = allocate(10);
        sum += reinterpret_cast<std::uintptr_t>(ptr) & 1;
        deallocate(ptr);
    }
    return 0;
}
