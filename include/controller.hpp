#pragma once
#include "analysis.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace hg {
struct Process {
    DWORD pid{};
    std::string name, architecture;
};
std::vector<Process> processes();
std::string narrow(const std::wstring &);
std::wstring widen(const std::string &);
std::string error_text(DWORD code);
std::wstring executable_directory();
std::string protection_name(uint32_t);
Capture read_memory(HANDLE process, uint64_t address, size_t count);
struct PipelineMetrics {
    uint64_t frequency{}, ingest_ticks{}, apply_ticks{}, publish_ticks{}, sample_ticks{}, symbol_ticks{},
        controller_dropped{}, max_pending{};
};
class Controller {
  public:
    explicit Controller(std::wstring agent_filename = L"HeapglassAgent.dll");
    ~Controller();
    Controller(const Controller &) = delete;
    Controller &operator=(const Controller &) = delete;
    void attach(DWORD pid);
    void launch(std::wstring path, std::wstring args = {}, std::wstring cwd = {});
    void disconnect();
    void record(bool enabled);
    void take_snapshot(std::string name);
    void watch(uint64_t generation, bool enabled);
    void configure(Limits limits);
    void save(std::wstring path, bool bytes);
    void load(std::wstring path);
    void csv(std::wstring path);
    void sample_now();
    std::shared_ptr<const Model> view() const {
        return published.load();
    }
    std::string symbol(uint64_t address) const;
    // Validation uses the same worker and transport as the desktop.
    void stall_ingestion(unsigned ms);
    PipelineMetrics metrics() const;

  private:
    std::jthread worker, collector;
    std::wstring agent_filename;
    static constexpr size_t pending_capacity = 524288;
    std::vector<Event> pending_events;
    std::atomic<size_t> pending_write{0}, pending_read{0};
    std::atomic<uint64_t> local_dropped{0}, stall_until{0}, ingest_ticks{0}, apply_ticks{0}, publish_ticks{0},
        sample_ticks{0}, symbol_ticks{0}, max_pending{0};
    bool dirty = true;
    void collect(std::stop_token, HANDLE idle_timer);
    void stop_collector();
    std::atomic<std::shared_ptr<const Model>> published;
    mutable std::mutex jobs_mutex, symbol_mutex;
    std::deque<std::function<void()>> jobs;
    std::map<uint64_t, std::string> symbols;
    Model model;
    Limits desired_limits;
    HANDLE process_handle = nullptr, map_handle = nullptr, lease = nullptr;
    Shared *shared = nullptr;
    uint64_t last_sample = 0, last_region = 0, last_publish = 0, cursor = 0;
    bool symbols_ready = false, force_sample = false;
    void enqueue(std::function<void()>);
    void loop(std::stop_token);
    void connect(DWORD pid);
    void close();
    void drain();
    void sample();
    void memory_map();
    void resolve();
    void publish();
};
} // namespace hg
