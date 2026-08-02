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

Json::Value TimelineEvent::SaveToJSON(int) const {
    // POST /api/v9/timeline takes an array of models.TimelineEvent:
    // desktop_id, start_time, end_time, filename, title, idle, id. Only the
    // endpoint moved from v8 to v9 -- the payload shape did not change, so
    // there is nothing left to branch on here. (desktop_id is attached by
    // convertTimelineToJSON below, not here.) `created_with` and `guid` are
    // not part of the v9 schema and must not be sent.
    Json::Value n;
    n["filename"] = Filename();
    n["title"] = Title();
    n["start_time"] = Json::Int64(Start());
    n["end_time"] = Json::Int64(EndTime());
    n["idle"] = Idle();
    return n;
}

void TimelineEvent::updateDuration() {
    Poco::Int64 value = EndTime() - StartTime();
    DurationInSeconds.Set(value < 0 ? 0 : value);
}

}   // namespace toggl
