// Copyright 2014 Toggl Desktop developers.

#ifndef SRC_WEBSOCKET_CLIENT_H_
#define SRC_WEBSOCKET_CLIENT_H_

#include <string>
#include <vector>
#include <ctime>

#include <Poco/Activity.h>
#include <Poco/Net/HTTPClientSession.h>

#include "types.h"
#include "util/logger.h"

namespace Poco {
namespace Net {
class HTTPClientSession;
class HTTPRequest;
class HTTPResponse;
class WebSocket;
} // namespace Poco::Net
} // namespace Poco

namespace toggl {

// Called with a legacy `{"model":..,"action":..,"data":..}` push frame, which
// carries the changed record inline.
typedef void (*WebSocketMessageCallback)(
    void *callback,
    std::string json);

// Called when the server announces that something changed but does not send
// the record itself. The receiver is expected to pull over HTTP.
typedef void (*WebSocketSyncCallback)(
    void *callback);

class TOGGL_INTERNAL_EXPORT WebSocketClient {
 public:
    // What an inbound frame asks of the client. Public, and split out of the
    // handler, so the routing is testable without a server to talk to.
    enum FrameAction {
        kFrameIgnore,
        kFrameLegacyModelUpdate,  // apply the record carried in the frame
        kFrameLegacyPing,         // reply {"type":"pong"}
        kFramePing,               // reply {"action":"pong"}
        kFrameServerError,        // log it, the server is complaining
        kFrameSync                // something changed upstream, go pull
    };

    static FrameAction ClassifyFrame(const std::string &json);

    WebSocketClient() :
    activity_(this, &WebSocketClient::runActivity),
    session_(nullptr),
    req_(nullptr),
    res_(nullptr),
    ws_(nullptr),
    on_websocket_message_(nullptr),
    on_websocket_sync_(nullptr),
    ctx_(nullptr),
    last_connection_at_(0),
    last_ping_at_(0),
    api_token_("") {}
    virtual ~WebSocketClient();

    virtual void Start(
        void *ctx,
        const std::string &api_token,
        WebSocketMessageCallback on_websocket_message,
        WebSocketSyncCallback on_websocket_sync);
    virtual void Shutdown();

 protected:
    void runActivity();

 private:
    error createSession();

    void authenticate();

    error poll();

    void handleMessage(const std::string &json);

    void sendFrame(const std::string &payload);

    void sendPing();

    error receiveWebSocketMessage(std::string *message, int *flags);

    void deleteSession();

    int nextWebsocketRestartInterval();

    Logger logger() const;

    Poco::Activity<WebSocketClient> activity_;
    Poco::Net::HTTPClientSession *session_;
    Poco::Net::HTTPRequest *req_;
    Poco::Net::HTTPResponse *res_;
    Poco::Net::WebSocket *ws_;
    WebSocketMessageCallback on_websocket_message_;
    WebSocketSyncCallback on_websocket_sync_;
    void *ctx_;

    std::time_t last_connection_at_;
    std::time_t last_ping_at_;

    std::string api_token_;

    Poco::Mutex mutex_;
};
}  // namespace toggl

#endif  // SRC_WEBSOCKET_CLIENT_H_
