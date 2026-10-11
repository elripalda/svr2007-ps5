#pragma once
/**
 * [ps5 port] Audio out through sceAudioOut: the Xbox 360's 48 kHz frames of
 * 256 samples x 6 channels, folded to stereo 16-bit (the port format the
 * UnleashedRecomp PS5 port verified), pushed by a thread of its own.
 */
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <stack>
#include <thread>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

namespace rex::audio::ps5 {

class Ps5AudioDriver : public AudioDriver {
 public:
  Ps5AudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~Ps5AudioDriver() override;

  bool Initialize();
  void SubmitFrame(uint32_t frame_ptr) override;
  void Shutdown();

 private:
  void OutputThread();

  static constexpr uint32_t kFrequency = 48000;
  static constexpr uint32_t kChannels = 6;
  static constexpr uint32_t kChannelSamples = 256;
  static constexpr uint32_t kFrameSamples = kChannels * kChannelSamples;

  rex::thread::Semaphore* semaphore_ = nullptr;
  int32_t port_ = -1;
  std::thread thread_;
  std::atomic<bool> running_{false};

  std::queue<float*> frames_queued_;
  std::stack<float*> frames_unused_;
  std::mutex frames_mutex_;
};

}  // namespace rex::audio::ps5
