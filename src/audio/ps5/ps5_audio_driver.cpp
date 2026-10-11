/**
 * [ps5 port] sceAudioOut audio driver (see ps5_audio_driver.h).
 */
#include <rex/audio/ps5/ps5_audio_driver.h>

#include <algorithm>
#include <cstring>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/audio/flags.h>
#include <rex/logging.h>

extern "C" {
int32_t sceAudioOutInit(void);
int32_t sceAudioOutOpen(int32_t user_id, int32_t type, int32_t index, uint32_t len, uint32_t freq,
                        uint32_t param);
int32_t sceAudioOutOutput(int32_t handle, const void* ptr);
int32_t sceAudioOutClose(int32_t handle);
}

REXCVAR_DECLARE(bool, audio_mute);

namespace rex::audio::ps5 {

namespace {
constexpr int32_t kSystemUser = 0xFF;
constexpr int32_t kPortTypeMain = 0;
constexpr uint32_t kFormatS16Stereo = 1;
}  // namespace

Ps5AudioDriver::Ps5AudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

Ps5AudioDriver::~Ps5AudioDriver() { Shutdown(); }

bool Ps5AudioDriver::Initialize() {
  int32_t rc = sceAudioOutInit();
  // (Already initialized by someone else in the process is fine.)
  (void)rc;
  port_ = sceAudioOutOpen(kSystemUser, kPortTypeMain, 0, kChannelSamples, kFrequency,
                          kFormatS16Stereo);
  if (port_ < 0) {
    REXAPU_ERROR("PS5 audio: sceAudioOutOpen failed ({:#x})", uint32_t(port_));
    return false;
  }
  REXAPU_INFO("PS5 audio: port {} (48000 Hz, 256-sample frames, s16 stereo)", port_);
  running_ = true;
  thread_ = std::thread([this] { OutputThread(); });
  return true;
}

void Ps5AudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* frame;
  {
    std::lock_guard<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      frame = new float[kFrameSamples];
    } else {
      frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }
  std::memcpy(frame, input_frame, kFrameSamples * sizeof(float));
  std::lock_guard<std::mutex> guard(frames_mutex_);
  frames_queued_.push(frame);
}

// sceAudioOutOutput blocks until the previous buffer has started playing, so
// this loop runs at exactly the output rate (one frame per 5.33 ms): a queued
// game frame when there is one (then the game may submit another: release
// the semaphore), else silence.
void Ps5AudioDriver::OutputThread() {
  float stereo[kChannelSamples * 2];
  int16_t pcm[kChannelSamples * 2];
  while (running_) {
    float* frame = nullptr;
    {
      std::lock_guard<std::mutex> guard(frames_mutex_);
      if (!frames_queued_.empty()) {
        frame = frames_queued_.front();
        frames_queued_.pop();
      }
    }
    if (frame && !REXCVAR_GET(audio_mute)) {
      conversion::sequential_6_BE_to_interleaved_2_LE(stereo, frame, kChannelSamples,
                                                      GetStereoFold(), GetOutputGain());
      for (uint32_t i = 0; i < kChannelSamples * 2; ++i) {
        float s = std::clamp(stereo[i], -1.0f, 1.0f);
        pcm[i] = int16_t(s * 32767.0f);
      }
    } else {
      std::memset(pcm, 0, sizeof(pcm));
    }
    if (frame) {
      {
        std::lock_guard<std::mutex> guard(frames_mutex_);
        frames_unused_.push(frame);
      }
      semaphore_->Release(1, nullptr);
    }
    sceAudioOutOutput(port_, pcm);
  }
}

void Ps5AudioDriver::Shutdown() {
  if (running_.exchange(false) && thread_.joinable()) {
    thread_.join();
  }
  if (port_ >= 0) {
    sceAudioOutOutput(port_, nullptr);  // (waits for the last buffer)
    sceAudioOutClose(port_);
    port_ = -1;
  }
  std::lock_guard<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

}  // namespace rex::audio::ps5
