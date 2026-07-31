// Copyright 2015 Toggl Desktop developers.

#include "model/timeline_event.h"

#include <sstream>
#include <cstring>

namespace toggl {

std::string TimelineEvent::String() const {
    std::stringstream ss;
    ss << "TimelineEvent"
       << " guid=" << GUID()
       << " local_id=" << LocalID()
       << " start_time=" << Start()
       << " end_time=" << EndTime()
       << " filename=" << Filename()
       << " title=" << Title()
       << " duration=" << Duration();
    return ss.str();
}

std::string TimelineEvent::ModelName() const {
    return kModelTimelineEvent;
}

std::string TimelineEvent::ModelURL() const {
    return "";
}

void TimelineEvent::SetTitle(const std::string &value) {
    if (Title.Set(value))
        SetDirty();
}

void TimelineEvent::SetStartTime(Poco::Int64 value) {
    if (StartTime.Set(value)) {
        updateDuration();
        SetDirty();
    }
}

void TimelineEvent::SetEndTime(Poco::Int64 value) {
    if (EndTime.Set(value))  {
        updateDuration();
        SetDirty();
    }
}

void TimelineEvent::SetIdle(bool value) {
    if (Idle.Set(value))
        SetDirty();
}

void TimelineEvent::SetFilename(const std::string &value) {
    if (Filename.Set(value))
        SetDirty();
}

void TimelineEvent::SetChunked(bool value) {
    if (Chunked.Set(value))
        SetDirty();
}

void TimelineEvent::SetUploaded(bool value) {
    if (Uploaded.Set(value))
        SetDirty();
}

Json::Value TimelineEvent::SaveToJSON(int apiVersion) const {
    Json::Value n;
    n["guid"] = GUID();
    n["created_with"] = "timeline";

    if (apiVersion <= 8) {
        // Legacy shape, kept for reference and tests. The v8 API was shut
        // down by Toggl in 2023, so this is no longer sent to the backend.
        n["filename"] = Filename();
        n["title"] = Title();
        n["start_time"] = Json::Int64(Start());
        n["end_time"] = Json::Int64(EndTime());
        return n;
    }

    // v9 shape of POST /api/v9/timeline: the file name and the window title
    // were renamed and the epoch timestamps became ISO 8601 strings in UTC
    // (2006-01-02T15:04:05Z), like everywhere else in API v9.
    n["app_name"] = Filename();
    n["window_title"] = Title();
    if (Start()) {
        n["start"] = Formatter::Format8601(Start());
    }
    if (EndTime()) {
        n["end"] = Formatter::Format8601(EndTime());
    }
    return n;
}

void TimelineEvent::updateDuration() {
    Poco::Int64 value = EndTime() - StartTime();
    DurationInSeconds.Set(value < 0 ? 0 : value);
}

}   // namespace toggl
