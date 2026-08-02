// Copyright 2014 Toggl Desktop developers.

#ifndef SRC_HTTPS_CLIENT_H_
#define SRC_HTTPS_CLIENT_H_

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "const.h"
#include "proxy.h"
#include "types.h"
#include "util/logger.h"
#include "toggl_api.h"

#include <Poco/Activity.h>
#include <Poco/Mutex.h>
#include <Poco/Timestamp.h>
#include <Poco/Net/Context.h>
#include <Poco/URI.h>

namespace Poco {
namespace Net {
class HTMLForm;
class Context;
} // namespace Poco::Net
} // namespace Poco

// ---------------------------------------------------------------------------
// Test-only HTTP seam (plan.md 2.4).
//
// TOGGL_TEST_HTTP_SEAM is defined by src/test/CMakeLists.txt, on
// TogglDesktopLibrary itself (PUBLIC, so every consumer of the library sees the
// same class layout), and *only* when TOGGL_PRODUCTION_BUILD is OFF. A shipping
// build therefore never defines it, and every seam member below disappears at
// preprocessing time: TogglClient::GetInstance() is byte-for-byte the original
// Meyers singleton, there is no extra static, no branch and no extra member.
//
// The definition must be identical in every translation unit that includes this
// header, otherwise TogglClient would have two different definitions (ODR) and
// the inlined GetInstance() in context.cc would not see the seam at all. That is
// why it is set on the library target rather than on the test executables.
//
// Belt and braces: refuse to compile if the two are ever combined.
// ---------------------------------------------------------------------------
#if defined(TOGGL_TEST_HTTP_SEAM) && defined(TOGGL_PRODUCTION_BUILD)
#error "TOGGL_TEST_HTTP_SEAM must never be enabled in a production build"
#endif

namespace toggl {

class TOGGL_INTERNAL_EXPORT ServerStatus {
 public:
    ServerStatus()
        : gone_(false)
    , checker_(this, &ServerStatus::runActivity)
    , fast_retry_(true) {}

    virtual ~ServerStatus() {
        stopStatusCheck("destructor");
    }

    error Status();
    void UpdateStatus(const Poco::Int64 status_code);
    void DisableStatusCheck() {
        stopStatusCheck("DisableStatusCheck");
    }

 protected:
    void runActivity();

 private:
    bool gone_;
    Poco::Activity<ServerStatus> checker_;
    bool fast_retry_;

    void setGone(const bool value);
    bool gone();

    void startStatusCheck();
    void stopStatusCheck(const std::string &reason);
    bool checkingStatus();

    Logger logger() const;
};

class TOGGL_INTERNAL_EXPORT HTTPClientConfig {
 public:
    HTTPClientConfig()
        : AppName("")
    , AppVersion("")
    , UseProxy(false)
    , ProxySettings(Proxy())
    , AutodetectProxy(true)
    , ignoreCert(false)
    , caCertPath("") {}
    ~HTTPClientConfig() {}

    std::string AppName;
    std::string AppVersion;
    bool UseProxy;
    toggl::Proxy ProxySettings;
    bool AutodetectProxy;

    std::string UserAgent() const {
        std::stringstream ss;
        ss << AppName + "/" + AppVersion;
        return ss.str();
    }

    bool IgnoreCert() {
        return ignoreCert;
    };
    std::string CACertPath() {
        return caCertPath;
    };
    void SetCACertPath(std::string path) {
        caCertPath = path;
    }
    void SetIgnoreCert(bool ignore) {
        ignoreCert = ignore;
    }

 private:
    bool ignoreCert;
    std::string caCertPath;
};

class TOGGL_INTERNAL_EXPORT HTTPRequest {
 public:
    HTTPRequest()
        : method("")
    , host("")
    , relative_url("")
    , payload("")
    , basic_auth_username("")
    , basic_auth_password("")
    , form(nullptr)
    , query(nullptr)
    , timeout_seconds(kHTTPClientTimeoutSeconds) {}
    virtual ~HTTPRequest() {}

    std::string method;
    std::string host;
    std::string relative_url;
    std::string payload;
    std::string basic_auth_username;
    std::string basic_auth_password;
    Poco::Net::HTMLForm *form;
    Poco::URI::QueryParameters *query;
    Poco::Int64 timeout_seconds;
};

class TOGGL_INTERNAL_EXPORT HTTPResponse {
 public:
    HTTPResponse()
        : body("")
    , err(noError)
    , status_code(0) {}
    virtual ~HTTPResponse() {}

    std::string body;
    error err;
    Poco::Int64 status_code;
};

class TOGGL_INTERNAL_EXPORT HTTPClient {
 public:
    HTTPClient() {}
    virtual ~HTTPClient() {}

    HTTPResponse Post(
        HTTPRequest req) const;

    HTTPResponse Get(
        HTTPRequest req) const;

    HTTPResponse Delete(
        HTTPRequest req) const;

    HTTPResponse Put(
        HTTPRequest req) const;

    static HTTPClientConfig Config;

    void SetCACertPath(std::string path);
    void SetIgnoreCert(bool ignore);

    static error StatusCodeToError(const Poco::Int64 status_code);

    // Strips secrets out of a JSON body so it can be written to the log.
    //
    // This is security-critical: feedback submissions attach the raw log file
    // (see Context::SendFeedback), so anything that reaches the log reaches
    // Toggl support. /me responses carry api_token, and me.payload accepts
    // password / current_password.
    //
    // Bodies that are valid JSON get their sensitive members replaced by
    // kRedactedValuePlaceholder, recursively. Bodies that are not parseable as
    // JSON but mention a sensitive field name are suppressed entirely rather
    // than logged on a best-effort basis.
    //
    // max_chars == 0 means "do not truncate".
    static std::string RedactPayloadForLogging(
        const std::string &payload, std::size_t max_chars = 0);

 protected:
    virtual HTTPResponse request(
        HTTPRequest req, bool_t loggingOn = true) const;

    virtual Logger logger() const;

 private:
    Poco::Net::Context::Ptr context; // share context with many Poco session

    // Per-host throttling state. All three maps are guarded by throttle_m_ --
    // HTTPClient is used from several threads at once (the syncer activity,
    // the timeline uploader, the ServerStatus checker and UI-initiated calls),
    // so they must not be touched without the lock held.
    static Poco::Mutex throttle_m_;

    // We only make requests to a host if this timestamp lies in the past.
    // Only ever set by a 429 response; cleared as soon as the host answers
    // anything else.
    static std::map<std::string, Poco::Timestamp> banned_until_;

    // Earliest moment the next request to a host may be sent. Implements the
    // ~1 req/s per-host token bucket (plan.md 1.8).
    static std::map<std::string, Poco::Timestamp> next_request_at_;

    // Consecutive 429s seen from a host, used to size the incremental backoff.
    static std::map<std::string, unsigned int> rate_limit_hits_;

    // True while the host is in rate-limit backoff. Also drops entries whose
    // backoff has expired.
    bool isRateLimited(const std::string &host) const;

    // Blocks the calling thread until this host's next request slot is due.
    // Reserves the slot before returning, so concurrent callers queue up
    // instead of all waking at the same instant. Never sleeps while holding
    // the lock and never busy-waits.
    void paceRequest(const std::string &host) const;

    // Records a 429 from a host and extends its backoff.
    // retry_after_seconds <= 0 means the server sent no usable Retry-After.
    void noteRateLimited(
        const std::string &host, Poco::Int64 retry_after_seconds) const;

    // Records that a host answered something other than 429, clearing its
    // backoff.
    void noteNotRateLimited(const std::string &host) const;

    error accountLockingError(int remainingLogins) const;

    bool isRedirect(const Poco::Int64 status_code) const;

    virtual HTTPResponse makeHttpRequest(
        HTTPRequest req, bool_t loggingOn = true) const;

    std::string clientIDForRefererHeader() const;

    void resetPocoContext();
};

class TOGGL_INTERNAL_EXPORT SyncStateMonitor {
 public:
    virtual ~SyncStateMonitor() {}

    virtual void DisplaySyncState(const Poco::Int64 state) = 0;
};

class TOGGL_INTERNAL_EXPORT TogglClient : public HTTPClient {
 public:
    static ServerStatus TogglStatus;
    static TogglClient& GetInstance() {
        static TogglClient instance; // static is thread-safe in C++11.
#ifdef TOGGL_TEST_HTTP_SEAM
        // Test-only (plan.md 2.4). Null unless a test explicitly installs a
        // stand-in, so even with the seam compiled in the default behaviour is
        // identical to a release build.
        if (test_seam_) {
            return *test_seam_;
        }
#endif
        return instance;
    }

#ifdef TOGGL_TEST_HTTP_SEAM
    // Install (or, with nullptr, remove) the TogglClient that every
    // TogglClient::GetInstance() call site in the app will use from now on.
    // The stand-in is a subclass overriding the already-virtual
    // HTTPClient::makeHttpRequest, so it can record the outgoing HTTPRequest
    // and answer with a canned v9 response without any socket being opened.
    //
    // Not thread-safe by design: tests install the seam before driving the
    // code under test and remove it afterwards.
    static void SetTestSeam(TogglClient *seam) {
        test_seam_ = seam;
    }
    static TogglClient *TestSeam() {
        return test_seam_;
    }
#endif

    void SetSyncStateMonitor(SyncStateMonitor *monitor = nullptr) {
        monitor_ = monitor;
    }

    // TogglClient exposes two families of request methods.
    //
    // Post/Get/Delete/Put (inherited from HTTPClient, routed through
    // TogglClient::request) are *gated*: they refuse to run while
    // ServerStatus believes the backend is down, and they feed every response
    // status back into ServerStatus. A single 5xx from any of them starts the
    // background status checker, after which every gated request in the app
    // short-circuits with kBackendIsDownError until GET /api/v9/status
    // succeeds.
    //
    // silentPost/silentGet/silentDelete/silentPut bypass that gate entirely by
    // calling HTTPClient::request directly. They still return the real error in
    // HTTPResponse::err, they just do not let one endpoint's outage stop the
    // rest of the app. This is the intended route for *best-effort* endpoints
    // -- feedback, timeline uploads, update checks, countries, analytics --
    // where a 5xx must not stall time-entry sync (plan.md 1.2).
    //
    // Rate-limit backoff and the ~1 req/s pacer live in HTTPClient and apply to
    // both families.
    //
    // Note the asymmetry: silentGet also turns request logging off (it is used
    // by the ServerStatus poller and by analytics, which would otherwise spam
    // the log), while silentPost/silentDelete/silentPut keep logging on.
    HTTPResponse silentPost(
        HTTPRequest req) const;

    HTTPResponse silentGet(
        HTTPRequest req) const;

    HTTPResponse silentDelete(
        HTTPRequest req) const;

    HTTPResponse silentPut(
        HTTPRequest req) const;

 protected:
    virtual HTTPResponse request(HTTPRequest req, bool_t loggingOn = true) const override;
    virtual Logger logger() const override;

#ifdef TOGGL_TEST_HTTP_SEAM
    // Test stand-ins derive from TogglClient, so the constructor has to be
    // reachable by a subclass while the seam is compiled in. Without the seam
    // it stays private, exactly as it always was.
 protected:
#else
 private:
#endif
    TogglClient() {};

 private:
    SyncStateMonitor *monitor_;

#ifdef TOGGL_TEST_HTTP_SEAM
    // C++17 inline static: no definition needed in https_client.cc, which
    // keeps the whole seam inside this header.
    inline static TogglClient *test_seam_ = nullptr;
#endif
};

}  // namespace toggl

#endif  // SRC_HTTPS_CLIENT_H_
