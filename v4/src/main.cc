#include "Allocator.h"

#include <iostream>

using namespace memorypool;

int main() {
    void* size = allocate(10);
    deallocate(size, 10);
    return 0;
}