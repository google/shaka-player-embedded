// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/media/audio_renderer_common.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>

namespace shaka {
namespace media {

namespace {

/** The number of seconds to buffer ahead of the current time. */
const double kBufferTarget = 2;

/** The minimum difference, in seconds, to introduce silence or drop frames. */
const double kSyncLimit = 0.1;

/** A buffer that contains silence. */
const uint8_t kSilenceBuffer[4096] = {0};


uint8_t BytesPerSample(std::shared_ptr<DecodedFrame> frame) {
  switch (get<SampleFormat>(frame->format)) {
    case SampleFormat::PackedU8:
    case SampleFormat::PlanarU8:
      return 1;
    case SampleFormat::PackedS16:
    case SampleFormat::PlanarS16:
      return 2;
    case SampleFormat::PackedS32:
    case SampleFormat::PlanarS32:
      return 4;
    case SampleFormat::PackedS64:
    case SampleFormat::PlanarS64:
      return 8;
    case SampleFormat::PackedFloat:
    case SampleFormat::PlanarFloat:
      return 4;
    case SampleFormat::PackedDouble:
    case SampleFormat::PlanarDouble:
      return 8;
    default:
      LOG(DFATAL) << "Unsupported sample format: " << frame->format;
      return 0;
  }
}

size_t BytesToSamples(std::shared_ptr<DecodedFrame> frame, size_t bytes) {
  size_t bytes_per_sample =
      BytesPerSample(frame) * frame->stream_info->channel_count;
  return bytes / bytes_per_sample;
}

double BytesToSeconds(std::shared_ptr<DecodedFrame> frame, size_t bytes) {
  return static_cast<double>(BytesToSamples(frame, bytes)) /
         frame->stream_info->sample_rate;
}

/**
 * Calculates the byte sync needed to play the next frame.
 *
 * @param prev_time The previous synchronized time.
 * @param bytes_written The number of bytes written since |prev_time|.
 * @param next The next frame to be played.
 * @return If positive, the number of bytes to skip in |next|; if negative, the
 *   number of bytes of silence to play.
 */
int64_t GetSyncBytes(double prev_time, size_t bytes_written,
                     std::shared_ptr<DecodedFrame> next) {
  const int64_t bytes_per_sample = BytesPerSample(next);
  const double buffer_end = prev_time + BytesToSeconds(next, bytes_written);
  // If the difference is small, just ignore for now.
  if (std::abs(buffer_end - next->pts) < kSyncLimit)
    return 0;

  // Round to whole samples before converting to bytes.
  const int64_t sample_delta = static_cast<int64_t>(
      (buffer_end - next->pts) * next->stream_info->sample_rate);
  return sample_delta * next->stream_info->channel_count * bytes_per_sample;
}

}  // namespace

AudioRendererCommon::AudioRendererCommon()
    : mutex_("AudioRendererCommon"),
      on_play_("AudioRendererCommon"),
      clock_(&util::Clock::Instance),
      player_(nullptr),
      input_(nullptr),
      volume_(1),
      muted_(false),
      needs_resync_(true),
      shutdown_(false),
      thread_("AudioRenderer",
              std::bind(&AudioRendererCommon::ThreadMain, this)) {}

AudioRendererCommon::~AudioRendererCommon() {
  CHECK(shutdown_) << "Must call Stop before destroying";
  thread_.join();
  if (player_)
    player_->RemoveClient(this);
}

void AudioRendererCommon::SetPlayer(const MediaPlayer* player) {
  std::unique_lock<Mutex> lock(mutex_);
  if (player_)
    player_->RemoveClient(this);

  player_ = player;
  needs_resync_ = true;
  if (player) {
    player->AddClient(this);
    on_play_.SignalAllIfNotSet();
  }
}

void AudioRendererCommon::Attach(const DecodedStream* stream) {
  std::unique_lock<Mutex> lock(mutex_);
  input_ = stream;
  needs_resync_ = true;
  if (stream)
    on_play_.SignalAllIfNotSet();
}

void AudioRendererCommon::Detach() {
  std::unique_lock<Mutex> lock(mutex_);
  input_ = nullptr;
  SetDeviceState(/* is_playing= */ false);
}

double AudioRendererCommon::Volume() const {
  std::unique_lock<Mutex> lock(mutex_);
  return volume_;
}

void AudioRendererCommon::SetVolume(double volume) {
  std::unique_lock<Mutex> lock(mutex_);
  volume_ = volume;
  UpdateVolume(muted_ ? 0 : volume_);
}

bool AudioRendererCommon::Muted() const {
  std::unique_lock<Mutex> lock(mutex_);
  return muted_;
}

void AudioRendererCommon::SetMuted(bool muted) {
  std::unique_lock<Mutex> lock(mutex_);
  muted_ = muted;
  UpdateVolume(muted ? 0 : volume_);
}

void AudioRendererCommon::Stop() {
  {
    std::unique_lock<Mutex> lock(mutex_);
    shutdown_ = true;
  }
  on_play_.SignalAllIfNotSet();
}

bool AudioRendererCommon::FillSilence(size_t bytes) {
  while (bytes > 0) {
    const size_t to_write = std::min(bytes, sizeof(kSilenceBuffer));
    if (!AppendBuffer(kSilenceBuffer, to_write))
      return false;
    bytes_written_ += to_write;
    bytes -= to_write;
  }
  return true;
}

bool AudioRendererCommon::IsFrameSimilar(
    std::shared_ptr<DecodedFrame> frame1,
    std::shared_ptr<DecodedFrame> frame2) const {
  return frame1 && frame2 && frame1->stream_info == frame2->stream_info &&
         frame1->format == frame2->format;
}

bool AudioRendererCommon::WriteFrame(std::shared_ptr<DecodedFrame> frame,
                                     size_t sync_bytes) {
  if (IsPlanarFormat(frame->format)) {
    // We need to pack the samples into a single array.
    // Before:
    //   data[0] -> | 1A | 1B | 1C |
    //   data[1] -> | 2A | 2B | 2C |
    // After:
    //   data    -> | 1A | 2A | 1B | 2B | 1C | 2C |
    const size_t channel_count = frame->stream_info->channel_count;
    const size_t sample_size = BytesPerSample(frame);
    const size_t sample_count = frame->linesize[0] / sample_size;
    const size_t per_channel_sync = sync_bytes / channel_count;
    const size_t skipped_samples = per_channel_sync / sample_size;
    if (sample_count > skipped_samples) {
      std::vector<uint8_t> temp(frame->linesize[0] * channel_count -
                                sync_bytes);
      uint8_t* output = temp.data();
      for (size_t sample = skipped_samples; sample < sample_count; sample++) {
        for (size_t channel = 0; channel < channel_count; channel++) {
          std::memcpy(output, frame->data[channel] + sample * sample_size,
                      sample_size);
          output += sample_size;
        }
      }
      if (!AppendBuffer(temp.data(), temp.size()))
        return false;
      bytes_written_ += temp.size();
    }
  } else {
    if (frame->linesize[0] > sync_bytes) {
      if (!AppendBuffer(frame->data[0] + sync_bytes,
                        frame->linesize[0] - sync_bytes)) {
        return false;
      }
      bytes_written_ += frame->linesize[0] - sync_bytes;
    }
  }
  return true;
}

void AudioRendererCommon::SetClock(const util::Clock* clock) {
  clock_ = clock;
}

void AudioRendererCommon::ThreadMain() {
  std::unique_lock<Mutex> lock(mutex_);
  while (!shutdown_) {
    if (!player_ || !input_) {
      on_play_.ResetAndWaitWhileUnlocked(lock);
      continue;
    }

    // TODO(#15): Support playback rate.  For the moment, mute audio.
    const bool is_playing =
        player_->PlaybackRate() == 1 &&
        player_->PlaybackState() == VideoPlaybackState::Playing;
    SetDeviceState(is_playing);
    if (!is_playing) {
      on_play_.ResetAndWaitWhileUnlocked(lock);
      continue;
    }

    double time = player_->CurrentTime();
    const size_t buffered_bytes = GetBytesBuffered();
    std::shared_ptr<DecodedFrame> next;
    if (needs_resync_ || !cur_frame_) {
      ClearBuffer();
      next = input_->GetFrame(time, FrameLocation::Near);
    } else {
      const double buffered_extra =
          BytesToSeconds(cur_frame_, buffered_bytes) - kBufferTarget;
      if (buffered_extra > 0) {
        util::Unlocker<Mutex> unlock(&lock);
        clock_->SleepSeconds(buffered_extra);
        continue;
      }

      next = input_->GetFrame(cur_frame_->pts, FrameLocation::After);
    }

    if (!next) {
      util::Unlocker<Mutex> unlock(&lock);
      clock_->SleepSeconds(0.1);
      continue;
    }

    if (!IsFrameSimilar(cur_frame_, next)) {
      // If we've changed to another stream, reset the audio device to the new
      // stream.
      if (cur_frame_) {
        // Try to play out the existing buffer since resetting will clear the
        // buffer.
        const double buffered = BytesToSeconds(cur_frame_, buffered_bytes);
        const double delay = std::max(buffered - 0.1, 0.0);
        util::Unlocker<Mutex> unlock(&lock);
        clock_->SleepSeconds(delay);
        if (shutdown_)
          return;
        // Mute audio when player is not playing after sleep
        const bool is_playing =
                player_->PlaybackRate() == 1 &&
                player_->PlaybackState() == VideoPlaybackState::Playing;
        SetDeviceState(is_playing);
        if (!is_playing) {
            std::unique_lock<Mutex> lock(mutex_);
            on_play_.ResetAndWaitWhileUnlocked(lock);
            continue;
        }
        time = player_->CurrentTime();
      }

      if (!InitDevice(next, muted_ ? 0 : volume_))
        return;
      SetDeviceState(/* is_playing= */ true);
      needs_resync_ = true;
    }

    int64_t sync_bytes;
    if (needs_resync_ || !cur_frame_) {
      sync_bytes = GetSyncBytes(time, 0, next);
      sync_time_ = time;
      bytes_written_ = 0;
    } else {
      sync_bytes = GetSyncBytes(sync_time_, bytes_written_, next);
    }
    if (sync_bytes < 0) {
      if (!FillSilence(-sync_bytes))
        return;
      sync_bytes = 0;
    }
    if (!WriteFrame(next, sync_bytes))
      return;
    cur_frame_ = next;
    needs_resync_ = false;
  }
}

void AudioRendererCommon::OnPlaybackStateChanged(VideoPlaybackState old_state,
                                                 VideoPlaybackState new_state) {
  std::unique_lock<Mutex> lock(mutex_);
  needs_resync_ = true;
  on_play_.SignalAllIfNotSet();
}

void AudioRendererCommon::OnPlaybackRateChanged(double old_rate,
                                                double new_rate) {
  std::unique_lock<Mutex> lock(mutex_);
  needs_resync_ = true;
  on_play_.SignalAllIfNotSet();
}

void AudioRendererCommon::OnSeeking() {
  std::unique_lock<Mutex> lock(mutex_);
  needs_resync_ = true;
  on_play_.SignalAllIfNotSet();
}

}  // namespace media
}  // namespace shaka
