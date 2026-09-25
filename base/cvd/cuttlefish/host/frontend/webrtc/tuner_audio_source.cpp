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

#include "cuttlefish/host/frontend/webrtc/tuner_audio_source.h"

#include <sys/socket.h>  // IWYU pragma: keep
#include <sys/types.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"

#include "cuttlefish/common/libs/fs/shared_fd.h"

namespace cuttlefish {
namespace {

// Format produced by the virtual tuner daemon.
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kBytesPerSample = sizeof(int16_t);
constexpr size_t kFrameBytes = kChannels * kBytesPerSample;
constexpr size_t kBytesPer10Ms = kSampleRate / 100 * kFrameBytes;

// Two 10 ms periods of headroom before serving, enough to absorb host
// scheduling jitter without adding meaningful latency.
constexpr size_t kPrimeBytes = 2 * kBytesPer10Ms;

// Hard ceiling on buffered audio. Anything beyond this is stale by definition,
// so the oldest frames are dropped rather than played late.
constexpr size_t kMaxFifoBytes = 10 * kBytesPer10Ms;  // 100 ms

constexpr size_t kReadChunkBytes = 4096;

}  // namespace

TunerAudioSource::TunerAudioSource(std::string pcm_socket_path,
                                   std::chrono::milliseconds reconnect_interval)
    : pcm_socket_path_(std::move(pcm_socket_path)),
      reconnect_interval_(reconnect_interval) {
  fifo_.reserve(kMaxFifoBytes);
}

TunerAudioSource::~TunerAudioSource() { CloseSocket(); }

int TunerAudioSource::GetMoreAudioData(void* data, int bytes_per_sample,
                                       int samples_per_channel,
                                       int num_channels, int sample_rate,
                                       bool& muted) {
  muted = false;
  const size_t bytes_needed = static_cast<size_t>(samples_per_channel) *
                              num_channels * bytes_per_sample;

  EnsureConnected();
  DrainSocket();
  if (reset_requested_.exchange(false)) {
    // Everything received so far was produced while the guest stream was
    // stopped.
    fifo_.clear();
    primed_ = false;
  }
  TrimFifo();

  if (bytes_per_sample != kBytesPerSample || num_channels != kChannels ||
      sample_rate != kSampleRate) {
    LOG_FIRST_N(WARNING, 1)
        << "Unsupported tuner capture format: " << sample_rate << " Hz, "
        << num_channels << " ch, " << bytes_per_sample * 8 << " bit";
    // Keep draining the socket, but drop the stereo PCM.
    fifo_.clear();
    primed_ = false;
    std::memset(data, 0, bytes_needed);
    return samples_per_channel;
  }

  if (!primed_) {
    if (fifo_.size() < kPrimeBytes) {
      std::memset(data, 0, bytes_needed);
      return samples_per_channel;
    }
    primed_ = true;
  }

  if (fifo_.size() < bytes_needed) {
    // Underrun: the daemon is idle or fell behind. Emit silence and re-prime so
    // a single late delivery doesn't cause repeated stuttering.
    primed_ = false;
    ++underrun_count_;
    LogStats();
    std::memset(data, 0, bytes_needed);
    return samples_per_channel;
  }

  std::memcpy(data, fifo_.data(), bytes_needed);
  fifo_.erase(fifo_.begin(), fifo_.begin() + bytes_needed);
  return samples_per_channel;
}

void TunerAudioSource::Reset() { reset_requested_.store(true); }

void TunerAudioSource::EnsureConnected() {
  if (fd_->IsOpen()) {
    return;
  }
  const std::chrono::steady_clock::time_point now =
      std::chrono::steady_clock::now();
  if (now < next_connect_attempt_) {
    return;
  }
  next_connect_attempt_ = now + reconnect_interval_;
  fd_ = SharedFD::SocketLocalClient(pcm_socket_path_, false,
                                    SOCK_STREAM | SOCK_NONBLOCK);
  if (fd_->IsOpen()) {
    LOG(INFO) << "Tuner audio source connected to " << pcm_socket_path_;
  }
}

void TunerAudioSource::DrainSocket() {
  if (!fd_->IsOpen()) {
    return;
  }
  uint8_t chunk[kReadChunkBytes];
  while (true) {
    const ssize_t bytes_read = fd_->Recv(chunk, sizeof(chunk), MSG_DONTWAIT);
    if (bytes_read > 0) {
      fifo_.insert(fifo_.end(), chunk, chunk + bytes_read);
      continue;
    }
    if (bytes_read == 0) {
      LOG(INFO) << "Tuner audio source disconnected";
      CloseSocket();
      return;
    }
    const int err = fd_->GetErrno();
    if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
      return;
    }
    LOG(ERROR) << "Tuner audio source read failed: " << fd_->StrError();
    CloseSocket();
    return;
  }
}

void TunerAudioSource::TrimFifo() {
  if (fifo_.size() <= kMaxFifoBytes) {
    return;
  }
  // Drop whole frames only, so the head of the FIFO stays aligned even if the
  // tail holds a partially received frame.
  size_t excess = fifo_.size() - kMaxFifoBytes;
  excess += (kFrameBytes - excess % kFrameBytes) % kFrameBytes;
  fifo_.erase(fifo_.begin(), fifo_.begin() + excess);
  ++overflow_count_;
  LogStats();
}

void TunerAudioSource::CloseSocket() {
  fd_ = SharedFD();
  fifo_.clear();
  primed_ = false;
}

void TunerAudioSource::LogStats() const {
  LOG_EVERY_N_SEC(INFO, 10)
      << "Tuner audio: " << overflow_count_ << " overflows, " << underrun_count_
      << " underruns";
}

}  // namespace cuttlefish
