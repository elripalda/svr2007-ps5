/**
 * @file        rex/timeline.h
 * @brief       [svr2011] Microsecond event timeline for latency analysis.
 *
 * With --timeline_file=<path>, events from every thread (guest waits and
 * signals, GPU fence waits, interrupts, command submission, presents) are
 * recorded into a preallocated buffer for --timeline_seconds after
 * --timeline_delay seconds, then written as CSV:
 *   time_us,host_tid,event,a,b
 * Recording costs one atomic increment per event; with no file set it is a
 * single branch.
 */

#pragma once

#include <cstdint>

namespace rex::timeline {

enum Event : uint16_t {
  kWaitBegin = 1,    // a = object (guest ptr or handle), b = kind
  kWaitEnd,          // a = object, b = result
  kSignal,           // a = object, b = kind
  kDelayBegin,       // a = interval (100 ns units, low 32 bits)
  kDelayEnd,
  kGpuWaitBegin,     // a = polled address/register, b = reference
  kGpuWaitEnd,       // a = polled address/register, b = polls
  kInterruptBegin,   // a = source, b = cpu
  kInterruptEnd,
  kPresent,          // a = frame
  kVdSwap,
  kWritePointer,     // a = new ring buffer write pointer
  kVblank,
  kUser = 100,       // app-defined
};

bool Enabled();
void Record(uint16_t event, uint32_t a = 0, uint32_t b = 0);

}  // namespace rex::timeline

#define REX_TIMELINE(ev, ...)                                   \
  do {                                                          \
    if (::rex::timeline::Enabled())                             \
      ::rex::timeline::Record(::rex::timeline::ev, ##__VA_ARGS__); \
  } while (0)
