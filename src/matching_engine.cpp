#include "tachyon/matching_engine.hpp"

namespace tachyon {

void MatchingEngine::on_order(const Order& order, std::vector<Trade>& out) {
    book_.submit(order, out);
    if (listener_) {
        for (const Trade& t : out) listener_(t);
    }
}

std::vector<Trade> MatchingEngine::on_order(const Order& order) {
    std::vector<Trade> trades;
    on_order(order, trades);
    return trades;
}

bool MatchingEngine::on_cancel(OrderId id) {
    return book_.cancel(id);
}

}  // namespace tachyon
