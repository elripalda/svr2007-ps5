// [ps5 port] see rex/ps5_diag.h
#include <rex/ps5_diag.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <pthread.h>
#include <pthread_np.h>
#include <signal.h>
#include <unistd.h>

#include <rex/logging.h>

namespace rex::ps5diag {
std::atomic<uint64_t> counters[kCount];
WaitSlot wait_slots[kWaitSlots];
uintptr_t guest_arena_lo = 0, guest_arena_hi = 0;
void (*crash_hook)(const char* what) = nullptr;
void (*title_exit_hook)() = nullptr;

namespace {
std::atomic<int> next_slot{0};
}

WaitSlot* CurrentWaitSlot() {
  thread_local int index = -1;
  if (index < 0) index = next_slot.fetch_add(1);
  return index < kWaitSlots ? &wait_slots[index] : nullptr;
}

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

const char* ObjectTypeName(uint32_t type) {
  static const char* const kNames[] = {"Undefined", "Enumerator", "Event",     "File",
                                       "IOCompletion", "Module",  "Mutant",    "NotifyListener",
                                       "Semaphore", "Session",    "Socket",    "SymbolicLink",
                                       "Thread",    "Timer"};
  return type < sizeof(kNames) / sizeof(kNames[0]) ? kNames[type] : "?";
}

namespace {
struct ThreadEntry {
  std::atomic<bool> used{false};
  pthread_t pthread{};
  int tid = 0;
  uintptr_t stack_top = 0;
  char what[24] = {};
  // filled by the signal handler
  std::atomic<int> done{0};
  uintptr_t rip = 0, rsp = 0;
  uintptr_t words[64] = {};
  int nwords = 0;
};
constexpr int kThreads = 512;
ThreadEntry threads[kThreads];
std::atomic<int> next_thread{0};
thread_local ThreadEntry* my_entry = nullptr;

constexpr uintptr_t kNear = 256ull << 20;  // the eboot text is ~100 MB

void SampleHandler(int, siginfo_t*, void* uc) {
  ThreadEntry* e = my_entry;
  if (!e) return;
  auto* gregs = reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(uc) + 64);
  e->rip = gregs[20];
  e->rsp = gregs[23];
  uintptr_t anchor = Anchor();
  uintptr_t end = e->rsp + 64 * 1024;
  if (e->stack_top && e->stack_top < end) end = e->stack_top;
  int n = 0;
  for (uintptr_t p = e->rsp & ~uintptr_t(7); p + 8 <= end && n < 64; p += 8) {
    uintptr_t v = *reinterpret_cast<uintptr_t*>(p);
    if (v > (anchor > kNear ? anchor - kNear : 0x10000) && v < anchor + kNear) e->words[n++] = v;
  }
  e->nwords = n;
  e->done.store(1, std::memory_order_release);
}
}  // namespace

namespace {
std::mutex cs_mutex;
std::unordered_map<uint32_t, uint32_t>& CsInits() {
  static auto* m = new std::unordered_map<uint32_t, uint32_t>();
  return *m;
}
}  // namespace
void NoteCriticalSectionInit(uint32_t guest, uint32_t lr) {
  std::lock_guard<std::mutex> lock(cs_mutex);
  CsInits()[guest] = lr ? lr : 1;
}
uint32_t CriticalSectionInitBy(uint32_t guest) {
  std::lock_guard<std::mutex> lock(cs_mutex);
  auto it = CsInits().find(guest);
  return it == CsInits().end() ? 0 : it->second;
}

void UnhandledHostFault(uintptr_t rip, uintptr_t address, uintptr_t rsp) {
  static std::atomic<uint64_t> count{0};
  const uint64_t n = ++count;
  if (n == 1000 && crash_hook) {
    crash_hook("a thread crashed");  // (doesn't return)
  }
  if (n > 5 && n % 200000 != 0) return;
  const uintptr_t anchor = Anchor();
  std::string line;
  if (ThreadEntry* e = my_entry) {
    uintptr_t end = rsp + 64 * 1024;
    if (e->stack_top && e->stack_top < end) end = e->stack_top;
    int k = 0;
    for (uintptr_t p = rsp & ~uintptr_t(7); p + 8 <= end && k < 48; p += 8) {
      uintptr_t v = *reinterpret_cast<uintptr_t*>(p);
      if (v > (anchor > kNear ? anchor - kNear : 0x10000) && v < anchor + kNear) {
        char buf[24];
        snprintf(buf, sizeof(buf), " %lX", (unsigned long)(v - anchor + 0x10000000));
        line += buf;
        ++k;
      }
    }
  }
  REXLOG_ERROR("ps5 crash: thread {} ({}) faulted at {:X} (rip {:X}, anchor-relative {:X}); fault #{} | stack:{}",
               my_entry ? my_entry->tid : pthread_getthreadid_np(), my_entry ? my_entry->what : "unregistered",
               address, rip, rip - anchor + 0x10000000, n, line);
}

uintptr_t Anchor() { return reinterpret_cast<uintptr_t>(&NowMs); }

void RegisterThread(const char* what) {
  if (my_entry) return;
  int i = next_thread.fetch_add(1);
  if (i >= kThreads) return;
  ThreadEntry& e = threads[i];
  e.pthread = pthread_self();
  e.tid = pthread_getthreadid_np();
  int marker = 0;
  e.stack_top = reinterpret_cast<uintptr_t>(&marker) + 256;  // (a little above: the caller's frame)
  std::strncpy(e.what, what ? what : "", sizeof(e.what) - 1);
  my_entry = &e;
  e.used.store(true, std::memory_order_release);
}

void SampleAllThreads() {
  static bool installed = false;
  if (!installed) {
    struct sigaction sa = {};
    sa.sa_sigaction = SampleHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
    installed = true;
  }
  REXLOG_INFO("ps5 stack: anchor {:X}", Anchor());
  int count = next_thread.load();
  if (count > kThreads) count = kThreads;
  for (int i = 0; i < count; ++i) {
    ThreadEntry& e = threads[i];
    if (!e.used.load(std::memory_order_acquire)) continue;
    e.done.store(0);
    if (pthread_kill(e.pthread, SIGPROF) != 0) continue;
    for (int ms = 0; ms < 200 && !e.done.load(std::memory_order_acquire); ++ms) usleep(1000);
    if (!e.done.load()) {
      REXLOG_INFO("ps5 stack: tid {} {}: no answer", e.tid, e.what);
      continue;
    }
    std::string line;
    for (int k = 0; k < e.nwords; ++k) {
      char buf[24];
      snprintf(buf, sizeof(buf), " %lX", (unsigned long)(e.words[k] - Anchor() + 0x10000000));
      line += buf;
    }
    REXLOG_INFO("ps5 stack: tid {} {}: rip {:X} |{}", e.tid, e.what,
                e.rip - Anchor() + 0x10000000, line);
  }
}
}  // namespace rex::ps5diag
