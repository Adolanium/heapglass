#pragma once
#include "protocol.hpp"
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace hg {
struct AllocationId {
    uint64_t session{}, heap_generation{}, generation{};
    auto operator<=>(const AllocationId &) const = default;
};
enum class Life { live, freed, uncertain };
struct Text {
    uint64_t offset{};
    std::string encoding, value;
};
struct CapturedContent {
    std::vector<uint8_t> bytes;
    std::vector<Text> strings;
};
struct Capture {
    uint64_t begin{}, end{}, offset{}, requested{}, hash{}, sampled_size{}, allocation_size{};
    double entropy{};
    uint32_t protection{};
    bool attributed{}, truncated{};
    std::string note;
    uint64_t sequence{};
    std::shared_ptr<const CapturedContent> content;
    const std::vector<uint8_t> &bytes() const;
    const std::vector<Text> &strings() const;
    void set_bytes(std::vector<uint8_t> bytes);
};
struct SizePoint {
    uint64_t ticks{}, size{}, address{};
};
struct Allocation {
    AllocationId id;
    uint64_t heap{}, address{}, size{}, born{}, died{}, begin_order{}, end_order{}, caller{}, parent{};
    uint32_t thread{};
    std::array<uint64_t, max_stack_frames> stack{};
    uint32_t stack_count{}, stack_status{};
    Life life = Life::live;
    bool birth_known = true;
    std::vector<SizePoint> history;
    std::vector<Capture> captures;
};
struct Region {
    uint64_t base{}, size{}, allocation_base{}, observed{}, previous_observed{};
    uint32_t protection{}, previous_protection{}, type{};
    bool image_association{};
    std::string module, mapped_file;
};
struct Finding {
    std::string rule, title, evidence, confidence;
    uint64_t address{}, begin{}, end{}, generation{};
};
enum class Retention { rolling, stop_at_capacity };
struct Limits {
    size_t per_allocation = 4096, per_scan = 1024 * 1024, per_session = 32 * 1024 * 1024;
    size_t records = 50000, events = 10000, captures = 8, min_string = 4;
    unsigned sample_ms = 1000;
    unsigned stack_frames = 0;
    Retention retention = Retention::rolling;
    double high_entropy = 7.2, changed_fraction = .35, entropy_change = 1.5;
};
struct Snapshot {
    std::string name;
    uint64_t begin{}, end{}, session{};
    bool incomplete{};
    std::vector<Allocation> records;
};
enum class Change { added, freed, resized, modified, unchanged, uncertain };
struct Difference {
    AllocationId id;
    uint64_t address{};
    Change change{};
    std::vector<uint64_t> byte_offsets;
};
struct Model {
    uint64_t session{}, frequency{}, started{}, dropped{}, observed{}, evicted{}, retained_bytes{};
    DWORD pid{};
    uint64_t creation{};
    std::string process, status = "No session", start_time;
    bool incomplete{}, recording{}, connected{}, offline{}, sampling_budget_exhausted{};
    uint64_t evicted_captures{}, skipped_captures{}, controller_dropped{}, transport_pending{},
        analysis_pending{}, stack_violations{};
    uint32_t active_stack_frames{}, stack_state{};
    Limits limits;
    std::map<uint64_t, Allocation> records;
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> live;
    std::set<uint64_t> retired_records;
    struct Heap {
        uint64_t generation{}, end_order{};
        bool dead{};
    };
    std::map<uint64_t, Heap> heaps;
    std::deque<Event> events;
    std::vector<Region> regions;
    std::vector<Finding> findings;
    std::vector<Snapshot> snapshots;
    std::set<uint64_t> watched;
    uint64_t next_generation = 1, next_heap = 1, unsafe_through = 0, next_capture = 1;
    std::map<uint64_t, uint64_t> capture_owners;
    void apply(const Event &e);
    void loss(uint64_t total, uint64_t through = 0);
    void boundary();
    void capture(uint64_t generation, Capture c);
    void trim_captures();
    void rebuild_retention();
    Model presentation() const;
    void analyze();
    Snapshot snapshot(std::string name, uint64_t begin, uint64_t end) const;
    void prune();
};
Limits validated_limits(Limits limits);
const char *life_name(Life s);
const char *change_name(Change c);
const char *op_name(Op op);
double entropy(std::span<const uint8_t> bytes);
uint64_t content_hash(std::span<const uint8_t> bytes);
std::vector<Text> extract_strings(std::span<const uint8_t> bytes, size_t minimum);
std::vector<Difference> compare(const Snapshot &a, const Snapshot &b);
void save_session(const Model &, const std::wstring &path, bool include_bytes);
Model load_session(const std::wstring &path);
void export_csv(const Model &, const std::wstring &path);
} // namespace hg
