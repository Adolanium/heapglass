#include <windows.h>
#include <array>
#include <atomic>
#include <bcrypt.h>
#include <compressapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

std::mutex log_mutex;
std::ofstream truth;
void log(const char *operation, HANDLE heap, const void *ptr, size_t size, bool success = true) {
    std::lock_guard lock(log_mutex);
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    if (truth)
        truth << "{\"qpc\":" << q.QuadPart << ",\"thread\":" << GetCurrentThreadId() << ",\"operation\":\""
              << operation << "\",\"heap\":" << reinterpret_cast<uint64_t>(heap)
              << ",\"address\":" << reinterpret_cast<uint64_t>(ptr) << ",\"size\":" << size
              << ",\"success\":" << (success ? "true" : "false") << "}\n";
}
bool api_semantics() {
    using Alloc = PVOID(NTAPI *)(PVOID, ULONG, SIZE_T);
    using Realloc = PVOID(NTAPI *)(PVOID, ULONG, PVOID, SIZE_T);
    using Free = ULONG(NTAPI *)(PVOID, ULONG, PVOID);
    auto nt = GetModuleHandleW(L"ntdll.dll");
    auto allocate = reinterpret_cast<Alloc>(GetProcAddress(nt, "RtlAllocateHeap"));
    auto reallocate = reinterpret_cast<Realloc>(GetProcAddress(nt, "RtlReAllocateHeap"));
    auto release = reinterpret_cast<Free>(GetProcAddress(nt, "RtlFreeHeap"));
    HANDLE heap = HeapCreate(0, 0, 0);
    bool valid = true;
    constexpr DWORD sentinel = 0x1234abcd;
    SetLastError(sentinel);
    PVOID p = allocate(heap, 0, 64);
    valid = valid && p && GetLastError() == sentinel;
    SetLastError(sentinel);
    PVOID q = reallocate(heap, HEAP_REALLOC_IN_PLACE_ONLY, p, 32);
    valid = valid && q && GetLastError() == sentinel;
    if (q)
        p = q;
    SetLastError(sentinel);
    ULONG result = release(heap, 0, p);
    valid = valid && result != 0 && GetLastError() == sentinel;
    bool raised = false;
    __try {
        allocate(heap, HEAP_GENERATE_EXCEPTIONS, SIZE_MAX - 4096);
    } __except (GetExceptionCode() == 0xc0000017 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        raised = true;
    }
    HeapDestroy(heap);
    return valid && raised;
}
int sustained_workload(unsigned duration, unsigned growth_ms, const std::wstring &metrics) {
    HANDLE heap = HeapCreate(0, 0, 0);
    if (!heap)
        return 4;
    std::atomic<bool> stop = false, failed = false;
    std::atomic<uint64_t> operations = 0;
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 4; ++t)
        workers.emplace_back([&, t] {
            auto local = HeapCreate(0, 0, 0);
            if (!local) {
                failed = true;
                return;
            }
            uint64_t count = 0;
            while (!stop.load()) {
                uint64_t batch = 0;
                for (unsigned i = 0; i < 16; ++i) {
                    auto p = HeapAlloc(local, 0, 64 + ((++count + t) % 1024));
                    ++batch;
                    if (p) {
                        static_cast<char *>(p)[0] = char('a' + t);
                        if (count % 3 == 0) {
                            auto resized = HeapReAlloc(local, 0, p, 1200);
                            ++batch;
                            if (resized)
                                p = resized;
                        }
                        HeapFree(local, 0, p);
                        ++batch;
                    }
                }
                operations.fetch_add(batch);
                Sleep(25);
            }
            HeapDestroy(local);
        });
    std::vector<void *> retained;
    retained.reserve(uint64_t(duration) * 1000 / growth_ms + 1);
    auto start = GetTickCount64(), next_growth = start;
    while (!failed && GetTickCount64() - start < uint64_t(duration) * 1000) {
        if (GetTickCount64() >= next_growth) {
            auto p = static_cast<char *>(HeapAlloc(heap, HEAP_ZERO_MEMORY, 4096));
            if (!p) {
                failed = true;
                break;
            }
            strcpy_s(p, 4096, "Heapglass sustained retained fixture");
            retained.push_back(p);
            log("sustained-retained", heap, p, 4096);
            truth.flush();
            next_growth = GetTickCount64() + growth_ms;
        }
        Sleep(10);
    }
    stop = true;
    for (auto &thread : workers)
        thread.join();
    double seconds = double(GetTickCount64() - start) / 1000.;
    const uint64_t first = retained.empty() ? 0 : reinterpret_cast<uint64_t>(retained.front());
    const uint64_t last = retained.empty() ? 0 : reinterpret_cast<uint64_t>(retained.back());
    const auto result =
        "{\"operations\":" + std::to_string(operations.load()) + ",\"seconds\":" + std::to_string(seconds) +
        ",\"operations_per_second\":" + std::to_string(double(operations.load()) / seconds) +
        ",\"retained_count\":" + std::to_string(retained.size()) +
        ",\"first_retained\":" + std::to_string(first) + ",\"last_retained\":" + std::to_string(last) + "}";
    if (!metrics.empty()) {
        std::ofstream output{std::filesystem::path(metrics)};
        output << result;
    }
    HeapDestroy(heap);
    return failed ? 5 : 0;
}
int wmain(int argc, wchar_t **argv) {
    unsigned duration = 30;
    bool burst = false;
    bool sustained = false;
    unsigned growth_ms = 5000;
    uint64_t benchmark = 0;
    std::wstring ground, metrics;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--duration" && i + 1 < argc)
            duration = std::stoul(argv[++i]);
        else if (a == L"--burst")
            burst = true;
        else if (a == L"--sustained")
            sustained = true;
        else if (a == L"--growth-ms" && i + 1 < argc)
            growth_ms = std::stoul(argv[++i]);
        else if (a == L"--ground" && i + 1 < argc)
            ground = argv[++i];
        else if (a == L"--benchmark" && i + 1 < argc)
            benchmark = std::stoull(argv[++i]);
        else if (a == L"--metrics" && i + 1 < argc)
            metrics = argv[++i];
    }
    Sleep(1200); // Allow a launching controller to instrument before this controlled workload.
    if (!ground.empty())
        truth.open(std::filesystem::path(ground));
    if (sustained) {
        if (!duration || duration > 600 || growth_ms < 100 || growth_ms > 60000)
            return 2;
        return sustained_workload(duration, growth_ms, metrics);
    }
    if (benchmark) {
        auto h = HeapCreate(0, 0, 0);
        LARGE_INTEGER f{}, b{}, e{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&b);
        for (uint64_t i = 0; i < benchmark; ++i) {
            void *p = HeapAlloc(h, 0, 64 + (i % 1024));
            if (p) {
                static_cast<volatile unsigned char *>(p)[0] = 42;
                HeapFree(h, 0, p);
            }
        }
        QueryPerformanceCounter(&e);
        HeapDestroy(h);
        double seconds = double(e.QuadPart - b.QuadPart) / double(f.QuadPart);
        std::string result =
            "{\"operations\":" + std::to_string(benchmark * 2) + ",\"seconds\":" + std::to_string(seconds) +
            ",\"operations_per_second\":" + std::to_string(double(benchmark * 2) / seconds) + "}";
        std::cout << result << "\n";
        if (!metrics.empty()) {
            std::ofstream file{std::filesystem::path(metrics)};
            file << result;
        };
        return 0;
    }
    bool semantics = api_semantics();
    log("api-semantics", nullptr, nullptr, 0, semantics);
    if (!semantics)
        return 3;
    HANDLE heap = HeapCreate(0, 0, 0);
    log("create", heap, heap, 0);
    auto ascii = static_cast<char *>(HeapAlloc(heap, HEAP_ZERO_MEMORY, 4096));
    strcpy_s(ascii, 4096,
             "Heapglass controlled ASCII fixture: local analysis, exact offsets, known lifetime.");
    log("ascii", heap, ascii, 4096);
    auto wide = static_cast<wchar_t *>(HeapAlloc(heap, HEAP_ZERO_MEMORY, 4096));
    wcscpy_s(wide, 2048, L"Heapglass UTF-16LE fixture: allocation generation and snapshot comparison.");
    log("utf16", heap, wide, 4096);
    auto utf8_text = static_cast<char *>(HeapAlloc(heap, HEAP_ZERO_MEMORY, 4096));
    strcpy_s(utf8_text, 4096, "Heapglass UTF-8 fixture: שלום 世界 😀");
    log("utf8-multilingual", heap, utf8_text, 4096);
    auto unicode_text = static_cast<wchar_t *>(HeapAlloc(heap, HEAP_ZERO_MEMORY, 4096));
    wcscpy_s(unicode_text, 2048, L"שלום 世界 Ελληνικά Привет 😀");
    log("utf16-multilingual", heap, unicode_text, 4096);
    auto random = static_cast<PUCHAR>(HeapAlloc(heap, 0, 65536));
    std::mt19937 rng(0x48475031);
    for (size_t i = 0; i < 65536; ++i)
        random[i] = uint8_t(rng());
    log("seeded-random", heap, random, 65536);
    auto zeros = HeapAlloc(heap, HEAP_ZERO_MEMORY, 8192);
    log("zeros", heap, zeros, 8192);
    auto compressed = static_cast<PUCHAR>(HeapAlloc(heap, HEAP_ZERO_MEMORY, 70000));
    COMPRESSOR_HANDLE compressor{};
    SIZE_T compressed_size = 0;
    if (CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &compressor)) {
        Compress(compressor, random, 65536, compressed, 70000, &compressed_size);
        CloseCompressor(compressor);
    }
    log("compressed", heap, compressed, compressed_size);
    auto cipher = static_cast<PUCHAR>(HeapAlloc(heap, HEAP_ZERO_MEMORY, 4096));
    auto key_bytes = static_cast<PUCHAR>(HeapAlloc(heap, HEAP_ZERO_MEMORY, 32));
    log("cng-buffer", heap, cipher, 4096);
    log("cng-key", heap, key_bytes, 32);
    BCRYPT_ALG_HANDLE aes{};
    BCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, nullptr, 0);
    BCryptSetProperty(aes, BCRYPT_CHAINING_MODE,
                      reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_ECB)),
                      sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
    auto executable = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    log("private-rw-page", nullptr, executable, 4096);
    auto inaccessible = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    log("noaccess", nullptr, inaccessible, 4096);
    auto guard = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_GUARD);
    log("guard", nullptr, guard, 4096);
    // Independent output describes fixture operations; the controller never reads this file.
    auto inplace = HeapAlloc(heap, 0, 1024);
    log("alloc", heap, inplace, 1024);
    auto smaller = HeapReAlloc(heap, HEAP_REALLOC_IN_PLACE_ONLY, inplace, 256);
    log("realloc-in-place-shrink", heap, smaller, 256, smaller != nullptr);
    if (smaller)
        inplace = smaller;
    auto moved = HeapReAlloc(heap, 0, inplace, 1024 * 1024);
    log("realloc-grow", heap, moved, 1024 * 1024, moved != nullptr);
    if (moved)
        inplace = moved;
    auto failed = HeapReAlloc(heap, 0, inplace, SIZE_MAX - 4096);
    log("realloc-failed-original-preserved", heap, inplace, SIZE_MAX - 4096, failed != nullptr);
    HeapFree(heap, 0, inplace);
    auto zero = HeapAlloc(heap, 0, 0);
    log("zero-size", heap, zero, 0, zero != nullptr);
    HeapFree(heap, 0, zero);
    HANDLE retired = HeapCreate(0, 0, 0);
    auto outstanding = HeapAlloc(retired, 0, 333);
    log("destroy-outstanding", retired, outstanding, 333);
    HeapDestroy(retired);
    std::atomic<bool> stop = false;
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            HANDLE local = HeapCreate(0, 0, 0);
            log("create-worker", local, local, 0);
            uint64_t count = 0;
            while (!stop) {
                for (unsigned j = 0; j < (burst ? 2048u : 8u); ++j) {
                    size_t size = 32 + ((count++ * 73 + t) % 2048);
                    void *p = HeapAlloc(local, 0, size);
                    if (p) {
                        memset(p, int(t + 65), size);
                        if (!burst)
                            log("alloc-worker", local, p, size);
                        if (count % 3 == 0) {
                            void *q = HeapReAlloc(local, 0, p, size + 128);
                            if (!burst)
                                log("realloc-worker", local, q, size + 128, q != nullptr);
                            if (q)
                                p = q;
                        }
                        BOOL ok = HeapFree(local, 0, p);
                        if (!burst)
                            log("free-worker", local, p, 0, ok != FALSE);
                    }
                }
                Sleep(burst ? 1 : 25);
            }
            HeapDestroy(local);
            log("destroy-worker", local, local, 0);
        });
    auto start = GetTickCount64();
    unsigned turn = 0;
    while (GetTickCount64() - start < uint64_t(duration) * 1000) {
        for (unsigned i = 0; i < 32; ++i)
            key_bytes[i] = uint8_t(i + turn);
        BCRYPT_KEY_HANDLE key{};
        if (BCryptGenerateSymmetricKey(aes, &key, nullptr, 0, key_bytes, 32, 0) >= 0) {
            ULONG output = 0;
            auto status = BCryptEncrypt(key, random, 4096, nullptr, nullptr, 0, cipher, 4096, &output, 0);
            log("cng-encrypt", heap, cipher, output, status >= 0);
            BCryptDestroyKey(key);
        }
        sprintf_s(ascii, 4096, "Heapglass fixture sample %u: ordinary writes are sampled, not intercepted.",
                  turn);
        DWORD protection = (turn % 9 < 3)   ? PAGE_READWRITE
                           : (turn % 9 < 6) ? PAGE_EXECUTE_READ
                                            : PAGE_EXECUTE_READWRITE;
        DWORD old = 0;
        VirtualProtect(executable, 4096, protection, &old);
        log("sampled-protection", nullptr, executable, protection);
        ++turn;
        Sleep(500);
    }
    stop = true;
    for (auto &t : threads)
        t.join();
    BCryptCloseAlgorithmProvider(aes, 0);
    VirtualFree(executable, 0, MEM_RELEASE);
    VirtualFree(inaccessible, 0, MEM_RELEASE);
    VirtualFree(guard, 0, MEM_RELEASE);
    HeapDestroy(heap);
    log("destroy-main-with-outstanding", heap, heap, 0);
    return 0;
}
