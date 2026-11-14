#include "controller.hpp"
#include <algorithm>
#include <stdexcept>
namespace hg {
Capture read_memory(HANDLE process, uint64_t address, size_t count) {
    if (count > 1024 * 1024 || count > UINT64_MAX - address)
        throw std::invalid_argument("Remote read range exceeds bounds");
    Capture c;
    c.begin = ticks();
    c.requested = count;
    std::vector<uint8_t> bytes;
    bytes.reserve(count);
    while (bytes.size() < count) {
        MEMORY_BASIC_INFORMATION region{};
        const auto cursor = address + bytes.size();
        if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(cursor), &region, sizeof(region)) !=
                sizeof(region) ||
            region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            c.note = "Inaccessible, guard, uncommitted or exited target";
            break;
        }
        if (bytes.empty())
            c.protection = region.Protect;
        auto base = reinterpret_cast<uint64_t>(region.BaseAddress);
        if (base > cursor || region.RegionSize > UINT64_MAX - base || base + region.RegionSize <= cursor) {
            c.note = "Invalid queried region range";
            break;
        }
        auto part =
            static_cast<size_t>(std::min<uint64_t>(count - bytes.size(), base + region.RegionSize - cursor));
        auto previous = bytes.size();
        bytes.resize(previous + part);
        SIZE_T got = 0;
        BOOL ok = ReadProcessMemory(process, reinterpret_cast<LPCVOID>(cursor), bytes.data() + previous, part,
                                    &got);
        if (got > part) {
            bytes.resize(previous);
            c.note = "Invalid partial read length";
            break;
        }
        bytes.resize(previous + got);
        if (!ok || got < part) {
            c.note = "Partial or failed ReadProcessMemory";
            break;
        }
    }
    c.end = ticks();
    c.truncated = bytes.size() < count;
    c.sampled_size = bytes.size();
    c.set_bytes(std::move(bytes));
    return c;
}
} // namespace hg
