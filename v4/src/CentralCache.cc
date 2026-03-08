#include "CentralCache.h"
#include "PageCache.h"

#include <thread>

namespace memorypool {

    void RadixTreePageMap::push(uintptr_t key, Span* span) {
        size_t i1 = (key >> (BITS_PER_LEVEL * 2)) & (LEVEL_LENGTH - 1);
        size_t i2 = (key >> BITS_PER_LEVEL) & (LEVEL_LENGTH - 1);
        size_t i3 = key & (LEVEL_LENGTH - 1);

        // 1. 确保 L2 节点存在
        Node* node = root_[i1].load(std::memory_order_acquire);
        if (!node) {
            // 【关键改变】：从元数据分配器直接拿系统底层内存
            Node* new_node = MetaDataAllocator::getInstance().allocateNode<Node>();

            Node* expected = nullptr;
            if (root_[i1].compare_exchange_strong(expected, new_node, std::memory_order_release)) {
                node = new_node;
            } else {
                // 注意：由于我们使用的是 Bump Allocator 且不回收节点，
                // 这里的竞态失败会导致这 32KB 内存被“浪费”（Leak）。
                // 但在整个进程生命周期中，这种情况发生的概率极低，且浪费上限极小，
                // TCMalloc 的实现中也是容忍这种良性泄漏的。
                node = expected;
            }
        }

        // 2. 确保 L3 节点存在
        LeafNode* leaf = node->leaves[i2].load(std::memory_order_acquire);
        if (!leaf) {
            // 【关键改变】：直接拿到底层内存
            LeafNode* new_leaf = MetaDataAllocator::getInstance().allocateNode<LeafNode>();

            LeafNode* expected = nullptr;
            if (node->leaves[i2].compare_exchange_strong(expected, new_leaf, std::memory_order_release)) {
                leaf = new_leaf;
            } else {
                leaf = expected;
            }
        }

        // 3. 写入 Span 映射
        leaf->spans[i3].store(span, std::memory_order_release);
    }

    Span* RadixTreePageMap::getSpan(uintptr_t key) {
        size_t i1 = (key >> (BITS_PER_LEVEL * 2)) & (LEVEL_LENGTH - 1);
        size_t i2 = (key >> BITS_PER_LEVEL) & (LEVEL_LENGTH - 1);
        size_t i3 = key & (LEVEL_LENGTH - 1);

        Node* node = root_[i1].load(std::memory_order_acquire);
        if (!node) return nullptr;

        LeafNode* leaf = node->leaves[i2].load(std::memory_order_acquire);
        if (!leaf) return nullptr;

        return leaf->spans[i3].load(std::memory_order_acquire);
    }

    std::byte* CentralCache::allocate(size_t size, size_t count) {
        int index = getListIndex(align(size));
        if (index >= FREE_LIST_SIZE) {
            return {};
        }

        while (locks_[index].test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield(); // 添加线程让步，避免忙等待，避免过度消耗CPU
        }

        std::byte* ret = nullptr;

        if (count > centralFreeListSize_[index]) {
            locks_[index].clear(std::memory_order_release);

            void* applyMemory = PageCache::getInstance().allocate();
            if (applyMemory == nullptr) {
                return nullptr;
            }

        } else {
            ret = centralFreeList_[index];

            auto next = centralFreeList_[index];
            while (--count) {
                next = *(reinterpret_cast<std::byte**>(next));
            }

            centralFreeList_[index] = *(reinterpret_cast<std::byte**>(next));
            *(reinterpret_cast<std::byte**>(next)) = nullptr;
            centralFreeListSize_[index]--;
        }

        locks_[index].clear(std::memory_order_release);

        return ret;
    }

    void CentralCache::deallocate(std::byte*, size_t) {

    }

    CentralCache::~CentralCache() {

    }
}