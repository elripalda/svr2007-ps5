/**
 * @file        ui/windowed_app_main_sdl.cpp
 * @brief       Entry point for windowed applications (SDL3 windowing)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_sdl.h>

#if REX_PLATFORM_ANDROID
// [svr2011] main becomes SDL_main, which SDL's Java activity calls.
#include <SDL3/SDL_main.h>
#include <rex/main_android.h>
#endif

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#endif

namespace {

#if REX_PLATFORM_PS5
void Ps5EarlyFailure(const char* what);  // (below)
#endif

int RunWindowedApp(int argc, char** argv) {
#if REX_PLATFORM_ANDROID
  rex::InitializeAndroid();  // [svr2011]
#endif
  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

  int result;
  {
    rex::ui::SDLWindowedAppContext app_context;
    if (!app_context.Initialize()) {
#if REX_PLATFORM_PS5
      // (before the log file exists: the reason on the screen)
      Ps5EarlyFailure("SDL didn't start");
#endif
      return EXIT_FAILURE;
    }

#if REX_PLATFORM_WIN32
    // Apartment-threaded COM for shell dialogs.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
      return EXIT_FAILURE;
    }
#endif

    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);

    // Match remaining positional args to the app's expected options.
    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    result = app->OnInitialize() ? app_context.RunMainMessageLoop() : EXIT_FAILURE;

    app->InvokeOnDestroy();
  }

#if REX_PLATFORM_WIN32
  CoUninitialize();
#endif

  return result;
}

#if REX_PLATFORM_WIN32
// Convert wide argv from CommandLineToArgvW to UTF-8 for cvar::Init.
std::vector<std::string> WideArgsToUtf8(int argc, wchar_t** wargv) {
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    std::wstring wide(wargv[i]);
    if (wide.empty()) {
      args.emplace_back();
      continue;
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr,
                                   0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size,
                        nullptr, nullptr);
    args.push_back(std::move(utf8));
  }
  return args;
}
#endif

}  // namespace

#if REX_PLATFORM_WIN32

int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE hinstance_prev, LPWSTR command_line,
                    int show_cmd) {
  (void)hinstance;
  (void)hinstance_prev;
  (void)command_line;
  (void)show_cmd;

  int wargc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  auto utf8_args = WideArgsToUtf8(wargc, wargv);
  LocalFree(wargv);

  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(utf8_args.size());
  for (auto& s : utf8_args) {
    argv_ptrs.push_back(s.data());
  }
  return RunWindowedApp(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
}

#elif REX_PLATFORM_PS5

// [ps5 port] Work in the app's folder (logs/, UserData/, Saves/ go there; the
// process starts elsewhere, often read-only), and show a notification so the
// player can tell the app started at all.
#include <cstring>
#include <unistd.h>
extern "C" int sceKernelSendNotificationRequest(int, void*, size_t, int);
extern "C" int sceSystemServiceLoadExec(const char* path, char* const argv[]);
static void Ps5Notify(const char* text) {
  struct {
    char unused[45];
    char message[3075];
  } request;
  std::memset(&request, 0, sizeof(request));
  std::strncpy(request.message, text, sizeof(request.message) - 1);
  sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

#include <SDL3/SDL_error.h>
namespace {
void Ps5EarlyFailure(const char* what) {
  char text[600];
  std::snprintf(text, sizeof(text), "%s: %s", what, SDL_GetError());
  Ps5Notify(text);
}
}  // namespace

// An uncaught C++ exception ends in abort() with its message on stderr, which
// nobody sees on a PS5: put it in the log and on screen first.
#include <exception>
#include <typeinfo>
static void Ps5Terminate() {
  char text[600] = "Uncaught exception";
  if (auto current = std::current_exception()) {
    try {
      std::rethrow_exception(current);
    } catch (const std::exception& e) {
      std::snprintf(text, sizeof(text), "Uncaught %s: %s", typeid(e).name(), e.what());
    } catch (...) {
    }
  }
  REXLOG_ERROR("{}", text);
  Ps5Notify(text);
  sleep(2);  // (let the notification show)
  std::abort();
}

// Every 10 s: the progress counters (rex/ps5_diag.h), so a black screen's
// log shows which stage stopped moving.
#include <rex/ps5_diag.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <thread>
static void Ps5DiagMonitor() {
  using namespace rex::ps5diag;
  for (int tick = 1;; ++tick) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    REXLOG_INFO(
        "ps5 diag {}s: faults {} (mmio {}, watch {}, unhandled {}), vblanks {}, interrupts {}, "
        "ring writes {}, ring batches {}",
        tick * 10, counters[kAccessViolations].load(), counters[kMmioAccesses].load(),
        counters[kWatchFaults].load(), counters[kUnhandledFaults].load(), counters[kVblanks].load(),
        counters[kInterrupts].load(), counters[kWritePointer].load(),
        counters[kPrimaryBuffers].load());
    // Guest threads stuck in one kernel wait for over 5 s.
    const uint64_t now = NowMs();
    for (WaitSlot& w : wait_slots) {
      uint64_t since = w.since_ms.load(std::memory_order_acquire);
      if (since && since < now && now - since > 5000) {
        REXLOG_INFO("ps5 diag:   guest thread {:X} waiting {}s on {} {} handle {:08X}{} from game code {:08X}",
                    w.thread_id, (now - since) / 1000, w.count > 1 ? w.count : 1,
                    ObjectTypeName(w.object_type), w.handle, w.count > 1 ? " (first of several)" : "",
                    w.guest_lr);
        // its callers: each frame's back chain at 0(r1), the caller's LR
        // saved at -8 from the caller's frame (mflr r12; stw r12,-8(r1);
        // stwu ...) - read only where the guest memory is committed
        if (auto* kernel = rex::system::kernel_state(); kernel && w.guest_sp && w.guest_stack_lo) {
          auto* memory = kernel->memory();
          auto readable = [&](uint32_t a) {
            uint32_t protect = 0;
            auto* heap = memory->LookupHeap(a);
            return heap && heap->QueryProtect(a, &protect) && (protect & rex::memory::kMemoryProtectRead);
          };
          std::string callers;
          uint32_t sp = w.guest_sp;
          for (int i = 0; i < 8; ++i) {
            if (sp < w.guest_stack_lo || sp + 4 > w.guest_stack_hi || (sp & 3) || !readable(sp)) break;
            const uint32_t back = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(sp));
            if (back <= sp || back < w.guest_stack_lo + 8 || back > w.guest_stack_hi || !readable(back - 8)) break;
            callers += fmt::format(" < {:08X}",
                                   rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(back - 8)));
            sp = back;
          }
          if (!callers.empty()) REXLOG_INFO("ps5 diag:     called from{}", callers);
        }
        if (const auto* cs = static_cast<const uint8_t*>(w.critical_section)) {
          auto be32 = [cs](int off) {
            return uint32_t(cs[off]) << 24 | uint32_t(cs[off + 1]) << 16 |
                   uint32_t(cs[off + 2]) << 8 | uint32_t(cs[off + 3]);
          };
          auto le32 = [cs](int off) {
            return uint32_t(cs[off]) | uint32_t(cs[off + 1]) << 8 | uint32_t(cs[off + 2]) << 16 |
                   uint32_t(cs[off + 3]) << 24;
          };
          std::string raw;
          for (int b = 0; b < 28; ++b) raw += fmt::format("{:02X}{}", cs[b], b % 4 == 3 ? " " : "");
          REXLOG_INFO("ps5 diag:     critical section {:08X} entered from {:08X}: lock count {}, recursion {}, "
                      "held by kthread {:08X}; initialized {}; bytes {}",
                      w.critical_section_guest, w.critical_section_lr, int32_t(le32(0x10)),
                      int32_t(be32(0x14)), be32(0x18),
                      CriticalSectionInitBy(w.critical_section_guest)
                          ? fmt::format("by game code {:08X}", CriticalSectionInitBy(w.critical_section_guest))
                          : std::string("never (by RtlInitializeCriticalSection)"),
                      raw);
        }
      }
    }
    // The game stopped submitting GPU work: show where every thread is, twice.
    static uint64_t last_writes = 0, stuck_ticks = 0, samples = 0;
    uint64_t writes = counters[kWritePointer].load();
    stuck_ticks = writes == last_writes ? stuck_ticks + 1 : 0;
    last_writes = writes;
    if ((stuck_ticks == 3 || stuck_ticks == 6) && samples < 2) {
      ++samples;
      SampleAllThreads();
    }
  }
}

int main(int argc, char* argv[]) {
  chdir("/app0");
  rex::ps5diag::RegisterThread("main");
  // A thread that crashed (it would retry its faulting instruction forever,
  // behind a frozen loading screen): say so and close the game.
  rex::ps5diag::crash_hook = [](const char* what) {
    REXLOG_ERROR("{} - closing (the log above says where)", what);
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& l) { l->flush(); });
    Ps5Notify("The game crashed - send /app0/logs to the developer");
    sleep(2);
    _exit(1);
  };
  // EXIT in the game's main menu: start the app again, into its hub.
  rex::ps5diag::title_exit_hook = [] {
    REXLOG_INFO("back to the hub (restarting the app)");
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& l) { l->flush(); });
    char* args[] = {nullptr};
    const int rc = sceSystemServiceLoadExec("/app0/eboot.bin", args);
    REXLOG_ERROR("restarting failed ({:#x}) - closing", uint32_t(rc));
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& l) { l->flush(); });
    _exit(0);
  };
  std::set_terminate(Ps5Terminate);
  std::thread(Ps5DiagMonitor).detach();
  int result = RunWindowedApp(argc, argv);
  if (result != 0) Ps5Notify("Stopped with an error - see /app0/logs");
  return result;
}

#else

int main(int argc, char* argv[]) {
  return RunWindowedApp(argc, argv);
}

#endif
