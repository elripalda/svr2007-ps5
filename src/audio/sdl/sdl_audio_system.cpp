/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#if REX_PLATFORM_PS5
#include <rex/audio/ps5/ps5_audio_driver.h>
#endif
#include <rex/audio/sdl/sdl_audio_system.h>

namespace rex::audio::sdl {

std::unique_ptr<AudioSystem> SDLAudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<SDLAudioSystem>(function_dispatcher);
}

SDLAudioSystem::SDLAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

SDLAudioSystem::~SDLAudioSystem() {}

void SDLAudioSystem::Initialize() {
  AudioSystem::Initialize();
}

X_STATUS SDLAudioSystem::CreateDriver([[maybe_unused]] size_t index,
                                      rex::thread::Semaphore* semaphore, AudioDriver** out_driver) {
  assert_not_null(out_driver);
#if REX_PLATFORM_PS5
  // [ps5 port] SDL has no PS5 audio backend: sceAudioOut directly.
  auto ps5_driver = new ps5::Ps5AudioDriver(memory_, semaphore);
  if (!ps5_driver->Initialize()) {
    delete ps5_driver;
    return X_STATUS_UNSUCCESSFUL;
  }
  *out_driver = ps5_driver;
  return X_STATUS_SUCCESS;
#endif
  auto driver = new SDLAudioDriver(memory_, semaphore);
  if (!driver->Initialize()) {
    driver->Shutdown();
    delete driver;
    return X_STATUS_UNSUCCESSFUL;
  }

  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void SDLAudioSystem::DestroyDriver(AudioDriver* driver) {
  assert_not_null(driver);
#if REX_PLATFORM_PS5
  if (auto* ps5_driver = dynamic_cast<ps5::Ps5AudioDriver*>(driver)) {
    ps5_driver->Shutdown();
    delete ps5_driver;
    return;
  }
#endif
  auto sdldriver = dynamic_cast<SDLAudioDriver*>(driver);
  assert_not_null(sdldriver);
  sdldriver->Shutdown();
  delete sdldriver;
}

}  // namespace rex::audio::sdl
