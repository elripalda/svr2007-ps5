/**
 * @file        core/timeline.cpp
 * @brief       [svr2011] Microsecond event timeline (see rex/timeline.h).
 */

#include <rex/timeline.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(_WIN32)
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(timeline_file, "", "Debug",
                      "Record an event timeline (waits, signals, GPU fences) to this CSV file")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(timeline_delay, 60, "Debug", "Seconds after startup before recording")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(timeline_seconds, 3, "Debug", "Seconds to record")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::timeline {

namespace {

struct Entry {
  uint64_t ticks;
  uint32_t tid;
  uint16_t event;
  uint32_t a, b;
};

constexpr size_t kCapacity = 4 * 1024 * 1024;

std::atomic<int> g_state{0};  // 0 = not started, 1 = armed, 2 = recording, 3 = done
std::atomic<size_t> g_next{0};
std::unique_ptr<Entry[]> g_entries;
uint64_t g_start = 0, g_freq = 1;

uint64_t Now() {
#if defined(_WIN32)
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<uint64_t>(t.QuadPart);
#else
  return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

uint32_t ThreadId() {
#if defined(_WIN32)
  return GetCurrentThreadId();
#else
  return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

void Arm() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::string path = REXCVAR_GET(timeline_file);
    if (path.empty()) {
      g_state = 3;
      return;
    }
#if defined(_WIN32)
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = static_cast<uint64_t>(f.QuadPart);
#else
    g_freq = 1000000000ull;
#endif
    g_entries = std::make_unique<Entry[]>(kCapacity);
    g_state = 1;
    const int delay = REXCVAR_GET(timeline_delay);
    const int seconds = REXCVAR_GET(timeline_seconds);
    std::thread([path, delay, seconds] {
      std::this_thread::sleep_for(std::chrono::seconds(delay));
      g_start = Now();
      g_state = 2;
      REXLOG_INFO("timeline: recording {} s", seconds);
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
      g_state = 3;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let writers finish
      const size_t n = std::min(g_next.load(), kCapacity);
      if (FILE* f = std::fopen(path.c_str(), "w")) {
        std::fputs("time_us,tid,event,a,b\n", f);
        for (size_t i = 0; i < n; ++i) {
          const Entry& e = g_entries[i];
          std::fprintf(f, "%.1f,%u,%u,0x%08X,0x%08X\n",
                       (e.ticks - g_start) * 1e6 / static_cast<double>(g_freq), e.tid, e.event,
                       e.a, e.b);
        }
        std::fclose(f);
      }
      REXLOG_INFO("timeline: wrote {} events to {}", n, path);
    }).detach();
  });
}

}  // namespace

bool Enabled() {
  const int s = g_state.load(std::memory_order_relaxed);
  if (s == 0) {
    Arm();
    return false;
  }
  return s == 2;
}

void Record(uint16_t event, uint32_t a, uint32_t b) {
  if (g_state.load(std::memory_order_relaxed) != 2) {
    return;
  }
  const size_t i = g_next.fetch_add(1, std::memory_order_relaxed);
  if (i >= kCapacity) {
    return;
  }
  g_entries[i] = Entry{Now(), ThreadId(), event, a, b};
}

}  // namespace rex::timeline
