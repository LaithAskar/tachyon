#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "tachyon/order.hpp"

namespace tachyon {

// Node carrying one resting order plus the two pointers that splice it into a
// per-level intrusive doubly-linked list. Lives inside a Pool — its address is
// stable for the entire lifetime of that pool.
struct OrderNode {
    Order      order;
    OrderNode* prev = nullptr;
    OrderNode* next = nullptr;
};

// Block-growing pool of OrderNodes. Allocation pops from a free-list; release
// pushes back. When the free-list is empty, we allocate a fresh block of nodes
// rather than growing a single vector — that keeps every previously-handed-out
// OrderNode* address valid forever, which is the only reason we're using a
// pool at all (the id index stores raw OrderNode* and we cannot afford
// reallocation invalidation).
class Pool {
public:
    explicit Pool(std::size_t block_size = 4096) : block_size_(block_size) {}

    // Construct (well, prepare) a new node. Returns a pointer whose address
    // remains valid until `release()` or until the Pool is destroyed.
    OrderNode* acquire() {
        if (free_head_ == nullptr) grow_one_block();
        OrderNode* n = free_head_;
        free_head_   = free_head_->next;
        n->prev = n->next = nullptr;
        ++live_;
        return n;
    }

    void release(OrderNode* n) {
        n->prev = nullptr;
        n->next = free_head_;
        free_head_ = n;
        --live_;
    }

    std::size_t live_count() const noexcept { return live_; }

private:
    void grow_one_block() {
        auto block = std::make_unique<OrderNode[]>(block_size_);
        // Thread the new block's nodes onto the free-list. Done in reverse so
        // the lowest-address node ends up at the head — keeps allocation
        // sequences roughly contiguous in memory, which is the whole point
        // of a pool.
        for (std::size_t i = block_size_; i-- > 0; ) {
            block[i].next = free_head_;
            free_head_    = &block[i];
        }
        blocks_.push_back(std::move(block));
    }

    std::size_t                              block_size_;
    std::vector<std::unique_ptr<OrderNode[]>> blocks_;
    OrderNode*                                free_head_ = nullptr;
    std::size_t                               live_      = 0;
};

}  // namespace tachyon
