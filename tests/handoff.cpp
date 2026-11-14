#include "protocol.hpp"
#include <atomic>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>
using namespace hg;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(#x);                                                                    \
    } while (false)
int main() {
    try {
        auto *shared = static_cast<Shared *>(
            VirtualAlloc(nullptr, sizeof(Shared), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        CHECK(shared);
        initialize(shared, 1, 2, 3);
        CHECK(transport_quiet(shared));
        Event freed{};
        freed.op = Op::free;
        freed.begin = 1;
        freed.end = 2;
        freed.heap = 0x1000;
        freed.previous = 0x2000;
        freed.result = 1;
        CHECK(push(shared, freed));
        std::latch inside(1), resume(1);
        std::atomic<bool> published = false;
        Event destination{};
        std::jthread collector([&] {
            CHECK(transfer(shared, [&](const Event &event) {
                inside.count_down();
                resume.wait();
                destination = event;
                published.store(true, std::memory_order_release);
            }));
        });
        inside.wait();
        const bool protected_while_handoff = !transport_quiet(shared) &&
                                             !published.load(std::memory_order_acquire) &&
                                             atomic_read(&shared->write) - atomic_read(&shared->read) == 1;
        resume.count_down();
        collector.join();
        CHECK(protected_while_handoff);
        CHECK(published.load(std::memory_order_acquire));
        CHECK(transport_quiet(shared));
        CHECK(destination.op == Op::free);
        CHECK(destination.previous == 0x2000);
        CHECK(destination.result == 1);
        const auto quiet_clock = atomic_read(&shared->clock);
        CHECK(transport_quiet(shared, 0, 0));
        dropped(shared); // Loss completes after consumer accounting, before the first cutoff clock read.
        CHECK(atomic_read(&shared->clock) == quiet_clock);
        CHECK(transport_quiet(shared));
        CHECK(!transport_quiet(shared, 0, 0));
        CHECK(transport_quiet(shared, 1, 0));
        CHECK(!transport_quiet(shared, 1, 1));
        CHECK(transport_quiet(shared, 2, 1));
        InterlockedIncrement64(&shared->unbuffered_active);
        CHECK(!transport_quiet(shared));
        InterlockedDecrement64(&shared->unbuffered_active);
        CHECK(transport_quiet(shared));
        VirtualFree(shared, 0, MEM_RELEASE);
        std::cout << "Collector handoff never advertises idle before destination publication\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
