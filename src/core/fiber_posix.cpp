/**
 * @file        rex/core/fiber_posix.cpp
 * @brief       POSIX backend for rex::thread::Fiber (makecontext/swapcontext)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
// Darwin hides the deprecated ucontext API unless a POSIX/XSI feature level
// is requested before <ucontext.h> is included. fiber.h includes ucontext.h,
// so the feature macro must be set in this translation unit first.
#define _XOPEN_SOURCE 700
#endif

#include <rex/platform.h>
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC

#include <rex/thread/fiber.h>

#include <cassert>

#if REX_PLATFORM_ANDROID
// [svr2011] Android (bionic has no getcontext / makecontext / swapcontext):
// an arm64 switch of the callee-saved registers. rex_fiber_switch(&from_sp,
// to_sp) pushes x19-x30 and d8-d15, saves sp, loads to_sp and pops the same
// frame, returning into the other fiber (a new one "returns" to its entry).
static_assert(sizeof(void*) == 8, "arm64 only");
extern "C" void rex_fiber_switch(void** from_sp, void* to_sp);
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl rex_fiber_switch\n"
    ".type rex_fiber_switch, %function\n"
    "rex_fiber_switch:\n"
    "  sub sp, sp, #160\n"
    "  stp x19, x20, [sp, #0]\n"
    "  stp x21, x22, [sp, #16]\n"
    "  stp x23, x24, [sp, #32]\n"
    "  stp x25, x26, [sp, #48]\n"
    "  stp x27, x28, [sp, #64]\n"
    "  stp x29, x30, [sp, #80]\n"
    "  stp d8, d9, [sp, #96]\n"
    "  stp d10, d11, [sp, #112]\n"
    "  stp d12, d13, [sp, #128]\n"
    "  stp d14, d15, [sp, #144]\n"
    "  mov x2, sp\n"
    "  str x2, [x0]\n"
    "  mov sp, x1\n"
    "  ldp x19, x20, [sp, #0]\n"
    "  ldp x21, x22, [sp, #16]\n"
    "  ldp x23, x24, [sp, #32]\n"
    "  ldp x25, x26, [sp, #48]\n"
    "  ldp x27, x28, [sp, #64]\n"
    "  ldp x29, x30, [sp, #80]\n"
    "  ldp d8, d9, [sp, #96]\n"
    "  ldp d10, d11, [sp, #112]\n"
    "  ldp d12, d13, [sp, #128]\n"
    "  ldp d14, d15, [sp, #144]\n"
    "  add sp, sp, #160\n"
    "  ret\n"
    ".size rex_fiber_switch, .-rex_fiber_switch\n");
#endif

#if REX_PLATFORM_PS5
// [ps5 port] The PS5's getcontext/makecontext/swapcontext don't switch (tested
// on 13.60: 0 switches). An x86-64 switch of the callee-saved registers plus
// MXCSR and the x87 control word, tested on the console (~0.03 us a round
// trip). rex_fiber_switch(&from_sp, to_sp): pushes rbp rbx r12-r15 and an
// 8-byte MXCSR/FCW slot, saves rsp, loads to_sp, pops the same frame, returns.
extern "C" void rex_fiber_switch(void** from_sp, void* to_sp);
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl rex_fiber_switch\n"
    ".type rex_fiber_switch, @function\n"
    "rex_fiber_switch:\n"
    "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
    "  subq $8, %rsp\n  stmxcsr (%rsp)\n  fnstcw 4(%rsp)\n"
    "  movq %rsp, (%rdi)\n"
    "  movq %rsi, %rsp\n"
    "  ldmxcsr (%rsp)\n  fldcw 4(%rsp)\n  addq $8, %rsp\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n"
    "  ret\n"
    ".size rex_fiber_switch, .-rex_fiber_switch\n");
#endif

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

#if REX_PLATFORM_ANDROID || REX_PLATFORM_PS5
Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();  // (its sp_ is set by the first switch away)
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);
  uintptr_t top = (reinterpret_cast<uintptr_t>(f->stack_.data()) + stack_size) & ~uintptr_t(15);
#if REX_PLATFORM_PS5
  // The frame rex_fiber_switch pops (x86-64): [mxcsr|fcw] r15 r14 r13 r12 rbx
  // rbp, then the return address = Trampoline. The return slot sits on a
  // 16-byte boundary so Trampoline starts with rsp % 16 == 8, as after a call.
  auto* frame = reinterpret_cast<uint64_t*>(top - 16 - 7 * 8);
  for (int i = 0; i < 7; ++i) frame[i] = 0;
  uint32_t mxcsr;
  uint16_t fcw;
  __asm__ volatile("stmxcsr %0\n fnstcw %1" : "=m"(mxcsr), "=m"(fcw));
  frame[0] = (uint64_t(fcw) << 32) | mxcsr;
  frame[7] = reinterpret_cast<uint64_t>(&Fiber::Trampoline);
  frame[8] = 0;
#else
  // The frame rex_fiber_switch pops: zeroes, and x30 (at +88) the entry.
  auto* frame = reinterpret_cast<uint64_t*>(top - 160);
  for (int i = 0; i < 20; ++i) frame[i] = 0;
  frame[11] = reinterpret_cast<uint64_t>(&Fiber::Trampoline);
#endif
  f->sp_ = frame;
  return f;
}

/*static*/ void Fiber::Trampoline() {
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
  __builtin_trap();  // (a fiber's entry never returns)
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  rex_fiber_switch(&from->sp_, target->sp_);
}
#else

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);

  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->context_.uc_stack.ss_sp = f->stack_.data();
  f->context_.uc_stack.ss_size = f->stack_.size();
  f->context_.uc_link = nullptr;
  // Trampoline reads entry_/arg_ from tls_current_ — no pointer splitting needed.
  makecontext(&f->context_, &Fiber::Trampoline, 0);
  return f;
}

/*static*/ void Fiber::Trampoline() {
  // tls_current_ was updated by SwitchTo before swapcontext returned here.
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  swapcontext(&from->context_, &target->context_);
}
#endif  // REX_PLATFORM_ANDROID

void Fiber::Destroy() {
  // Thread fibers are destroyed from the owning thread itself.
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  // No POSIX equivalent of ConvertFiberToThread; stack_ is freed by the vector destructor.
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_LINUX || REX_PLATFORM_MAC
