// Copyright 2014 Toggl Desktop developers.

#include "websocket_client.h"

#include <string>
#include <sstream>

#include <json/json.h>  // NOLINT

#include <Poco/Exception.h>
#include <Poco/Logger.h>
#include <Poco/Net/AcceptCertificateHandler.h>
#include <Poco/Net/Context.h>
#include <Poco/Net/HTTPBasicCredentials.h>
#include <Poco/Net/HTTPMessage.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/InvalidCertificateHandler.h>
#include <Poco/Net/PrivateKeyPassphraseHandler.h>
#include <Poco/Net/SSLManager.h>
#include <Poco/Net/WebSocket.h>
#include <Poco/String.h>
#include <Poco/URI.h>

#include "const.h"
#include "https_client.h"
#include "netconf.h"
#include "util/random.h"
#include "urls.h"

namespace toggl {

WebSocketClient::~WebSocketClient() {
    deleteSession();
}

void WebSocketClient::Start(
    void *ctx,
    const std::string &api_token,
    WebSocketMessageCallback on_websocket_message,
    WebSocketSyncCallback on_websocket_sync) {

    poco_check_ptr(ctx);
    poco_check_ptr(on_websocket_message);
    poco_check_ptr(on_websocket_sync);

    if (api_token.empty()) {
        logger().error("API token is empty, cannot start websocket");
        return;
    }

    Poco::Mutex::ScopedLock lock(mutex_);

    if (activity_.isRunning()) {
        return;
    }

    activity_.start();

    ctx_ = ctx;
    on_websocket_message_ = on_websocket_message;
    on_websocket_sync_ = on_websocket_sync;
    api_token_ = api_token;
}

void WebSocketClient::Shutdown() {
    logger().debug("Shutdown");

    if (!activity_.isRunning()) {
        return;
    }
    activity_.stop();  // request stop
    activity_.wait();  // wait until activity actually stops

    deleteSession();

    logger().debug("Shutdown done");
}

error WebSocketClient::createSession() {
    logger().debug("createSession");

    if (HTTPClient::Config.CACertPath().empty()) {
        return error("Missing CA certifcate, cannot start Websocket");
    }

    Poco::Mutex::ScopedLock lock(mutex_);

    deleteSession();

    last_connection_at_ = time(nullptr);
    last_ping_at_ = last_connection_at_;

    error err = TogglClient::TogglStatus.Status();
    if (err != noError) {
        logger().error("Will not start Websocket sessions, ", "because of known bad Toggl status: ", err);
        return err;
    }

    try {
        Poco::URI uri(urls::WebSocket());

        Poco::SharedPtr<Poco::Net::InvalidCertificateHandler>
        acceptCertHandler =
            new Poco::Net::AcceptCertificateHandler(true);

        Poco::Net::Context::VerificationMode verification_mode =
            Poco::Net::Context::VERIFY_RELAXED;
        if (HTTPClient::Config.IgnoreCert()) {
            verification_mode = Poco::Net::Context::VERIFY_NONE;
        }
        Poco::Net::Context::Ptr context = new Poco::Net::Context(
            Poco::Net::Context::CLIENT_USE, "", "",
            HTTPClient::Config.CACertPath(),
            verification_mode, 9, true, "ALL");

        Poco::Net::SSLManager::instance().initializeClient(
            nullptr, acceptCertHandler, context);

        // "ws" and "wss" resolve to ports 80 and 443 in Poco::URI, so the
        // plain/secure split is the only thing the scheme decides here.
        const std::string scheme = uri.getScheme();
        const bool secure = !(scheme == "http" || scheme == "ws");
        if (secure) {
            session_ = new Poco::Net::HTTPSClientSession(uri.getHost(), uri.getPort(), context);
        }
        else {
            session_ = new Poco::Net::HTTPClientSession(uri.getHost(), uri.getPort());
        }

        // Proxy autodetection hands this URL to the platform resolver, which
        // is asked about http/https rather than about ws/wss.
        Poco::URI proxy_probe(uri);
        proxy_probe.setScheme(secure ? "https" : "http");
        Netconf::ConfigureProxy(proxy_probe.toString(), session_);

        // The request path comes from the URI rather than being hardcoded, so
        // that moving the endpoint is a one-line change in urls.cc.
        std::string path = uri.getPathEtc();
        if (path.empty()) {
            path = "/";
        }

        req_ = new Poco::Net::HTTPRequest(
            Poco::Net::HTTPRequest::HTTP_GET, path,
            Poco::Net::HTTPMessage::HTTP_1_1);
        // Servers commonly reject a websocket upgrade whose Origin does not
        // belong to the site; the previous "https://localhost" was accepted
        // only by the old desktop-specific endpoint.
        req_->set("Origin", urls::Main());
        req_->set("User-Agent", HTTPClient::Config.UserAgent());
        // Same credentials the REST API takes: token as user, the literal
        // "api_token" as password.
        Poco::Net::HTTPBasicCredentials(api_token_, "api_token")
        .authenticate(*req_);
        res_ = new Poco::Net::HTTPResponse();
        ws_ = new Poco::Net::WebSocket(*session_, *req_, *res_);
        ws_->setBlocking(false);
        ws_->setReceiveTimeout(Poco::Timespan(3 * Poco::Timespan::SECONDS));
        ws_->setSendTimeout(Poco::Timespan(3 * Poco::Timespan::SECONDS));

        logger().debug("connected to ", uri.toString());

        authenticate();
    } catch(const Poco::Exception& exc) {
        return exc.displayText();
    } catch(const std::exception& ex) {
        return ex.what();
    } catch(const std::string & ex) {
        return ex;
    }

    return noError;
}

void WebSocketClient::sendFrame(const std::string &payload) {
    if (!ws_) {
        return;
    }
    ws_->sendFrame(payload.data(),
                   static_cast<int>(payload.size()),
                   Poco::Net::WebSocket::FRAME_TEXT);
}

void WebSocketClient::sendPing() {
    if (!ws_) {
        return;
    }
    logger().trace("ping");
    last_ping_at_ = time(nullptr);
    ws_->sendFrame("", 0,
                   Poco::Net::WebSocket::FRAME_FLAG_FIN
                   | Poco::Net::WebSocket::FRAME_OP_PING);
}

void WebSocketClient::authenticate() {
    logger().debug("authenticate");

    // The endpoint speaks {"action":..,"parameters":{..}} going up and
    // {"event_type":..} coming down. The upgrade request already carries
    // Basic credentials; this frame covers the case where the server
    // authenticates on the stream instead of on the handshake. It is
    // deliberately harmless if the server has already accepted us.
    Json::Value c;
    c["action"] = "authenticate";
    c["parameters"]["api_token"] = api_token_;

    sendFrame(Json::FastWriter().write(c));
}

const int kWebsocketBufSize = 1024 * 10;

error WebSocketClient::receiveWebSocketMessage(std::string *message, int *flags) {
    *flags = 0;
    std::string json("");
    try {
        char buf[kWebsocketBufSize];
        int n = ws_->receiveFrame(buf, kWebsocketBufSize, *flags);
        if (n > 0) {
            json.append(buf, static_cast<unsigned>(n));
        }
    } catch(const Poco::Exception& exc) {
        return error(exc.displayText());
    } catch(const std::exception& ex) {
        return error(ex.what());
    } catch(const std::string & ex) {
        return error(ex);
    }
    *message = json;
    return noError;
}

const std::string kPong("{\"type\": \"pong\"}");

error WebSocketClient::poll() {
    try {
        Poco::Timespan span(250 * Poco::Timespan::MILLISECONDS);
        if (!ws_->poll(span, Poco::Net::Socket::SELECT_READ)) {
            return noError;
        }

        std::string json("");
        int flags = 0;
        error err = receiveWebSocketMessage(&json, &flags);
        if (err != noError) {
            return err;
        }

        const int opcode = flags & Poco::Net::WebSocket::FRAME_OP_BITMASK;

        // Poco does not answer protocol-level control frames for us, and a
        // keepalive PING carries an empty payload -- which the previous code
        // read as a closed connection and tore the session down for.
        if (Poco::Net::WebSocket::FRAME_OP_CLOSE == opcode) {
            return error("WebSocket closed the connection");
        }
        if (Poco::Net::WebSocket::FRAME_OP_PING == opcode) {
            last_connection_at_ = time(nullptr);
            ws_->sendFrame(json.data(),
                           static_cast<int>(json.size()),
                           Poco::Net::WebSocket::FRAME_FLAG_FIN
                           | Poco::Net::WebSocket::FRAME_OP_PONG);
            return noError;
        }
        if (Poco::Net::WebSocket::FRAME_OP_PONG == opcode) {
            last_connection_at_ = time(nullptr);
            return noError;
        }

        if (json.empty()) {
            // A zero-length data frame with no opcode at all means the peer
            // hung up; anything else empty is just noise worth ignoring.
            return opcode == Poco::Net::WebSocket::FRAME_OP_CONT
                   ? error("WebSocket closed the connection")
                   : noError;
        }

        logger().debug("WebSocket message: ", json);

        last_connection_at_ = time(nullptr);

        if (activity_.isStopped()) {
            return noError;
        }

        handleMessage(json);
    } catch(const Poco::Exception& exc) {
        return error(exc.displayText());
    } catch(const std::exception& ex) {
        return error(ex.what());
    } catch(const std::string & ex) {
        return error(ex);
    }
    return noError;
}

WebSocketClient::FrameAction WebSocketClient::ClassifyFrame(
    const std::string &json) {

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(json, root) || !root.isObject()) {
        return kFrameIgnore;
    }

    // Legacy push frame: the record travels with the notification, so it can
    // be applied without a round trip.
    if (root.isMember("model")) {
        return kFrameLegacyModelUpdate;
    }

    // Legacy application-level keepalive.
    if (root.isMember("type")) {
        return "ping" == root["type"].asString()
               ? kFrameLegacyPing
               : kFrameIgnore;
    }

    const std::string event_type =
        root.isMember("event_type") ? root["event_type"].asString() : "";

    if (Poco::icompare(event_type, "ping") == 0) {
        return kFramePing;
    }
    if (Poco::icompare(event_type, "pong") == 0) {
        return kFrameIgnore;
    }
    if (Poco::icompare(event_type, "error") == 0
            || Poco::icompare(event_type, "unauthorized") == 0) {
        return kFrameServerError;
    }

    // Everything else counts as "something changed upstream". These frames
    // announce a change without carrying the record, so the reaction is to
    // pull over HTTP rather than to apply anything locally. Syncing on an
    // event that did not need one is cheap and debounced; missing one puts
    // the app back to a 15-30 minute lag, so unrecognised frames sync too.
    return kFrameSync;
}

void WebSocketClient::handleMessage(const std::string &json) {
    switch (ClassifyFrame(json)) {
        case kFrameIgnore:
            break;
        case kFrameLegacyModelUpdate:
            on_websocket_message_(ctx_, json);
            break;
        case kFrameLegacyPing:
            sendFrame(kPong);
            break;
        case kFramePing: {
            Json::Value pong;
            pong["action"] = "pong";
            sendFrame(Json::FastWriter().write(pong));
            break;
        }
        case kFrameServerError:
            logger().error("WebSocket server reported an error: ", json);
            break;
        case kFrameSync:
            logger().debug("WebSocket announced a change, will sync");
            on_websocket_sync_(ctx_);
            break;
    }
}

void WebSocketClient::runActivity() {
    int restart_interval = nextWebsocketRestartInterval();
    while (!activity_.isStopped()) {
        if (ws_) {
            error err = poll();
            if (err != noError) {
                logger().error(err);
                logger().debug("encountered an error and will delete session");
                deleteSession();
                logger().debug("will sleep for 10 sec");
                for (int i = 0; i < 20; i++) {
                    if (activity_.isStopped()) {
                        return;
                    }
                    Poco::Thread::sleep(500);
                }
                logger().debug("sleep done");
            }
        }

        if (ws_) {
            // last_connection_at_ moves on every frame received, so with no
            // keepalive of our own an idle connection looks indistinguishable
            // from a dead one and the reconnect below would tear down a
            // perfectly good session within 45 seconds. Ping first, and only
            // treat silence as death once a pong has had time to come back.
            const std::time_t idle_seconds = time(nullptr) - last_connection_at_;
            if (idle_seconds >= kWebsocketStaleSeconds) {
                logger().debug("no traffic for ", static_cast<Poco::Int64>(idle_seconds),
                               " sec, will reconnect");
                deleteSession();
            } else if (idle_seconds >= kWebsocketPingIntervalSeconds
                       && time(nullptr) - last_ping_at_ >= kWebsocketPingIntervalSeconds) {
                sendPing();
            }
        }

        if (!ws_ && time(nullptr) - last_connection_at_ > restart_interval) {
            restart_interval = nextWebsocketRestartInterval();
            logger().debug("restarting");
            error err = createSession();
            if (err != noError) {
                logger().error(err);
                for (int i = 0; i < 20; i++) {
                    if (activity_.isStopped()) {
                        return;
                    }
                    Poco::Thread::sleep(500);
                }
            }
        }

        Poco::Thread::sleep(1000);
    }

    logger().debug("activity finished");
}

void WebSocketClient::deleteSession() {
    logger().debug("deleteSession");

    Poco::Mutex::ScopedLock lock(mutex_);

    if (ws_) {
        delete ws_;
        ws_ = nullptr;
    }
    if (res_) {
        delete res_;
        res_ = nullptr;
    }
    if (req_) {
        delete req_;
        req_ = nullptr;
    }
    if (session_) {
        delete session_;
        session_ = nullptr;
    }

    logger().debug("session deleted");
}

Logger WebSocketClient::logger() const {
    return { "websocket_client" };
}

int WebSocketClient::nextWebsocketRestartInterval() {
    int res = static_cast<int>(Random::next(kWebsocketRestartRangeSeconds)) + 1;
    logger().trace("Next websocket restart in ", res, " seconds");
    return res;
}

}   // namespace toggl
