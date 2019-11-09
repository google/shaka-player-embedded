// Copyright 2016 Google LLC
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

#include "src/js/mse/video_element.h"

#include <cmath>

#include "src/core/js_manager_impl.h"
#include "src/js/dom/document.h"
#include "src/js/mse/media_source.h"
#include "src/js/mse/time_ranges.h"
#include "src/util/macros.h"

namespace shaka {
namespace js {
namespace mse {

HTMLVideoElement::HTMLVideoElement(RefPtr<dom::Document> document)
    : dom::Element(document, "video", nullopt, nullopt),
      ready_state(media::HAVE_NOTHING),
      autoplay(false),
      loop(false),
      pipeline_status_(media::PipelineStatus::Initializing),
      volume_(1),
      will_play_(false),
      is_muted_(false),
      clock_(&util::Clock::Instance),
      shutdown_(false),
      thread_("VideoElement", std::bind(&HTMLVideoElement::ThreadMain, this)) {
  AddListenerField(EventType::Encrypted, &on_encrypted);
  AddListenerField(EventType::WaitingForKey, &on_waiting_for_key);
}

// \cond Doxygen_Skip
HTMLVideoElement::~HTMLVideoElement() {
  shutdown_.store(true, std::memory_order_release);
  thread_.join();
}
// \endcond Doxygen_Skip

void HTMLVideoElement::Trace(memory::HeapTracer* tracer) const {
  dom::Element::Trace(tracer);
  tracer->Trace(&text_tracks);
  tracer->Trace(&media_source_);
}

void HTMLVideoElement::ThreadMain() {
  double lastTime = CurrentTime();
  while (!shutdown_.load(std::memory_order_acquire)) {
    const double time = CurrentTime();

    if (pipeline_status_ == media::PipelineStatus::Playing) {
      CheckForCueChange(time, lastTime);
      lastTime = time;
    }

    clock_->SleepSeconds(0.25);
  }
}

void HTMLVideoElement::OnReadyStateChanged(
    media::MediaReadyState new_ready_state) {
  DCHECK(media_source_ ? new_ready_state != media::HAVE_NOTHING
                       : new_ready_state == media::HAVE_NOTHING);
  if (ready_state == new_ready_state)
    return;

  if (ready_state < media::HAVE_METADATA &&
      new_ready_state >= media::HAVE_METADATA) {
    ScheduleEvent<events::Event>(EventType::LoadedMetaData);
  }
  if (ready_state < media::HAVE_CURRENT_DATA &&
      new_ready_state >= media::HAVE_CURRENT_DATA) {
    ScheduleEvent<events::Event>(EventType::LoadedData);
  }
  if (ready_state < media::HAVE_ENOUGH_DATA &&
      new_ready_state >= media::HAVE_ENOUGH_DATA) {
    ScheduleEvent<events::Event>(EventType::CanPlay);
  }
  if (ready_state >= media::HAVE_FUTURE_DATA &&
      new_ready_state < media::HAVE_FUTURE_DATA &&
      new_ready_state != media::HAVE_NOTHING) {
    ScheduleEvent<events::Event>(EventType::Waiting);
  }

  ScheduleEvent<events::Event>(EventType::ReadyStateChange);
  ready_state = new_ready_state;
}

void HTMLVideoElement::OnPipelineStatusChanged(media::PipelineStatus status) {
  if (status == pipeline_status_) {
    // If we get another seeking status change, we still fire the 'seeking'
    // event since the current time changed.
    if (status == media::PipelineStatus::SeekingPlay ||
        status == media::PipelineStatus::SeekingPause) {
      ScheduleEvent<events::Event>(EventType::Seeking);
    }
    return;
  }

  switch (status) {
    case media::PipelineStatus::Initializing:
      ScheduleEvent<events::Event>(EventType::Emptied);
      break;
    case media::PipelineStatus::Playing:
      if (pipeline_status_ == media::PipelineStatus::Paused) {
        ScheduleEvent<events::Event>(EventType::Play);
      } else if (pipeline_status_ == media::PipelineStatus::SeekingPlay) {
        ScheduleEvent<events::Event>(EventType::Seeked);
      } else {
        DCHECK(pipeline_status_ == media::PipelineStatus::Stalled ||
               pipeline_status_ == media::PipelineStatus::Initializing);
      }
      ScheduleEvent<events::Event>(EventType::Playing);
      break;
    case media::PipelineStatus::Paused:
      if (pipeline_status_ == media::PipelineStatus::Playing ||
          pipeline_status_ == media::PipelineStatus::Stalled) {
        ScheduleEvent<events::Event>(EventType::Pause);
      } else if (pipeline_status_ == media::PipelineStatus::SeekingPause) {
        ScheduleEvent<events::Event>(EventType::Seeked);
      } else {
        DCHECK_EQ(pipeline_status_, media::PipelineStatus::Initializing);
      }
      break;
    case media::PipelineStatus::Stalled:
      break;
    case media::PipelineStatus::SeekingPlay:
    case media::PipelineStatus::SeekingPause:
      ScheduleEvent<events::Event>(EventType::Seeking);
      break;
    case media::PipelineStatus::Ended:
      if (pipeline_status_ == media::PipelineStatus::Playing) {
        ScheduleEvent<events::Event>(EventType::Pause);
      } else if (pipeline_status_ == media::PipelineStatus::SeekingPlay ||
                 pipeline_status_ == media::PipelineStatus::SeekingPause) {
        ScheduleEvent<events::Event>(EventType::Seeked);
      }
      ScheduleEvent<events::Event>(EventType::Ended);
      break;
    case media::PipelineStatus::Errored:
      ScheduleEvent<events::Event>(EventType::Error);
      if (!error)
        error = new MediaError(MEDIA_ERR_DECODE, "Unknown media error");
      break;
  }

  pipeline_status_ = status;
}

void HTMLVideoElement::CheckForCueChange(double newTime, double oldTime) {
  for (const auto& text_track : text_tracks) {
    text_track->CheckForCueChange(newTime, oldTime);
  }
}

void HTMLVideoElement::OnMediaError(media::SourceType /* source */,
                                    media::Status status) {
  ScheduleEvent<events::Event>(EventType::Error);
  if (!error)
    error = new MediaError(MEDIA_ERR_DECODE, GetErrorString(status));
}

RefPtr<MediaSource> HTMLVideoElement::GetMediaSource() const {
  return media_source_;
}

Promise HTMLVideoElement::SetMediaKeys(RefPtr<eme::MediaKeys> media_keys) {
  if (!media_keys && !media_source_)
    return Promise::Resolved();
  if (!media_source_) {
    return Promise::Rejected(JsError::DOMException(
        InvalidStateError, "Cannot set MediaKeys until after setting source"));
  }

  eme::Implementation* cdm = media_keys ? media_keys->GetCdm() : nullptr;
  media_source_->GetController()->SetCdm(cdm);
  this->media_keys = media_keys;
  return Promise::Resolved();
}

void HTMLVideoElement::Load() {
  error = nullptr;
  if (media_source_) {
    media_source_->CloseMediaSource();
    media_source_.reset();
    OnReadyStateChanged(media::HAVE_NOTHING);
    OnPipelineStatusChanged(media::PipelineStatus::Initializing);
    will_play_ = false;
  }
}

CanPlayTypeEnum HTMLVideoElement::CanPlayType(const std::string& type) {
  if (!MediaSource::IsTypeSupported(type))
    return CanPlayTypeEnum::EMPTY;
  return CanPlayTypeEnum::MAYBE;
}

media::VideoPlaybackQuality HTMLVideoElement::GetVideoPlaybackQuality() const {
  return media_source_
             ? *media_source_->GetController()->GetVideoPlaybackQuality()
             : media::VideoPlaybackQuality();
}

RefPtr<TimeRanges> HTMLVideoElement::Buffered() const {
  if (!media_source_)
    return new TimeRanges(media::BufferedRanges());

  return new TimeRanges(media_source_->GetController()->GetBufferedRanges(
      media::SourceType::Unknown));
}

RefPtr<TimeRanges> HTMLVideoElement::Seekable() const {
  media::BufferedRanges ranges;
  if (media_source_ && !std::isnan(media_source_->GetDuration()))
    ranges.emplace_back(0, media_source_->GetDuration());
  return new TimeRanges(ranges);
}

std::string HTMLVideoElement::Source() const {
  return media_source_ ? media_source_->url : "";
}

ExceptionOr<void> HTMLVideoElement::SetSource(const std::string& src) {
  // Unload any previous MediaSource objects.
  Load();

  DCHECK(!media_source_);
  if (src.empty())
    return {};

  media_source_ = MediaSource::FindMediaSource(src);
  if (media_source_) {
    media_source_->OpenMediaSource(this);
    media_source_->GetController()->SetVolume(is_muted_ ? 0 : volume_);
    if (autoplay || will_play_)
      media_source_->GetController()->GetPipelineManager()->Play();
  } else {
    // We don't support direct URL playback, only MediaSource playback.
    return JsError::DOMException(NotSupportedError,
                                 "Unknown MediaSource URL given.");
  }
  return {};
}

double HTMLVideoElement::CurrentTime() const {
  if (!media_source_)
    return 0;

  return media_source_->GetController()->GetPipelineManager()->GetCurrentTime();
}

void HTMLVideoElement::SetCurrentTime(double time) {
  if (media_source_) {
    media_source_->GetController()->GetPipelineManager()->SetCurrentTime(time);
  }
}

double HTMLVideoElement::Duration() const {
  if (!media_source_)
    return 0;
  return media_source_->GetController()->GetPipelineManager()->GetDuration();
}

double HTMLVideoElement::PlaybackRate() const {
  if (!media_source_)
    return 1;

  return media_source_->GetController()
      ->GetPipelineManager()
      ->GetPlaybackRate();
}

void HTMLVideoElement::SetPlaybackRate(double rate) {
  if (media_source_) {
    return media_source_->GetController()
        ->GetPipelineManager()
        ->SetPlaybackRate(rate);
  }
}

bool HTMLVideoElement::Muted() const {
  return is_muted_;
}

void HTMLVideoElement::SetMuted(bool muted) {
  is_muted_ = muted;
  if (media_source_)
    media_source_->GetController()->SetVolume(muted ? 0 : volume_);
}

double HTMLVideoElement::Volume() const {
  return volume_;
}

void HTMLVideoElement::SetVolume(double volume) {
  volume_ = volume;
  if (media_source_)
    media_source_->GetController()->SetVolume(is_muted_ ? 0 : volume_);
}

bool HTMLVideoElement::Paused() const {
  return pipeline_status_ == media::PipelineStatus::Paused ||
         pipeline_status_ == media::PipelineStatus::SeekingPause ||
         pipeline_status_ == media::PipelineStatus::Ended;
}

bool HTMLVideoElement::Seeking() const {
  return pipeline_status_ == media::PipelineStatus::SeekingPlay ||
         pipeline_status_ == media::PipelineStatus::SeekingPause;
}

bool HTMLVideoElement::Ended() const {
  return pipeline_status_ == media::PipelineStatus::Ended;
}

void HTMLVideoElement::Play() {
  if (media_source_)
    return media_source_->GetController()->GetPipelineManager()->Play();

  will_play_ = true;
}

void HTMLVideoElement::Pause() {
  if (media_source_)
    return media_source_->GetController()->GetPipelineManager()->Pause();

  will_play_ = false;
}

RefPtr<TextTrack> HTMLVideoElement::AddTextTrack(
    TextTrackKind kind, optional<std::string> label,
    optional<std::string> language) {
  RefPtr<TextTrack> ret =
      new TextTrack(kind, label.value_or(""), language.value_or(""));
  text_tracks.emplace_back(ret);
  return ret;
}


HTMLVideoElementFactory::HTMLVideoElementFactory() {
  AddConstant("HAVE_NOTHING", media::HAVE_NOTHING);
  AddConstant("HAVE_METADATA", media::HAVE_METADATA);
  AddConstant("HAVE_CURRENT_DATA", media::HAVE_CURRENT_DATA);
  AddConstant("HAVE_FUTURE_DATA", media::HAVE_FUTURE_DATA);
  AddConstant("HAVE_ENOUGH_DATA", media::HAVE_ENOUGH_DATA);

  AddListenerField(EventType::Encrypted, &HTMLVideoElement::on_encrypted);
  AddListenerField(EventType::WaitingForKey,
                   &HTMLVideoElement::on_waiting_for_key);

  AddReadWriteProperty("autoplay", &HTMLVideoElement::autoplay);
  AddReadWriteProperty("loop", &HTMLVideoElement::loop);
  AddReadOnlyProperty("mediaKeys", &HTMLVideoElement::media_keys);
  AddReadOnlyProperty("readyState", &HTMLVideoElement::ready_state);
  AddReadOnlyProperty("textTracks", &HTMLVideoElement::text_tracks);
  AddReadOnlyProperty("error", &HTMLVideoElement::error);

  AddGenericProperty("paused", &HTMLVideoElement::Paused);
  AddGenericProperty("seeking", &HTMLVideoElement::Seeking);
  AddGenericProperty("ended", &HTMLVideoElement::Ended);
  AddGenericProperty("buffered", &HTMLVideoElement::Buffered);
  AddGenericProperty("seekable", &HTMLVideoElement::Seekable);
  AddGenericProperty("src", &HTMLVideoElement::Source,
                     &HTMLVideoElement::SetSource);
  AddGenericProperty("currentSrc", &HTMLVideoElement::Source);
  AddGenericProperty("currentTime", &HTMLVideoElement::CurrentTime,
                     &HTMLVideoElement::SetCurrentTime);
  AddGenericProperty("duration", &HTMLVideoElement::Duration);
  AddGenericProperty("playbackRate", &HTMLVideoElement::PlaybackRate,
                     &HTMLVideoElement::SetPlaybackRate);

  AddMemberFunction("load", &HTMLVideoElement::Load);
  AddMemberFunction("play", &HTMLVideoElement::Play);
  AddMemberFunction("pause", &HTMLVideoElement::Pause);
  AddMemberFunction("setMediaKeys", &HTMLVideoElement::SetMediaKeys);
  AddMemberFunction("addTextTrack", &HTMLVideoElement::AddTextTrack);
  AddMemberFunction("getVideoPlaybackQuality",
                    &HTMLVideoElement::GetVideoPlaybackQuality);
  AddMemberFunction("canPlayType", &HTMLVideoElement::CanPlayType);

  NotImplemented("crossOrigin");
  NotImplemented("networkState");
  NotImplemented("preload");
  NotImplemented("getStartDate");
  NotImplemented("defaultPlaybackRate");
  NotImplemented("playable");
  NotImplemented("mediaGroup");
  NotImplemented("controller");
  NotImplemented("controls");
  NotImplemented("volume");
  NotImplemented("muted");
  NotImplemented("defaultMuted");
  NotImplemented("audioTracks");
  NotImplemented("videoTracks");
}

}  // namespace mse
}  // namespace js
}  // namespace shaka
