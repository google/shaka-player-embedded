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

#ifndef SHAKA_EMBEDDED_MEDIA_PIPELINE_MANAGER_H_
#define SHAKA_EMBEDDED_MEDIA_PIPELINE_MANAGER_H_

#include <atomic>
#include <thread>

#include "shaka/media/media_player.h"
#include "shaka/src/mapping/promise.h"
#include "src/debug/mutex.h"
#include "src/util/clock.h"

namespace shaka {
namespace media {

/**
 * Tracks the current playhead time and tracks the pipeline status.  This
 * handles playback rate, pause/play, and tracking current time.  The caller
 * is in charge of tracking the amount of content that is buffered and whether
 * playback is actually possible.
 *
 * This type is thread safe; however if calls are made to this from multiple
 * threads at once, it is unspecified what order the changes will happen
 * including the order of the calls to |on_status_changed|.  The callback is
 * invoked without the lock held, which allows for calls back into this object;
 * but this means that another thread could make a state change before the
 * callback is completed.  This also means that the callback can be invoked
 * multiple times concurrently.
 */
class PipelineManager {
 public:
  PipelineManager(std::function<void(VideoPlaybackState)> on_status_changed,
                  std::function<void()> on_seek, const util::Clock* clock);
  virtual ~PipelineManager();

  /** Resets the state to the initial state; this doesn't raise events. */
  virtual void Reset();

  /** Tells the manager that we have gotten all the initialization data. */
  virtual void DoneInitializing();

  /** @return The current pipeline status. */
  virtual VideoPlaybackState GetPlaybackState() const;

  /** @return The current video duration. */
  virtual double GetDuration() const;

  /** Sets the video duration. */
  virtual void SetDuration(double duration);

  /** @return The current video time, in seconds. */
  virtual double GetCurrentTime() const;

  /** Seeks to the given video time. */
  virtual void SetCurrentTime(double time);

  /** @return The current playback rate. */
  virtual double GetPlaybackRate() const;

  /** Sets the current playback rate. */
  virtual void SetPlaybackRate(double rate);

  /** Starts playing the video. */
  virtual Promise Play();

  /** Pauses the video. */
  virtual void Pause();

  /** Called when the video stalls due to lack of content. */
  virtual void Buffering();

  /** Called when the video has enough content to play forward. */
  virtual void CanPlay();

  /**
   * Called when the video should end.  Note that the current time is always
   * clamped to duration, so this only raises the event.
   */
  virtual void OnEnded();

  /** Called when an error occurs and the pipeline should stop forever. */
  virtual void OnError();

 private:
  /** @return The video time for the given wall-clock time. */
  double GetTimeFor(uint64_t wall_time) const;

  /**
   * Introduces a time sync point.  This avoids rounding errors by reducing the
   * number of times we change the stored current time.  What we do is store
   * the video time at a sync point with the wall-clock time.  Then, when we
   * later need the current video time, we add the change in wall-clock time
   * to the previous video time.
   *
   * This method stores the current video time and the wall-clock time.
   */
  void SyncPoint();

  mutable SharedMutex mutex_;
  const std::function<void(VideoPlaybackState)> on_status_changed_;
  const std::function<void()> on_seek_;
  const util::Clock* const clock_;
  VideoPlaybackState status_;

  /** The media time at the last sync point. */
  double prev_media_time_;
  /** The wall-clock time at the last sync point. */
  uint64_t prev_wall_time_;
  double playback_rate_;
  double duration_;
  bool will_play_;
};

}  // namespace media
}  // namespace shaka

#endif  // SHAKA_EMBEDDED_MEDIA_PIPELINE_MANAGER_H_
