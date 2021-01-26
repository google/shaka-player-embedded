// Copyright 2017 Google LLC
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

#include "src/media/pipeline_manager.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "src/util/utils.h"
#include "src/js/js_error.h"

namespace shaka {
namespace media {

PipelineManager::PipelineManager(
    std::function<void(VideoPlaybackState)> on_status_changed,
    std::function<void()> on_seek, const util::Clock* clock)
    : mutex_("PipelineManager"),
      on_status_changed_(std::move(on_status_changed)),
      on_seek_(std::move(on_seek)),
      clock_(clock) {
  Reset();
}

PipelineManager::~PipelineManager() {}

void PipelineManager::Reset() {
  std::unique_lock<SharedMutex> lock(mutex_);
  status_ = VideoPlaybackState::Initializing;
  prev_media_time_ = 0;
  prev_wall_time_ = clock_->GetMonotonicTime();
  playback_rate_ = 1;
  duration_ = NAN;
  will_play_ = false;
}

void PipelineManager::DoneInitializing() {
  VideoPlaybackState new_status;
  {
    std::unique_lock<SharedMutex> lock(mutex_);
    if (status_ == VideoPlaybackState::Errored)
      return;
    DCHECK(status_ == VideoPlaybackState::Initializing);
    if (will_play_) {
      new_status = status_ = VideoPlaybackState::Buffering;
    } else {
      new_status = status_ = VideoPlaybackState::Paused;
    }
  }
  on_status_changed_(new_status);
}

VideoPlaybackState PipelineManager::GetPlaybackState() const {
  util::shared_lock<SharedMutex> lock(mutex_);
  return status_;
}

double PipelineManager::GetDuration() const {
  util::shared_lock<SharedMutex> lock(mutex_);
  return duration_;
}

void PipelineManager::SetDuration(double duration) {
  VideoPlaybackState new_status = VideoPlaybackState::Initializing;
  {
    std::unique_lock<SharedMutex> lock(mutex_);
    duration_ = duration;

    // Seek to duration if current time is past the new duration.
    const uint64_t wall_time = clock_->GetMonotonicTime();
    if (!std::isnan(duration) && GetTimeFor(wall_time) > duration) {
      {
        util::Unlocker<SharedMutex> unlock(&lock);
        on_seek_();
      }

      prev_media_time_ = duration;
      prev_wall_time_ = wall_time;
      if (status_ == VideoPlaybackState::Playing ||
          status_ == VideoPlaybackState::Buffering ||
          status_ == VideoPlaybackState::WaitingForKey) {
        will_play_ = true;
        new_status = status_ = VideoPlaybackState::Seeking;
      } else if (status_ == VideoPlaybackState::Paused ||
                 status_ == VideoPlaybackState::Ended) {
        will_play_ = false;
        new_status = status_ = VideoPlaybackState::Seeking;
      }
    }
  }
  if (new_status != VideoPlaybackState::Initializing)
    on_status_changed_(new_status);
}

double PipelineManager::GetCurrentTime() const {
  util::shared_lock<SharedMutex> lock(mutex_);
  return GetTimeFor(clock_->GetMonotonicTime());
}

void PipelineManager::SetCurrentTime(double time) {
  VideoPlaybackState new_status = VideoPlaybackState::Initializing;
  {
    std::unique_lock<SharedMutex> lock(mutex_);
    if (status_ != VideoPlaybackState::Initializing) {
      {
        util::Unlocker<SharedMutex> unlock(&lock);
        on_seek_();
      }

      prev_media_time_ =
          std::isnan(duration_) ? time : std::min(duration_, time);
      prev_wall_time_ = clock_->GetMonotonicTime();
      switch (status_) {
        case VideoPlaybackState::Playing:
        case VideoPlaybackState::Buffering:
        case VideoPlaybackState::WaitingForKey:
          will_play_ = true;
          new_status = status_ = VideoPlaybackState::Seeking;
          break;
        case VideoPlaybackState::Paused:
        case VideoPlaybackState::Ended:
          will_play_ = false;
          new_status = status_ = VideoPlaybackState::Seeking;
          break;
        default:  // Ignore remaining enum values.
          break;
      }
    }
  }
  if (new_status != VideoPlaybackState::Initializing)
    on_status_changed_(new_status);
}

double PipelineManager::GetPlaybackRate() const {
  util::shared_lock<SharedMutex> lock(mutex_);
  return playback_rate_;
}

void PipelineManager::SetPlaybackRate(double rate) {
  std::unique_lock<SharedMutex> lock(mutex_);
  SyncPoint();
  playback_rate_ = rate;
}

void PipelineManager::Play() {
  VideoPlaybackState new_status = VideoPlaybackState::Initializing;
  try
  {
    std::unique_lock<SharedMutex> lock(mutex_);
    SyncPoint();
    will_play_ = true;
    if (status_ == VideoPlaybackState::Paused) {
      // Assume we are stalled; we will transition to Playing quickly if not.
      new_status = status_ = VideoPlaybackState::Buffering;
    } else if (status_ == VideoPlaybackState::Ended) {
      {
        util::Unlocker<SharedMutex> unlock(&lock);
        on_seek_();
      }

      prev_media_time_ = 0;
      new_status = status_ = VideoPlaybackState::Seeking;
    }
  }
  catch (const js::JsError& e)
  {
    return Promise::Rejected(e);
  }
  if (new_status != VideoPlaybackState::Initializing)
    on_status_changed_(new_status);
}

void PipelineManager::Pause() {
  VideoPlaybackState new_status = VideoPlaybackState::Initializing;
  {
    std::unique_lock<SharedMutex> lock(mutex_);
    SyncPoint();
    will_play_ = false;
    if (status_ == VideoPlaybackState::Playing ||
        status_ == VideoPlaybackState::Buffering ||
        status_ == VideoPlaybackState::WaitingForKey) {
      new_status = status_ = VideoPlaybackState::Paused;
    }
  }
  if (new_status != VideoPlaybackState::Initializing)
    on_status_changed_(new_status);
}

void PipelineManager::Buffering() {
  bool status_changed = false;
  {
    std::unique_lock<SharedMutex> lock(mutex_);
    if (status_ == VideoPlaybackState::Playing) {
      SyncPoint();
      status_ = VideoPlaybackState::Buffering;
      status_changed = true;
    }
  }
  if (status_changed)
    on_status_changed_(VideoPlaybackState::Buffering);
}

void PipelineManager::CanPlay() {
  VideoPlaybackState new_status = VideoPlaybackState::Initializing;
  {
    std::unique_lock<SharedMutex> lock(mutex_);
    SyncPoint();
    if (status_ == VideoPlaybackState::Buffering ||
        status_ == VideoPlaybackState::WaitingForKey ||
        status_ == VideoPlaybackState::Seeking) {
      if (will_play_)
        new_status = status_ = VideoPlaybackState::Playing;
      else
        new_status = status_ = VideoPlaybackState::Paused;
    }
  }
  if (new_status != VideoPlaybackState::Initializing)
    on_status_changed_(new_status);
}

void PipelineManager::OnEnded() {
  VideoPlaybackState new_status = VideoPlaybackState::Initializing;
  {
    std::unique_lock<SharedMutex> lock(mutex_);
    if (status_ != VideoPlaybackState::Ended &&
        status_ != VideoPlaybackState::Errored) {
      const uint64_t wall_time = clock_->GetMonotonicTime();
      DCHECK(!std::isnan(duration_));
      prev_wall_time_ = wall_time;
      prev_media_time_ = duration_;
      new_status = status_ = VideoPlaybackState::Ended;
    }
  }
  if (new_status != VideoPlaybackState::Initializing)
    on_status_changed_(new_status);
}

void PipelineManager::OnError() {
  bool fire_event = false;
  {
    std::unique_lock<SharedMutex> lock(mutex_);
    if (status_ != VideoPlaybackState::Errored) {
      SyncPoint();
      status_ = VideoPlaybackState::Errored;
      fire_event = true;
    }
  }
  if (fire_event)
    on_status_changed_(VideoPlaybackState::Errored);
}

double PipelineManager::GetTimeFor(uint64_t wall_time) const {
  if (status_ != VideoPlaybackState::Playing)
    return prev_media_time_;

  const uint64_t wall_diff = wall_time - prev_wall_time_;
  const double time = prev_media_time_ + (wall_diff * playback_rate_ / 1000.0);
  return std::isnan(duration_) ? time : std::min(duration_, time);
}

void PipelineManager::SyncPoint() {
  const uint64_t wall_time = clock_->GetMonotonicTime();
  prev_media_time_ = GetTimeFor(wall_time);
  prev_wall_time_ = wall_time;
}

}  // namespace media
}  // namespace shaka
