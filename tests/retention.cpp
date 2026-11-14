#include "analysis.hpp"
#include <iostream>
#include <stdexcept>
using namespace hg;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(#x);                                                                    \
    } while (false)
Event allocation(uint64_t order, uint64_t address) {
    Event e{};
    e.op = Op::allocate;
    e.begin = order;
    e.end = order + 1;
    e.ticks = order;
    e.heap = 0x1000;
    e.result = address;
    e.requested = 16;
    return e;
}
Capture capture(uint8_t value, uint64_t at) {
    Capture c;
    c.set_bytes(std::vector<uint8_t>(16, value));
    c.begin = at;
    c.end = at + 1;
    c.requested = 16;
    c.attributed = true;
    return c;
}
int main() {
    try {
        Model m;
        m.session = 123;
        m.limits.per_session = 32;
        m.limits.per_allocation = 16;
        m.limits.per_scan = 32;
        m.apply(allocation(1, 0x2000));
        m.apply(allocation(3, 0x3000));
        m.capture(1, capture('a', 10));
        m.capture(2, capture('b', 20));
        m.capture(1, capture('c', 30));
        CHECK(m.records.at(1).captures.back().bytes() == std::vector<uint8_t>(16, 'c'));
        CHECK(m.retained_bytes == 32);
        m.apply(allocation(5, 0x4000));
        m.capture(3, capture('d', 40));
        CHECK(m.records.at(2).captures.empty());
        CHECK(m.records.at(3).captures.back().bytes() == std::vector<uint8_t>(16, 'd'));
        CHECK(m.retained_bytes == 32);
        auto old_view = m.presentation();
        m.limits.retention = Retention::stop_at_capacity;
        m.capture(1, capture('e', 50));
        CHECK(m.sampling_budget_exhausted);
        CHECK(m.skipped_captures == 1);
        CHECK(m.records.at(1).captures.back().bytes() == std::vector<uint8_t>(16, 'c'));
        m.limits.retention = Retention::rolling;
        m.capture(1, capture('e', 60));
        CHECK(!m.sampling_budget_exhausted);
        CHECK(m.retained_bytes == 32);
        CHECK(old_view.records.at(1).captures.back().bytes() == std::vector<uint8_t>(16, 'c'));
        CHECK(m.records.at(1).captures.back().bytes() == std::vector<uint8_t>(16, 'e'));
        m.limits.per_session = 16;
        m.trim_captures();
        CHECK(m.retained_bytes == 16);
        CHECK(m.records.at(3).captures.empty());
        CHECK(m.records.at(1).captures.back().bytes() == std::vector<uint8_t>(16, 'e'));
        CHECK(m.evicted_captures == 4);
        Model bounded;
        bounded.session = 456;
        bounded.limits.records = 2;
        bounded.apply(allocation(1, 0x2000));
        bounded.apply(allocation(3, 0x3000));
        Event freed{};
        freed.op = Op::free;
        freed.begin = 5;
        freed.end = 6;
        freed.heap = 0x1000;
        freed.previous = 0x3000;
        freed.result = 1;
        bounded.apply(freed);
        bounded.apply(allocation(7, 0x4000));
        CHECK(bounded.records.contains(1));
        CHECK(!bounded.records.contains(2));
        CHECK(bounded.records.at(3).life == Life::live);
        bounded.loss(1, 10);
        bounded.apply(allocation(11, 0x5000));
        CHECK(!bounded.records.contains(1));
        CHECK(bounded.records.at(3).life == Life::uncertain);
        CHECK(bounded.records.at(4).life == Life::live);
        CHECK(bounded.evicted == 2);
        std::cout << "rolling retention keeps sampling at the byte limit\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
