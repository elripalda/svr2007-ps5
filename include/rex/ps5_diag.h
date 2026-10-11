#pragma once
/**
 * [ps5 port] Progress counters for bringing the game up on the PS5: a
 * monitor thread logs them every 10 s (windowed_app_main_sdl.cpp), so a
 * black screen shows which stage stopped moving. Cheap relaxed atomics.
 */
#include <atomic>
#include <cstdint>

namespace rex::ps5diag {
enum Counter : int {
  kAccessViolations,    // faults reaching the MMIO handler
  kMmioAccesses,        // of those, GPU register reads / writes emulated
  kWatchFaults,         // of those, write watches / other handlers
  kUnhandledFaults,     // of those, not handled (a real crash)
  kVblanks,             // GPU vblanks
  kInterrupts,          // guest GPU interrupt callbacks run
  kWritePointer,        // ring buffer write pointer updates by the game
  kPrimaryBuffers,      // ring buffer batches executed
  kCount
};
extern std::atomic<uint64_t> counters[kCount];
// The reserved guest address space (all views live inside it).
extern uintptr_t guest_arena_lo, guest_arena_hi;
inline void Add(Counter c) { counters[c].fetch_add(1, std::memory_order_relaxed); }

// Guest threads blocked in a kernel wait: one slot per host thread, filled on
// entering XObject::Wait / WaitMultiple / SignalAndWait and cleared on leaving.
struct WaitSlot {
  std::atomic<uint64_t> since_ms{0};  // 0: not waiting
  uint32_t thread_id = 0;             // guest thread id
  uint32_t object_type = 0;           // XObject::Type (first object)
  uint32_t handle = 0;                // its handle
  uint32_t count = 0;                 // objects waited on
  uint32_t guest_lr = 0;              // the game code that waits
  uint32_t timeout_ms = 0;            // 0xFFFFFFFF: infinite
  const void* critical_section = nullptr;
  uint32_t critical_section_guest = 0;
  uint32_t critical_section_lr = 0;  // the game code that entered it  // host address of a contended X_RTL_CRITICAL_SECTION
};
constexpr int kWaitSlots = 256;
extern WaitSlot wait_slots[kWaitSlots];
WaitSlot* CurrentWaitSlot();  // (nullptr when out of slots)
uint64_t NowMs();
const char* ObjectTypeName(uint32_t type);

// Host stack samples: every rex thread registers itself; SampleAllThreads()
// signals each one (SIGPROF), records its rip and the return-address-looking
// words on its stack, and logs them relative to Anchor() so they can be
// symbolized against the linked ELF offline.
void RegisterThread(const char* what);
void SampleAllThreads();
// A host fault no handler took (a crash in some thread): logged with the
// thread's return addresses; the thread then re-faults, so it is logged a few
// times only.
void UnhandledHostFault(uintptr_t rip, uintptr_t address, uintptr_t rsp);
// Called (once) when a thread keeps faulting: tells the player and ends the
// game rather than leave it hanging. Set by main.
extern void (*crash_hook)(const char* what);
uintptr_t Anchor();

// Critical sections the game initialized (guest address -> game code).
void NoteCriticalSectionInit(uint32_t guest, uint32_t lr);
uint32_t CriticalSectionInitBy(uint32_t guest);  // 0: never seen
}  // namespace rex::ps5diag
