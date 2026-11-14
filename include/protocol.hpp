#pragma once
#include <windows.h>
#if !defined(_M_X64)
#error Heapglass requires native Windows x64
#endif
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <type_traits>

namespace hg {
constexpr uint32_t protocol_version = 1;
constexpr uint64_t protocol_magic = 0x3153414c47414548;
constexpr size_t ring_capacity = 32768;
enum class Op : uint32_t { allocate, reallocate, free, create_heap, destroy_heap };
struct Event {
    uint64_t begin{}, end{}, ticks{}, heap{}, address{}, previous{}, requested{}, result{}, caller{};
    uint32_t thread{}, flags{};
    Op op{};
    uint32_t reserved{};
};
static_assert(sizeof(Event) == 88 && std::is_trivially_copyable_v<Event>);
struct alignas(64) Slot {
    volatile LONG64 sequence;
    Event event;
    uint8_t padding[32];
};
struct alignas(64) Shared {
    uint64_t magic;
    uint32_t version, bytes, pid, record_bytes;
    uint64_t creation, session, frequency, started;
    volatile LONG recording, disconnected, ready, error;
    volatile LONG64 write, read, clock, dropped, active, heartbeat, epoch;
    Slot slots[ring_capacity];
};
static_assert(sizeof(Slot) == 128 && offsetof(Shared, slots) == 128);
inline LONG64 atomic_read(volatile LONG64 *p) {
    return InterlockedCompareExchange64(p, 0, 0);
}
inline LONG atomic_read(volatile LONG *p) {
    return InterlockedCompareExchange(p, 0, 0);
}
struct Reservation {
    Slot *slot = nullptr;
    LONG64 position{};
};
inline Reservation reserve(Shared *s) noexcept {
    LONG64 pos = atomic_read(&s->write);
    for (unsigned tries = 0; tries < 16; ++tries) {
        auto &slot = s->slots[static_cast<size_t>(pos) % ring_capacity];
        auto diff = atomic_read(&slot.sequence) - pos;
        if (diff < 0)
            break;
        if (diff == 0 && InterlockedCompareExchange64(&s->write, pos + 1, pos) == pos)
            return {&slot, pos};
        pos = atomic_read(&s->write);
    }
    return {};
}
inline void dropped(Shared *s) noexcept {
    InterlockedIncrement64(&s->dropped);
}
inline void publish(Reservation reservation, const Event &e) noexcept {
    reservation.slot->event = e;
    InterlockedExchange64(&reservation.slot->sequence, reservation.position + 1);
}
inline bool push(Shared *s, const Event &e) noexcept {
    auto r = reserve(s);
    if (!r.slot) {
        dropped(s);
        return false;
    }
    publish(r, e);
    return true;
}
inline bool pop(Shared *s, Event &e) noexcept {
    auto pos = atomic_read(&s->read);
    auto &slot = s->slots[static_cast<size_t>(pos) % ring_capacity];
    if (atomic_read(&slot.sequence) != pos + 1)
        return false;
    e = slot.event;
    InterlockedExchange64(&slot.sequence, pos + static_cast<LONG64>(ring_capacity));
    InterlockedExchange64(&s->read, pos + 1);
    return true;
}
inline void initialize(Shared *s, DWORD pid, uint64_t creation, uint64_t session) {
    s->magic = protocol_magic;
    s->version = protocol_version;
    s->bytes = sizeof(Shared);
    s->pid = pid;
    s->record_bytes = sizeof(Event);
    s->creation = creation;
    s->session = session;
    LARGE_INTEGER q{};
    QueryPerformanceFrequency(&q);
    s->frequency = q.QuadPart;
    QueryPerformanceCounter(&q);
    s->started = q.QuadPart;
    for (size_t i = 0; i < ring_capacity; ++i)
        s->slots[i].sequence = static_cast<LONG64>(i);
}
inline bool valid(const Shared *s, DWORD pid, uint64_t creation) {
    return s->magic == protocol_magic && s->version == protocol_version && s->bytes == sizeof(Shared) &&
           s->record_bytes == sizeof(Event) && s->pid == pid && s->creation == creation && s->session &&
           s->frequency;
}
inline uint64_t filetime(FILETIME t) {
    return uint64_t(t.dwLowDateTime) | (uint64_t(t.dwHighDateTime) << 32);
}
inline uint64_t ticks() {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}
inline void mapping_name(wchar_t *b, size_t n, DWORD pid, uint64_t created) {
    swprintf_s(b, n, L"Local\\Heapglass-%lu-%016llx", pid, created);
}
} // namespace hg
