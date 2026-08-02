// Copyright 2014 Toggl Desktop developers.
//
// HTTP mock seam tests -- plan.md 2.4.
//
// These tests install a stand-in TogglClient (see TogglClient::SetTestSeam in
// src/https_client.h) that overrides the already-virtual
// HTTPClient::makeHttpRequest. Nothing below opens a socket: every request the
// production code makes is recorded and answered from a canned response table.
//
// They exist to pin the *outgoing* shapes -- URL, method and payload field
// names -- of everything this migration writes to the API. Both of the
// ship-blocking bugs found so far (the v8 wid/pid bodies POSTed to v9 URLs, and
// the invented app_name/window_title timeline payload) were invisible to the
// model-level unit tests because those only prove SaveToJSON is
// self-consistent. They would not have survived these.

#include "gtest/gtest.h"

#ifdef TOGGL_TEST_HTTP_SEAM

#include <json/json.h>

#include <memory>
#include <string>
#include <vector>

#include "context.h"
#include "https_client.h"
#include "model/client.h"
#include "model/project.h"
#include "model/time_entry.h"
#include "model/timeline_event.h"
#include "timeline_notifications.h"
#include "timeline_uploader.h"
#include "types.h"
#include "urls.h"

#include "test_data.h"

#include <Poco/File.h>
#include <Poco/Mutex.h>
#include <Poco/Thread.h>
#include <Poco/Timestamp.h>

namespace toggl {

namespace {

// How long a test is willing to wait for a background activity (the syncer,
// the timeline uploader, a Poco timer task) to make its request. Generous on
// purpose: the mock answers instantly, so the only thing being waited on is
// the fixed sleep those activities do between iterations. Overshooting costs
// nothing when the request arrives, and stops a slow CI box from flaking.
const int kSeamWaitMillis = 30000;

struct CapturedRequest {
    CapturedRequest() : multipart_form(false) {}

    std::string method;
    std::string host;
    std::string relative_url;
    std::string payload;
    std::string basic_auth_username;
    std::string basic_auth_password;
    bool multipart_form;

    // Parses the captured payload as JSON. Fails the calling test if the body
    // is not parseable, so a mis-shaped body never silently produces an empty
    // Json::Value that then satisfies every isMember() == false assertion.
    Json::Value json() const {
        Json::Value root;
        Json::Reader reader;
        EXPECT_TRUE(reader.parse(payload, root))
                << "payload is not valid JSON: " << payload;
        return root;
    }
};

// A canned answer, matched against the outgoing relative URL by substring.
struct Route {
    Route(const std::string &m, const std::string &u,
          Poco::Int64 s, const std::string &b)
        : method(m), url_contains(u), status_code(s), body(b) {}

    std::string method;  // empty matches any method
    std::string url_contains;
    Poco::Int64 status_code;
    std::string body;
};

// The seam itself: a TogglClient whose makeHttpRequest never touches a socket.
//
// makeHttpRequest is private in HTTPClient, which does not prevent a subclass
// from overriding it (access control applies to calls, not to overriding) --
// this is the plain NVI pattern, and it is why plan.md could describe the
// virtual as "already the seam point".
class RecordingTogglClient : public TogglClient {
 public:
    RecordingTogglClient()
        : default_status_(200)
    , default_body_("{}") {
        // TogglClient's constructor leaves monitor_ alone. The production
        // singleton lives in zero-initialised static storage so it is null
        // there; a stack instance would inherit whatever was on the stack.
        SetSyncStateMonitor();
    }

    void SetDefaultResponse(Poco::Int64 status_code, const std::string &body) {
        Poco::Mutex::ScopedLock lock(mutex_);
        default_status_ = status_code;
        default_body_ = body;
    }

    // First matching route wins, so more specific routes must be added first.
    void AddRoute(const std::string &method,
                  const std::string &url_contains,
                  Poco::Int64 status_code,
                  const std::string &body) {
        Poco::Mutex::ScopedLock lock(mutex_);
        routes_.push_back(Route(method, url_contains, status_code, body));
    }

    std::vector<CapturedRequest> Requests() const {
        Poco::Mutex::ScopedLock lock(mutex_);
        return requests_;
    }

    std::size_t RequestCount() const {
        Poco::Mutex::ScopedLock lock(mutex_);
        return requests_.size();
    }

    // Blocks until a request whose relative URL contains url_contains (and
    // whose method matches, if given) has been captured. Returns false on
    // timeout so the caller can ASSERT and stop rather than dereference a
    // request that never happened.
    bool WaitFor(const std::string &method,
                 const std::string &url_contains,
                 int timeout_millis = kSeamWaitMillis) const {
        const Poco::Timestamp deadline =
            Poco::Timestamp()
            + (Poco::Timestamp::TimeDiff(timeout_millis) * 1000);
        while (Poco::Timestamp() < deadline) {
            if (Has(method, url_contains)) {
                return true;
            }
            Poco::Thread::sleep(50);
        }
        return Has(method, url_contains);
    }

    bool Has(const std::string &method,
             const std::string &url_contains) const {
        Poco::Mutex::ScopedLock lock(mutex_);
        return findLocked(method, url_contains) != nullptr;
    }

    // The first captured request matching method + URL substring. Only call
    // after WaitFor() returned true.
    CapturedRequest First(const std::string &method,
                          const std::string &url_contains) const {
        Poco::Mutex::ScopedLock lock(mutex_);
        const CapturedRequest *found = findLocked(method, url_contains);
        EXPECT_TRUE(found != nullptr)
                << "no captured " << method << " request whose URL contains "
                << url_contains;
        if (!found) {
            return CapturedRequest();
        }
        return *found;
    }

 private:
    const CapturedRequest *findLocked(const std::string &method,
                                      const std::string &url_contains) const {
        for (std::vector<CapturedRequest>::const_iterator it =
            requests_.begin(); it != requests_.end(); ++it) {
            if (!method.empty() && it->method != method) {
                continue;
            }
            if (it->relative_url.find(url_contains) == std::string::npos) {
                continue;
            }
            return &(*it);
        }
        return nullptr;
    }

    HTTPResponse makeHttpRequest(
        HTTPRequest req, bool_t loggingOn) const override {
        (void)loggingOn;

        CapturedRequest captured;
        captured.method = req.method;
        captured.host = req.host;
        captured.relative_url = req.relative_url;
        captured.payload = req.payload;
        captured.basic_auth_username = req.basic_auth_username;
        captured.basic_auth_password = req.basic_auth_password;
        captured.multipart_form = (req.form != nullptr);

        HTTPResponse resp;
        {
            Poco::Mutex::ScopedLock lock(mutex_);
            requests_.push_back(captured);

            resp.status_code = default_status_;
            resp.body = default_body_;
            for (std::vector<Route>::const_iterator it = routes_.begin();
                    it != routes_.end(); ++it) {
                if (!it->method.empty() && it->method != req.method) {
                    continue;
                }
                if (req.relative_url.find(it->url_contains)
                        == std::string::npos) {
                    continue;
                }
                resp.status_code = it->status_code;
                resp.body = it->body;
                break;
            }
        }
        resp.err = HTTPClient::StatusCodeToError(resp.status_code);
        return resp;
    }

    mutable Poco::Mutex mutex_;
    mutable std::vector<CapturedRequest> requests_;
    std::vector<Route> routes_;
    Poco::Int64 default_status_;
    std::string default_body_;
};

// Installs the seam for the lifetime of the guard and removes it afterwards,
// so a failing ASSERT can never leave a dangling stand-in installed for the
// next test in the binary.
class SeamGuard {
 public:
    explicit SeamGuard(TogglClient *seam) {
        TogglClient::SetTestSeam(seam);
    }
    ~SeamGuard() {
        TogglClient::SetTestSeam(nullptr);
    }

 private:
    SeamGuard(const SeamGuard &);
    SeamGuard &operator=(const SeamGuard &);
};

// ---------------------------------------------------------------------------
// A real Context, wired to no-op UI callbacks and a throwaway database, with
// the seam installed for its whole lifetime.
//
// Member order matters: ctx_ is declared last, so it is destroyed first and
// its background threads are joined while the stand-in it may still be calling
// is alive.
// ---------------------------------------------------------------------------
class SeamApp {
 public:
    SeamApp() : guard_(&mock_) {
        Poco::File db_file("seam_test.db");
        if (db_file.exists()) {
            db_file.remove(false);
        }

        // Sensible defaults so no background activity errors out while a test
        // is waiting for the one request it cares about.
        mock_.SetDefaultResponse(200, "{}");
        mock_.AddRoute("GET", "/api/v9/me?", 200, loadTestData());

        ctx_.reset(new Context("tests", "0.1"));

        // Belt and braces: even if the seam were somehow not installed, the
        // test environment refuses to let HTTPClient open a socket.
        ctx_->SetEnvironment("test");
        ctx_->DisableUpdateCheck();

        registerNoopCallbacks();

        poco_assert(noError == ctx_->SetDBPath("seam_test.db"));
    }

    ~SeamApp() {
        ctx_.reset();
    }

    error Login() {
        return ctx_->SetLoggedInUserFromJSON(loadTestData());
    }

    Context *ctx() {
        return ctx_.get();
    }
    RecordingTogglClient &mock() {
        return mock_;
    }

 private:
    void registerNoopCallbacks() {
        GUI *ui = ctx_->UI();
        ui->OnDisplayApp([](const bool_t) {});
        ui->OnDisplayError([](const char_t *, const bool_t) {});
        ui->OnDisplayOverlay([](const int64_t) {});
        ui->OnDisplaySyncState([](const int64_t) {});
        ui->OnDisplayUnsyncedItems([](const int64_t) {});
        ui->OnDisplayOnlineState([](const int64_t) {});
        ui->OnDisplayURL([](const char_t *) {});
        ui->OnDisplayLogin([](const bool_t, const uint64_t) {});
        ui->OnDisplayReminder([](const char_t *, const char_t *) {});
        ui->OnDisplayPomodoro([](const char_t *, const char_t *) {});
        ui->OnDisplayPomodoroBreak([](const char_t *, const char_t *) {});
        ui->OnDisplayAutotrackerNotification(
            [](const char_t *, const uint64_t, const uint64_t) {});
        ui->OnDisplayPromotion([](const int64_t) {});
        ui->OnDisplayTimeEntryList(
            [](const bool_t, TogglTimeEntryView *, const bool_t) {});
        ui->OnDisplayTimeline([](const bool_t, const char_t *,
                                 TogglTimelineChunkView *,
                                 TogglTimeEntryView *, const uint64_t,
                                 const uint64_t) {});
        ui->OnDisplayWorkspaceSelect([](TogglGenericView *) {});
        ui->OnDisplayClientSelect([](TogglGenericView *) {});
        ui->OnDisplayTags([](TogglGenericView *) {});
        ui->OnDisplayTimeEntryEditor(
            [](const bool_t, TogglTimeEntryView *, const char_t *) {});
        ui->OnDisplayTimeEntryAutocomplete([](TogglAutocompleteView *) {});
        ui->OnDisplayProjectAutocomplete([](TogglAutocompleteView *) {});
        ui->OnDisplayMinitimerAutocomplete([](TogglAutocompleteView *) {});
        ui->OnDisplaySettings([](const bool_t, TogglSettingsView *) {});
        ui->OnDisplayTimerState([](TogglTimeEntryView *) {});
        ui->OnDisplayIdleNotification(
            [](const char_t *, const char_t *, const char_t *, const int64_t,
               const char_t *, const char_t *, const char_t *,
               const char_t *) {});
        ui->OnDisplayUpdate([](const char_t *) {});
        ui->OnDisplayUpdateDownloadState([](const char_t *, const int64_t) {});
        ui->OnDisplayMessage([](const char_t *, const char_t *, const char_t *,
                                const char_t *) {});
        ui->OnDisplayAutotrackerRules(
            [](TogglAutotrackerRuleView *, const uint64_t, string_list_t) {});
        ui->OnDisplayProjectColors([](string_list_t, const uint64_t) {});
        ui->OnDisplayCountries([](TogglCountryView *) {});
        ui->OnDisplayHelpArticles([](TogglHelpArticleView *) {});
        ui->OnDisplayOnboarding([](const int64_t) {});
        ui->OnDisplayTimelineUI([](const bool_t) {});
        ui->OnDisplayLoginSSO([](const char_t *) {});
        ui->OnContinueSignIn([]() {});
    }

    RecordingTogglClient mock_;
    SeamGuard guard_;
    std::unique_ptr<Context> ctx_;
};

// A TimelineDatasource that hands the uploader exactly one batch, so the
// production upload path (TimelineUploader::upload -> convertTimelineToJSON ->
// TogglClient::silentPost) runs for real against the seam.
class OneBatchTimelineDatasource : public TimelineDatasource {
 public:
    OneBatchTimelineDatasource() : served_(false), uploaded_(false) {
        event_.SetStartTime(1378362830);
        event_.SetEndTime(1378362860);
        event_.SetFilename("Notepad.exe");
        event_.SetTitle("untitled");
        event_.SetIdle(true);
    }

    error StartAutotrackerEvent(const TimelineEvent &) override {
        return noError;
    }
    error StartTimelineEvent(TimelineEvent *) override {
        return noError;
    }

    error CreateCompressedTimelineBatchForUpload(
        TimelineBatch *batch) override {
        if (served_) {
            return noError;
        }
        served_ = true;

        batch->SetUserID(10471231);
        batch->SetAPIToken("30eb0ae954b536d2f6628f7fec47beb6");
        batch->SetDesktopID("a-desktop-id");

        std::vector<const TimelineEvent *> events;
        events.push_back(&event_);
        batch->SetEvents(std::move(events));
        return noError;
    }

    error MarkTimelineBatchAsUploaded(
        const std::vector<const TimelineEvent *> &) override {
        uploaded_ = true;
        return noError;
    }

    bool uploaded() const {
        return uploaded_;
    }

 private:
    TimelineEvent event_;
    bool served_;
    bool uploaded_;
};

}  // namespace

// ---------------------------------------------------------------------------
// The seam itself
// ---------------------------------------------------------------------------

TEST(HTTPSeam, IsInertUntilATestInstallsIt) {
    // Nothing installed -> GetInstance() hands out the real singleton, exactly
    // as in a build where TOGGL_TEST_HTTP_SEAM is not defined at all.
    ASSERT_TRUE(nullptr == TogglClient::TestSeam());
    TogglClient *real = &TogglClient::GetInstance();

    {
        RecordingTogglClient mock;
        SeamGuard guard(&mock);
        ASSERT_EQ(&mock, TogglClient::TestSeam());
        ASSERT_EQ(static_cast<TogglClient *>(&mock),
                  &TogglClient::GetInstance());
    }

    // ...and the guard puts it back.
    ASSERT_TRUE(nullptr == TogglClient::TestSeam());
    ASSERT_EQ(real, &TogglClient::GetInstance());
}

TEST(HTTPSeam, CapturesRequestsAndInjectsResponses) {
    RecordingTogglClient mock;
    mock.AddRoute("GET", "/api/v9/status", 200, "{\"ok\":true}");
    SeamGuard guard(&mock);

    HTTPRequest req;
    req.host = urls::API();
    req.relative_url = "/api/v9/status";

    const HTTPResponse resp = TogglClient::GetInstance().Get(req);

    ASSERT_EQ(noError, resp.err);
    ASSERT_EQ(Poco::Int64(200), resp.status_code);
    ASSERT_EQ("{\"ok\":true}", resp.body);

    ASSERT_EQ(std::size_t(1), mock.RequestCount());
    ASSERT_EQ("GET", mock.Requests()[0].method);
    ASSERT_EQ("/api/v9/status", mock.Requests()[0].relative_url);
}

TEST(HTTPSeam, InjectedErrorStatusReachesTheCaller) {
    RecordingTogglClient mock;
    mock.SetDefaultResponse(422, "start time is invalid");
    SeamGuard guard(&mock);

    HTTPRequest req;
    req.host = urls::API();
    req.relative_url = "/api/v9/workspaces/1/time_entries";
    req.payload = "{}";

    const HTTPResponse resp = TogglClient::GetInstance().Post(req);

    ASSERT_EQ(error(kUnprocessableEntityError), resp.err);
    ASSERT_EQ("start time is invalid", resp.body);
}

// ---------------------------------------------------------------------------
// Timeline upload -- plan.md 0.1. This is the payload that shipped wrong.
// ---------------------------------------------------------------------------

TEST(HTTPSeam, TimelineUploadBodyIsTheV9Shape) {
    RecordingTogglClient mock;
    mock.SetDefaultResponse(200, "");
    SeamGuard guard(&mock);

    OneBatchTimelineDatasource datasource;
    {
        // Constructing the uploader starts its activity, which uploads the
        // first batch immediately.
        TimelineUploader uploader(&datasource);
        ASSERT_TRUE(mock.WaitFor("POST", "/api/v9/timeline"));
    }

    const CapturedRequest req = mock.First("POST", "/api/v9/timeline");
    ASSERT_EQ("/api/v9/timeline", req.relative_url);
    ASSERT_EQ(urls::TimelineUpload(), req.host);
    ASSERT_EQ("api_token", req.basic_auth_password);
    ASSERT_EQ("30eb0ae954b536d2f6628f7fec47beb6", req.basic_auth_username);

    // POST /api/v9/timeline takes an ARRAY of models.TimelineEvent.
    const Json::Value root = req.json();
    ASSERT_TRUE(root.isArray());
    ASSERT_EQ(Json::ArrayIndex(1), root.size());

    const Json::Value event = root[0];
    ASSERT_EQ("Notepad.exe", event["filename"].asString());
    ASSERT_EQ("untitled", event["title"].asString());
    ASSERT_EQ(Poco::Int64(1378362830), event["start_time"].asInt64());
    ASSERT_EQ(Poco::Int64(1378362860), event["end_time"].asInt64());
    ASSERT_TRUE(event["idle"].asBool());
    ASSERT_EQ("a-desktop-id", event["desktop_id"].asString());

    // Not in models.TimelineEvent. Sending them is what broke the upload.
    ASSERT_FALSE(event.isMember("created_with"));
    ASSERT_FALSE(event.isMember("guid"));

    // And absolutely not the invented v9 shape that was on the branch.
    ASSERT_FALSE(event.isMember("app_name"));
    ASSERT_FALSE(event.isMember("window_title"));

    ASSERT_TRUE(datasource.uploaded());
}

// ---------------------------------------------------------------------------
// Push path -- client, project and time entry bodies.
//
// One Context, one push cycle: Context::pushChanges pushes clients, then
// projects, then time entries, so a single trigger covers all three.
// ---------------------------------------------------------------------------

TEST(HTTPSeam, PushChangesSendsV9ClientProjectAndTimeEntryBodies) {
    SeamApp app;

    app.mock().AddRoute("POST", "/clients", 200,
                        "{\"id\":878318,\"wid\":123456788,"
                        "\"name\":\"Seam Client\"}");
    app.mock().AddRoute("POST", "/projects", 200,
                        "{\"id\":2598323,\"workspace_id\":123456788,"
                        "\"name\":\"Seam Project\"}");
    app.mock().AddRoute("POST", "/time_entries", 200,
                        "{\"id\":89900001,\"workspace_id\":123456788}");

    ASSERT_EQ(noError, app.Login());

    const Poco::UInt64 wid(123456788);

    ASSERT_TRUE(nullptr != app.ctx()->CreateClient(wid, "Seam Client"));
    ASSERT_TRUE(nullptr != app.ctx()->CreateProject(
                    wid, 0, "", "Seam Project", true, ""));

    // Context::Start saves with push_changes = true, which is what actually
    // kicks the syncer into pushChanges().
    ASSERT_TRUE(nullptr != app.ctx()->Start(
                    "seam entry", "", 0, 0, "", "", false, 0, 0, true));

    ASSERT_TRUE(app.mock().WaitFor("POST", "/clients"));
    ASSERT_TRUE(app.mock().WaitFor("POST", "/projects"));
    ASSERT_TRUE(app.mock().WaitFor("POST", "/time_entries"));

    // --- client create -----------------------------------------------------
    const CapturedRequest client_req = app.mock().First("POST", "/clients");
    ASSERT_EQ(urls::API(), client_req.host);
    ASSERT_EQ("/api/v9/workspaces/123456788/clients",
              client_req.relative_url);

    const Json::Value client_body = client_req.json();
    // DELIBERATE, DO NOT "FIX": v9 kept the v8 field name for clients.
    // /api/v9/workspaces/{id}/clients takes wid, not workspace_id. This is the
    // single field in the whole migration most likely to be "corrected" into a
    // regression -- see plan.md, "Verified healthy".
    ASSERT_EQ(Poco::UInt64(123456788), client_body["wid"].asUInt64());
    ASSERT_FALSE(client_body.isMember("workspace_id"));
    ASSERT_EQ("Seam Client", client_body["name"].asString());

    // --- project create ----------------------------------------------------
    const CapturedRequest project_req = app.mock().First("POST", "/projects");
    ASSERT_EQ(urls::API(), project_req.host);
    ASSERT_EQ("/api/v9/workspaces/123456788/projects",
              project_req.relative_url);

    const Json::Value project_body = project_req.json();
    // Projects, unlike clients, did move to the v9 names. Sending wid/cid here
    // silently dropped the workspace and client of every project the app
    // created.
    ASSERT_EQ(Poco::UInt64(123456788), project_body["workspace_id"].asUInt64());
    ASSERT_FALSE(project_body.isMember("wid"));
    ASSERT_FALSE(project_body.isMember("cid"));
    ASSERT_EQ("Seam Project", project_body["name"].asString());

    // --- time entry create -------------------------------------------------
    const CapturedRequest entry_req =
        app.mock().First("POST", "/time_entries");
    ASSERT_EQ(urls::API(), entry_req.host);
    // No trailing /{id}: a create posts to the collection.
    ASSERT_EQ("/api/v9/workspaces/123456788/time_entries",
              entry_req.relative_url);

    const Json::Value entry_body = entry_req.json();
    ASSERT_EQ(Poco::UInt64(123456788), entry_body["workspace_id"].asUInt64());
    ASSERT_FALSE(entry_body.isMember("wid"));
    ASSERT_FALSE(entry_body.isMember("pid"));
    ASSERT_FALSE(entry_body.isMember("tid"));
    ASSERT_EQ("seam entry", entry_body["description"].asString());
    // created_with is required by POST /workspaces/{id}/time_entries.
    ASSERT_TRUE(entry_body.isMember("created_with"));
    ASSERT_FALSE(entry_body["created_with"].asString().empty());
    // A running entry must carry a negative duration (plan.md 1.9 -- the
    // negative-epoch encoding itself is still pending live verification).
    ASSERT_LT(entry_body["duration"].asInt64(), 0);

    // Basic auth is the API token with the literal string "api_token" as the
    // password, on every one of them.
    ASSERT_EQ("api_token", client_req.basic_auth_password);
    ASSERT_EQ("api_token", project_req.basic_auth_password);
    ASSERT_EQ("api_token", entry_req.basic_auth_password);
    ASSERT_EQ("30eb0ae954b536d2f6628f7fec47beb6",
              entry_req.basic_auth_username);
}

TEST(HTTPSeam, UpdatingAnExistingTimeEntryPutsToTheIDScopedURL) {
    SeamApp app;

    // The create response is what gives the entry its server-side ID, and the
    // ID is what turns the next push from a POST into a PUT.
    app.mock().AddRoute("POST", "/time_entries", 200,
                        "{\"id\":89900001,\"workspace_id\":123456788}");
    app.mock().AddRoute("PUT", "/time_entries", 200,
                        "{\"id\":89900001,\"workspace_id\":123456788}");

    ASSERT_EQ(noError, app.Login());

    TimeEntry *entry = app.ctx()->Start(
        "seam entry", "", 0, 0, "", "", false, 0, 0, true);
    ASSERT_TRUE(nullptr != entry);
    // Copy the GUID out immediately -- everything after this runs on the
    // syncer thread. GUIDs are generated locally (EnsureGUID), they are not
    // read back from the API, so this is the only way to name the entry.
    const std::string guid = entry->GUID();
    ASSERT_FALSE(guid.empty());

    ASSERT_TRUE(app.mock().WaitFor("POST", "/time_entries"));

    ASSERT_EQ(noError,
              app.ctx()->SetTimeEntryDescription(guid, "edited by seam"));

    ASSERT_TRUE(app.mock().WaitFor("PUT", "/time_entries"));

    const CapturedRequest req = app.mock().First("PUT", "/time_entries");
    ASSERT_EQ(urls::API(), req.host);
    // An update is a PUT to the ID-scoped URL, not a POST to the collection.
    ASSERT_EQ("/api/v9/workspaces/123456788/time_entries/89900001",
              req.relative_url);

    const Json::Value body = req.json();
    ASSERT_EQ("edited by seam", body["description"].asString());
    ASSERT_EQ(Poco::UInt64(123456788), body["workspace_id"].asUInt64());
    ASSERT_EQ(Poco::UInt64(89900001), body["id"].asUInt64());
    ASSERT_FALSE(body.isMember("wid"));
    ASSERT_FALSE(body.isMember("pid"));
    ASSERT_FALSE(body.isMember("tid"));
}

// ---------------------------------------------------------------------------
// Preferences -- plan.md 0.2.
//
// v8 had POST /timeline_settings; the branch briefly used PUT /api/v9/me,
// which cannot carry record_timeline at all (me.payload does not accept it).
// The real home is the preferences endpoint the app already reads from.
// ---------------------------------------------------------------------------

TEST(HTTPSeam, RecordTimelineIsWrittenToThePreferencesEndpoint) {
    SeamApp app;
    ASSERT_EQ(noError, app.Login());

    app.ctx()->ToggleTimelineRecording(false);

    ASSERT_TRUE(app.mock().WaitFor("POST", "/api/v9/me/preferences/desktop"));

    const CapturedRequest req =
        app.mock().First("POST", "/api/v9/me/preferences/desktop");
    ASSERT_EQ(urls::API(), req.host);
    ASSERT_EQ("/api/v9/me/preferences/desktop", req.relative_url);
    // Not PUT /api/v9/me, and not the v8 /timeline_settings path.
    ASSERT_EQ("POST", req.method);

    // Flat body: record_timeline sits at the root of the preferences object,
    // it is not wrapped in a user/preferences/data envelope.
    const Json::Value body = req.json();
    ASSERT_TRUE(body.isObject());
    ASSERT_TRUE(body.isMember("record_timeline"));
    ASSERT_FALSE(body["record_timeline"].asBool());
    ASSERT_FALSE(body.isMember("data"));
    ASSERT_FALSE(body.isMember("preferences"));

    ASSERT_EQ("api_token", req.basic_auth_password);
    ASSERT_EQ("30eb0ae954b536d2f6628f7fec47beb6", req.basic_auth_username);
}

}  // namespace toggl

#endif  // TOGGL_TEST_HTTP_SEAM
