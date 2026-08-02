// Copyright 2015 Toggl Desktop developers

#include "urls.h"

namespace toggl {

namespace urls {

// Whether requests are sent to staging backend

#ifndef TOGGL_PRODUCTION_BUILD
static bool use_staging_as_backend = true;
#else
static bool use_staging_as_backend = false;
#endif

// Whether requests are allowed to Toggl backend
static bool im_a_teapot_ = false;

// Whether requests are allowed at all (like in tests)
static bool requests_allowed_ = true;

void SetUseStagingAsBackend(const bool value) {
    use_staging_as_backend = value;
}

bool IsUsingStagingAsBackend() {
    return use_staging_as_backend;
}

std::string Main() {
    if (use_staging_as_backend) {
        return "https://track.toggl.space";
    }
    return "https://track.toggl.com";
}

std::string API() {
    // The desktop-specific host (desktop.track.toggl.*) was only ever needed
    // for the v8 API. Everything the app calls today lives under /api/v9 on
    // the public API host.
    return TrackAPI();
}

std::string SyncAPI() {
    if (use_staging_as_backend) {
        return "https://sync.toggl.space/";
    }
    return "https://sync.toggl.com/";
}

std::string TrackAPI() {
    if (use_staging_as_backend) {
        return "https://api.track.toggl.space";
    }
    return "https://api.track.toggl.com";
}

std::string TimelineUpload() {
    // The timeline endpoint lives on the main API host since the v8 shutdown,
    // not on the desktop-specific one.
    return TrackAPI();
}

std::string WebSocket() {
    // Full websocket URL, path included -- WebSocketClient takes the host,
    // port and request path from this URI.
    //
    // The old v8-era endpoint was desktop.track.toggl.com/stream. That host
    // died with v8 and now rejects the upgrade ("Cannot upgrade to WebSocket
    // connection"), which left the app with no push channel at all and a
    // 15-30 minute polling lag. The live endpoint is /websockets on the main
    // Track host.
    if (use_staging_as_backend) {
        return "wss://track.toggl.space/websockets";
    }
    return "wss://track.toggl.com/websockets";
}

bool ImATeapot() {
    return im_a_teapot_;
}

void SetImATeapot(const bool value) {
    im_a_teapot_ = value;
}

bool RequestsAllowed() {
    return requests_allowed_;
}

void SetRequestsAllowed(const bool value) {
    requests_allowed_ = value;
}


}  // namespace urls

}  // namespace toggl
