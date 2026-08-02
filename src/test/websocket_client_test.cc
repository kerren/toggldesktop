// Copyright 2014 Toggl Desktop developers.
//
// Routing tests for the WebSocket push channel.
//
// The old v8-era endpoint (desktop.track.toggl.com/stream) is gone and its
// upgrade is refused, which left the app with no push channel and a 15-30
// minute polling lag. The live endpoint speaks a different protocol:
// {"action":..,"parameters":{..}} up, {"event_type":..} down, and the
// downward frames announce a change without carrying the changed record.
//
// Nothing here opens a socket. WebSocketClient::ClassifyFrame is the pure
// decision the poll loop makes about an inbound frame, so these pin the part
// that would otherwise only be observable against a live server.

#include "gtest/gtest.h"

#include <string>

#include "urls.h"
#include "websocket_client.h"

namespace toggl {

namespace {

WebSocketClient::FrameAction classify(const std::string &json) {
    return WebSocketClient::ClassifyFrame(json);
}

}  // namespace

TEST(WebSocketURL, PointsAtTheLiveEndpointWithItsPath) {
    const bool was_staging = urls::IsUsingStagingAsBackend();

    urls::SetUseStagingAsBackend(false);
    ASSERT_EQ("wss://track.toggl.com/websockets", urls::WebSocket());

    urls::SetUseStagingAsBackend(true);
    ASSERT_EQ("wss://track.toggl.space/websockets", urls::WebSocket());

    urls::SetUseStagingAsBackend(was_staging);
}

TEST(WebSocketFrame, ChangeEventTriggersASync) {
    // The record is not in the frame, so the only useful reaction is to pull.
    ASSERT_EQ(WebSocketClient::kFrameSync,
              classify("{\"event_type\":\"service_notification\","
                       "\"data\":[],\"page\":1,\"rows\":50}"));
    ASSERT_EQ(WebSocketClient::kFrameSync,
              classify("{\"event_type\":\"time_entry_updated\"}"));
}

TEST(WebSocketFrame, UnrecognisedShapeTriggersASync) {
    // Missing an event is a 15-30 minute regression; an extra debounced pull
    // is not. Unknown frames therefore sync rather than being dropped.
    ASSERT_EQ(WebSocketClient::kFrameSync, classify("{\"something\":\"new\"}"));
}

TEST(WebSocketFrame, KeepalivesDoNotSync) {
    ASSERT_EQ(WebSocketClient::kFramePing,
              classify("{\"event_type\":\"ping\"}"));
    ASSERT_EQ(WebSocketClient::kFrameIgnore,
              classify("{\"event_type\":\"pong\"}"));
    // The pre-v9 endpoint used a differently shaped keepalive.
    ASSERT_EQ(WebSocketClient::kFrameLegacyPing,
              classify("{\"type\":\"ping\"}"));
}

TEST(WebSocketFrame, ServerErrorIsReportedNotSyncedOn) {
    // Syncing in response to an error frame would spin against a server that
    // is already refusing us.
    ASSERT_EQ(WebSocketClient::kFrameServerError,
              classify("{\"event_type\":\"error\",\"message\":\"nope\"}"));
    ASSERT_EQ(WebSocketClient::kFrameServerError,
              classify("{\"event_type\":\"unauthorized\"}"));
}

TEST(WebSocketFrame, LegacyModelUpdateIsAppliedNotSyncedOn) {
    // A frame carrying the record still gets applied directly, without the
    // round trip a sync would cost.
    ASSERT_EQ(WebSocketClient::kFrameLegacyModelUpdate,
              classify("{\"model\":\"time_entry\",\"action\":\"update\","
                       "\"data\":{\"id\":1}}"));
}

TEST(WebSocketFrame, GarbageIsIgnored) {
    ASSERT_EQ(WebSocketClient::kFrameIgnore, classify(""));
    ASSERT_EQ(WebSocketClient::kFrameIgnore, classify("not json"));
    ASSERT_EQ(WebSocketClient::kFrameIgnore, classify("[1,2,3]"));
}

}  // namespace toggl
