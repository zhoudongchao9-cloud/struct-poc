/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "cuttlefish/common/libs/fs/shared_fd.h"
#include "cuttlefish/host/frontend/webrtc/libcommon/audio_source.h"

namespace cuttlefish {

// Feeds a dedicated virtio-snd capture stream from the virtual tuner daemon's
// PCM socket. Emits silence whenever the daemon is absent or not tuned, so the
// guest always sees a continuous stream at the correct rate. Only 48 kHz,
// 16-bit, stereo is supported; any other requested format yields silence.
//
// GetMoreAudioData() calls must not overlap, though they may come from
// different threads over time. Reset() is safe to call from any thread.
class TunerAudioSource : public webrtc_streaming::AudioSource {
 public:
  // `reconnect_interval` rate-limits connection attempts while the daemon is
  // unreachable.
  explicit TunerAudioSource(
      std::string pcm_socket_path,
      std::chrono::milliseconds reconnect_interval = std::chrono::seconds(1));
  ~TunerAudioSource() override;

  TunerAudioSource(const TunerAudioSource&) = delete;
  TunerAudioSource& operator=(const TunerAudioSource&) = delete;

  int GetMoreAudioData(void* data, int bytes_per_sample,
                       int samples_per_channel, int num_channels,
                       int sample_rate, bool& muted) override;

  // Only sets a flag; the next GetMoreAudioData() call drops everything
  // received up to that point and re-primes.
  void Reset() override;

 private:
  void EnsureConnected();
  void DrainSocket();
  void TrimFifo();
  void CloseSocket();
  void LogStats() const;

  const std::string pcm_socket_path_;
  const std::chrono::milliseconds reconnect_interval_;
  std::chrono::steady_clock::time_point next_connect_attempt_;
  SharedFD fd_;
  std::vector<uint8_t> fifo_;
  bool primed_ = false;
  std::atomic<bool> reset_requested_{false};
  // Drift indicators: overflows mean the daemon runs faster than the guest
  // consumes, underruns mean it runs slower (or stopped sending).
  uint64_t overflow_count_ = 0;
  uint64_t underrun_count_ = 0;
};

}  // namespace cuttlefish
