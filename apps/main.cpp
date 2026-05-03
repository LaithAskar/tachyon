#include <iostream>

#include "tachyon/matching_engine.hpp"

int main() {
    using namespace tachyon;

    MatchingEngine engine;

    // $100.00 in 1/100-of-a-cent ticks (price multiplier = 10000)
    Order buy_limit{
        /*id=*/1,
        /*side=*/Side::Buy,
        /*type=*/OrderType::Limit,
        /*price=*/1'000'000,
        /*quantity=*/10,
        /*timestamp_ns=*/0,
    };

    auto trades = engine.on_order(buy_limit);

    std::cout << "tachyon online.\n"
              << "  orders submitted: 1\n"
              << "  trades produced:  " << trades.size() << "\n"
              << "  book size:        " << engine.book().size() << "\n";

    return 0;
}
