#ifndef _META_DATA_H_
#define _META_DATA_H_

#include "util.h"

#include <atomic>

namespace memorypool {

#ifdef MEMORY_POOL_ENABLE_UNIT_TEST_HOOKS
namespace {

constexpr size_t kNoSetSpanFailure = std::numeric_limits<size_t>::max();
std::atomic<size_t> g_fail_after_nonnull_sets{kNoSetSpanFailure};

}

void failNextNonNullSetSpanAfter(size_t successfulWrites) {
    g_fail_after_nonnull_sets.store(successfulWrites, std::memory_order_release);
}

void resetSetSpanFailureInjection() {
    g_fail_after_nonnull_sets.store(kNoSetSpanFailure, std::memory_order_release);
}
#endif

bool RadixTreePageMap::setSpan(uintptr_t key, Span* span) {
#ifdef MEMORY_POOL_ENABLE_UNIT_TEST_HOOKS
    if (span != nullptr) {
        size_t remaining = g_fail_after_nonnull_sets.load(std::memory_order_acquire);
        while (remaining != kNoSetSpanFailure) {
            if (remaining == 0) {
                if (g_fail_after_nonnull_sets.compare_exchange_weak(
                        remaining,
                        kNoSetSpanFailure,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    return false;
                }
                continue;
            }

            if (g_fail_after_nonnull_sets.compare_exchange_weak(
                    remaining,
                    remaining - 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                break;
            }
        }
    }
#endif

    auto pageId = key >> PAGE_SHIFT;

    size_t i1 = (pageId >> (BITS_PER_LEVEL * 2)) & (LEVEL_LENGTH - 1);
    size_t i2 = (pageId >> BITS_PER_LEVEL) & (LEVEL_LENGTH - 1);
    size_t i3 = pageId & (LEVEL_LENGTH - 1);

    // 1. 确保 L2 节点存在
    Node* node = root_[i1].load(std::memory_order_acquire);
    if (!node && span == nullptr) {
        return true;
    }
    if (!node) {
        // 【关键改变】：从元数据分配器直接拿系统底层内存
        Node* new_node = NodeAllocator::getInstance().allocate();
        if (new_node == nullptr) {
            return false;
        }

        Node* expected = nullptr;
        if (root_[i1].compare_exchange_strong(expected, new_node, std::memory_order_acq_rel, std::memory_order_acquire)) {
            node = new_node;
        } else {
            NodeAllocator::getInstance().deallocate(new_node);
            node = expected;
        }
    }

    // 2. 确保 L3 节点存在
    LeafNode* leaf = node->leaves[i2].load(std::memory_order_acquire);
    if (!leaf && span == nullptr) {
        return true;
    }
    if (!leaf) {
        // 【关键改变】：直接拿到底层内存
        LeafNode* new_leaf = LeafNodeAllocator::getInstance().allocate();
        if (new_leaf == nullptr) {
            return false;
        }

        LeafNode* expected = nullptr;
        if (node->leaves[i2].compare_exchange_strong(expected, new_leaf, std::memory_order_acq_rel, std::memory_order_acquire)) {
            leaf = new_leaf;
        } else {
            LeafNodeAllocator::getInstance().deallocate(new_leaf);
            leaf = expected;
        }
    }

    // 3. 写入 Span 映射
    leaf->spans[i3].store(span, std::memory_order_release);
    return true;
}

}

#endif
