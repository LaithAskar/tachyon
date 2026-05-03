#include "tachyon/matching_engine.hpp"

namespace tachyon {

std::vector<Trade> MatchingEngine::on_order(const Order& order) {
    return book_.submit(order);
}

bool MatchingEngine::on_cancel(OrderId id) {
    return book_.cancel(id);
}

}  // namespace tachyon
