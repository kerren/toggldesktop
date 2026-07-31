# Toggl API v9 migration plan

Working reference for finishing the migration of TogglDesktop off the shut-down
Toggl API v8. Branch: `claude/toggl-api-upgrade-timeline-1k4x06`.

Status at time of writing: commits `d6f5882` and `c496215` pushed, 71/71 offline
tests passing, **two of the endpoint changes in `d6f5882` are known to be wrong**
and are corrected in Phase 0 below.

---

## 1. Background

API v8 was deprecated 2022-09-26, its `/api/v8/*` endpoints were disabled
2023-04-01 and the version was shut down completely on 2024-05-23. Every call the
app made against v8 has been failing since. v9 is the only version left.

- Base URL: `https://api.track.toggl.com`, paths under `/api/v9`.
- Auth: HTTP Basic, API token as username with the literal string `api_token` as
  password; email + password Basic auth is also still accepted.
- Error bodies are **plain strings**, not JSON error objects.
- Rate limit is roughly 1 request/second per token+IP (leaky bucket), signalled
  by `429`, with no `Retry-After` header. `HTTPClient` already self-bans a host
  for 60s on a 429 (`src/https_client.cc:487-493`).

### Evidence sources

| Source | Used for | Trust |
| --- | --- | --- |
| Toggl OpenAPI v9 spec (`basePath: /api/v9`, `version: 9`) | endpoint paths, payload schemas | High — machine-readable spec |
| `toggl-open-source/toggldesktop` master (2023 snapshot) | reference client behaviour, real server error strings | High |
| Packet capture supplied by the maintainer | that `/api/v9/timeline` exists on `api.track.toggl.com` | Medium — host/path only |
| WebSearch snippets (community, blog) | rate limits, shutdown dates | Medium |

The spec's `host` field is a `localhost:8080` placeholder, so it does **not**
confirm which public host serves any given path. Host choices remain inferred.

---

## 2. Already done

Commits `d6f5882` and `c496215`:

- `src/urls.cc` — added `TrackAPI()` (`api.track.toggl.com`, staging
  `api.track.toggl.space`). `API()` and `TimelineUpload()` return it.
  `WebSocket()` still returns `desktop.track.toggl.com` for `/stream`.
- `GET /api/v8/me` → `GET /api/v9/me` in `Context::me` (`src/context.cc:6062`).
  This was the login/full-sync call; login had been dead for years.
- `POST /api/v8/timeline` → `POST /api/v9/timeline`.
- `POST /api/v8/timeline_settings` → `PUT /api/v9/me`. **Wrong, see 0.2.**
- `POST /api/v8/feedback/web` → `POST /api/v9/feedback/web`.
- `GET /api/v8/desktop_login` → `GET /api/v9/desktop_login`. Spec-confirmed.
- `User::loadUserFromJSON` reads `default_workspace_id` as well as `default_wid`.
- All model `SaveToJSON(int apiVersion)` defaults changed 8 → 9, fixing a silent
  bug where v8 bodies (`wid`/`pid`/`tid`) were POSTed to v9 URLs, dropping the
  project and task of every entry the app created or edited.
- `kAPIV8` removed from `src/const.h`.
- Timeline payload reshaped to `app_name`/`window_title` + ISO 8601.
  **Wrong, see 0.1.**

No `/api/v8` path, no `kAPIV8`, and no v8 payload shape remains in `src/`.

---

## 3. Phase 0 — correct what is already shipped

Ship-blocking. The branch is currently wrong on 0.1 and 0.2.

### 0.1 Revert the timeline payload (~1h)

`POST /api/v9/timeline` takes an array of `models.TimelineEvent`:

```
desktop_id (string), start_time (int), end_time (int),
filename (string), title (string), idle (bool), id (int)
```

That is the **v8 shape**. The endpoint moved to v9; the payload did not. The
`app_name`/`window_title` + ISO 8601 shape currently on the branch came from a
conceptual example that explicitly disclaimed its own key names, and is wrong.

- Restore `filename`, `title`, `start_time`, `end_time` in
  `TimelineEvent::SaveToJSON` (`src/model/timeline_event.cc:70`).
- Add `idle` — a real spec field the client has never sent.
- Keep the `/api/v9/timeline` path and keep `desktop_id`.
- Drop `created_with` and `guid`: neither is in the schema.
- Update the tests added in `d6f5882` (`src/test/app_test.cc:1559,1588`).

### 0.2 Move `record_timeline` to the preferences endpoint (~1h)

`me.payload` accepts only `beginning_of_week, country_id, current_password,
default_workspace_id, email, fullname, password, timezone`. `record_timeline` is
a property of `models.AllPreferences`, served by `/me/preferences/{client}`.
There is no `/timeline_settings` path in v9.

- `Context::onTimelineUpdateServerSettings` (`src/context.cc:1838`):
  `PUT /api/v9/me` → `POST /api/v9/me/preferences/desktop`.
- This is the same endpoint the app already *reads* the setting from
  (`src/context.cc:6303`).

### 0.3 Drop unsupported `/me` query params (~30m)

`GET /me` accepts only `with_related_data`. The code also sends `app_name` and
`since` (`src/context.cc:6060-6068`); neither exists in the spec. `since` in
particular is not doing what the code believes — see 1.1.

### 0.4 Fix `docs/lib/api.md` (~1h)

- Correct the timeline payload section to the real schema.
- Remove the claim that a payload shape change "shows up as repeated failures in
  the log rather than as data loss". That is only true inside the 7-day
  retention window; after it, events are destroyed (see 1.3).
- Document that `Client::SaveToJSON` deliberately keeps `wid` — v9 really did
  keep that name for clients. It looks like a bug and will be "fixed" by mistake
  otherwise (`src/model/client.cc:60`).
- Document the `sync.toggl.com` protocol (section 6) or mark it do-not-extend.

### 0.5 Switch feedback to `/api/v9/feedback` (~1h)

Both `/feedback` and `/feedback/web` exist and both take the multipart form the
code already builds. `/feedback` additionally carries `device_model`,
`build_number` and `operating_system` — it is the desktop-shaped one.

---

## 4. Phase 1 — correctness gaps

### 1.1 Incremental sync is dead (~1d)

`/me` returns no top-level `since` or `server_time`, so `User::Since()` never
advances, `HasValidSinceDate()` stays false, and every sync cycle does a full
pull (`src/model/user.cc:801-825`, `src/context.cc:5379-5397`).

Correct-but-heavy, and heavier requests against a ~1 req/s limit. The real fix
is per-collection `?since=`, which v9 supports on `/me/time_entries`,
`/me/workspaces`, `/me/clients` and `/me/preferences/{client}`.

Decision needed: accept full pulls for now, or implement per-collection since.

### 1.2 One 5xx from any endpoint stalls all sync (~4h)

`TogglClient::request()` (`src/https_client.cc:542-569`) feeds **every** response
status into `ServerStatus::UpdateStatus`. Any 5xx starts the background status
checker, after which every subsequent request short-circuits with
`kBackendIsDownError` until `GET /api/v9/status` succeeds.

So a single 5xx from a low-traffic endpoint — feedback, timeline — stops time
entries syncing. Decouple best-effort endpoints from the status gate.

### 1.3 Timeline events are destroyed after 7 days (~3h)

`User::CompressTimeline` deletes any event older than `kTimelineSecondsToKeep`
(7 days) **unconditionally**, with no check of `Uploaded()`
(`src/model/user.cc:1397-1400`). `CompressedTimeline` then skips anything with
`DeletedAt() > 0` (`src/model/user.cc:1484-1486`), so it is gone for good.

Pre-existing, but 0.1 is exactly the scenario that triggers it: if uploads are
rejected, activity data bleeds away weekly and silently. Skip deletion for
events that have not been uploaded.

### 1.4 No 422 handling (~3h)

`HTTPClient::StatusCodeToError` (`src/https_client.cc:187-230`) handles
400/401/402/403/404/410/418/429/500-505 but not 422. Anything unmatched returns
`kCannotConnectError`, so a validation rejection is reported to the user as
"cannot connect" and retried forever. Confirm which codes v9 uses for validation
failures, then handle them explicitly.

### 1.5 `pushEntries` can duplicate entries server-side (~2h)

`Context::pushEntries` (`src/context.cc:6012-6028`) requires `root["id"]` in the
create response and silently `continue`s if absent, leaving `ID()` at 0 so
`NeedsPOST()` stays true. If the server *did* create the record, the next cycle
re-POSTs it — indefinitely. Needs a real create-response to confirm the shape.

### 1.6 `ResolveError` matches hardcoded English v8 error strings (~4h)

`TimeEntry::ResolveError` and friends (`src/model/time_entry.cc:31-100`) match
substrings like `"Time entry not found"`. v9 returns plain-string bodies that may
be reworded. Conditions that used to self-correct now leave the entry stuck
unsynced, since `ValidationError` blocks `NeedsPush()`. Capture real v9 error
bodies and diff.

### 1.7 Bare `/tags` and `/tasks` do not exist in v9 (~1h)

`Tag::ModelURL()` (`src/model/tag.cc:45`) and `Task::ModelURL()`
(`src/model/task.cc:58`) return `/api/v9/tags` and `/api/v9/tasks`. Neither path
exists — they must be `/api/v9/workspaces/{workspace_id}/tags` and
`.../tasks`. Dead code today (only clients, projects and time entries are
pushed), but a trap for whoever wires up tag/task sync.

---

## 5. Phase 2 — verification infrastructure

### 2.1 Add a Linux CI job (~3h)

`.github/workflows/main.yml` currently runs Windows GUI builds only; every
macOS/Linux job is commented out. **No CI builds or tests the C++ core at all** —
the `wid`/`pid` bug would have shipped undetected.

Proven recipe (verified in this container, 71/71 pass):

```sh
apt-get install -y qtbase5-dev qtbase5-private-dev libqt5x11extras5-dev \
  libqt5networkauth5-dev libssl-dev libpoco-dev libjsoncpp-dev \
  libxmu-dev libxss-dev cmake build-essential pkg-config

cmake -S . -B build -DTOGGL_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target TogglDesktopLibrary TogglAppTest -j4
cd build && ./src/test/TogglAppTest
```

Two gotchas worth encoding in the job:

- The binary must run with its CWD one level below the repo root; fixtures are
  loaded via relative paths like `../testdata/...` (`src/test/app_test.cc:353`).
- Qt5 must be *installed* even though neither `TogglDesktopLibrary` nor
  `TogglAppTest` links it, because `find_package(Qt5... REQUIRED)` at
  `CMakeLists.txt:52-58` is unconditional. Making that conditional would let CI
  skip Qt entirely.

Build against **system** Poco/OpenSSL/jsoncpp, not the vendored copies.

### 2.2 Add a v9 `/me` fixture (~2h)

`testdata/me.json` is a v8 `{"since":…, "data":{…}}` envelope feeding ~50+ tests,
so the suite exercises the *fallback* branch of every `isMember("wid") ? … : …`
check and never the v9 branch. Nothing currently proves a real v9 login response
parses. Add `testdata/me_v9.json` — flat, `workspace_id`/`project_id`/
`client_id`/`task_id` throughout, `default_workspace_id`, no wrapper.

### 2.3 Stop `TogglApiTest` making live network calls (~1h)

`urls::requests_allowed_` defaults to true and is only changed by
`Context::SetEnvironment` (`src/context.cc:2399`), which the test harness never
calls. So `toggl_login`, `toggl_sync`, `toggl_add_project` and friends
(`src/test/toggl_api_test.cc:848-996`) issue **real** HTTP requests to staging
with throwaway credentials. One-line fix: set the environment to `test` in the
fixture. Do not add `TogglApiTest` to CI before this is fixed.

### 2.4 Add an HTTP mock seam (~1d)

`HTTPClient::makeHttpRequest` is already virtual. Add a test-only hook so tests
can capture the outgoing request (assert the URL is `/api/v9/...` and the payload
field names are right) and inject canned v9 responses. This is what would have
caught both the `wid`/`pid` bug and the timeline payload error.

### 2.5 Fix logging (~3h)

- Outgoing request payloads are **never** logged at any level, so a rejected
  payload shows the status code but not what was sent — exactly the wrong
  trade-off for this migration. Log bodies at debug.
- `src/context.cc:5491,5614,5626` dump full unredacted user JSON to `std::cerr`
  unconditionally, bypassing the logger — no level control, no rotation. Remove
  or gate them.
- The `Authorization` header is already stripped before logging
  (`src/https_client.cc:419`). Keep it that way; note that feedback submissions
  attach the raw log file (`src/context.cc:1929`), so anything logged reaches
  support.

---

## 6. Open question: is `sync.toggl.com` still alive?

There is a **second sync protocol** — `Context::pullBatchedUserData` /
`pushBatchedChanges` (`src/context.cc:5450-5622`) — talking to `urls::SyncAPI()`
(`sync.toggl.com`) via `GET /pull` and `POST /push/{uuid}`. It is outside
`/api/v9` entirely, so nothing in this migration touches it.

It is selected at `src/context.cc:5153` by
`user_->AlphaFeatureSettings->IsSyncEnabled()`, which defaults to false and is
flipped by a **server-sent** alpha-feature flag read from
`/me/preferences/desktop`. So REST is what runs for every account today — but
Toggl can move an account onto this path at any time without a client release.

If that protocol died with v8, affected users get broken sync with no client-side
warning. **This needs a backend answer, not engineering time.** Until then, treat
it as live and do not delete it.

Note: `SyncPayload()`/`SyncMetadata()` are hand-built and independent of
`SaveToJSON()`, so the Phase 0 and `c496215` changes do not affect this path.

---

## 7. Phase 3 — live verification (requires a real API token)

Cannot be done from the dev container: egress blocks all `toggl.com` hosts.
Roughly in priority order:

1. `POST /api/v9/timeline` with the corrected payload — which **host** serves it
   (`api.track.toggl.com` vs `desktop.track.toggl.com`) and does it return 2xx.
   The reference client posted to `/api/v8/timeline` on the desktop host, so the
   host is genuinely uncertain. Highest-value single check.
2. `POST /api/v9/me/preferences/desktop` with `record_timeline` — does it persist
   and round-trip through a follow-up `GET`.
3. `POST /api/v9/feedback` and `GET /api/v9/desktop_login` return 4xx not 5xx
   (see 1.2 — a 5xx here stalls everything).
4. Does `api.track.toggl.space` resolve and serve? It is a guess. If wrong, all
   staging verification is silently broken from the start.
5. Capture a real `POST .../time_entries` create response; diff against what
   `pushEntries` expects (see 1.5).
6. Capture 2-3 real v9 error bodies; diff against `time_entry.cc`/`error.cc`
   (see 1.6).
7. `desktop.track.toggl.com/stream` still accepts the websocket upgrade. Failure
   is silent and permanent: it retries every 45s forever and degrades to ~15-30
   minute polling with no user-visible error.

`src/test/online_test.cc` is an existing live end-to-end suite (signs up a
throwaway user, creates entries and projects) that nothing currently runs. It is
the cheapest way to cover most of the above. It has no production guard — do not
build it with `TOGGL_PRODUCTION_BUILD=ON`.

---

## 8. Sequencing

1. **Phase 0** — known-wrong code, do first.
2. **2.1 + 2.2** — get CI and a v9 fixture under the work before changing more.
3. **Phase 1** — in numbered order; 1.2 and 1.3 are the ones with user-visible
   blast radius.
4. **2.3 – 2.5** — as capacity allows.
5. **Phase 3** — gates the release.

Rough totals: ~2 days for Phase 0 + 2.1 + 2.2; ~3 days for Phase 1; Phase 3 is
maintainer time, not engineering time.

## 9. Explicitly not worth doing

- **Upgrading vendored Poco 1.9 / OpenSSL 1.1.0.** System libraries already build
  and pass cleanly; `USE_BUNDLED_LIBRARIES=OFF` is the default. Multi-day effort
  for no payoff. Note `third_party/CMakeLists.txt:30-31` already downgrades the
  vendored Poco to C++11 because it uses features removed in C++17.
- **The root `Makefile`.** A dead macOS-only build path superseded by CMake.
  Delete it in a separate cleanup rather than maintaining it.

---

## Appendix: spec-verified endpoint status

| Endpoint | Status |
| --- | --- |
| `GET /api/v9/me` | Correct. Only param is `with_related_data`; returns a flat user object; related collections are clients, projects, tags, tasks, time_entries, workspaces. No `since`/`server_time` in response. |
| `PUT /api/v9/me` | Exists, but **cannot** set `record_timeline`. |
| `GET/POST /api/v9/me/preferences/{client}` | Correct; `client` is `desktop` or `web`. Carries `record_timeline`. |
| `GET /api/v9/me/time_entries` | Correct. Supports `since`, `before`, `start_date`, `end_date`. |
| `GET /api/v9/me/workspaces` | Correct. |
| `POST /api/v9/me/accept_tos` | Correct. |
| `GET/POST /api/v9/workspaces/{id}/preferences` | Correct. |
| `POST/PUT/DELETE /api/v9/workspaces/{id}/time_entries[/{id}]` | Correct. `created_with` and `workspace_id` required in body. Running entry duration should be negative; `-1` is recommended, not mandatory. |
| `POST/PUT/DELETE /api/v9/workspaces/{id}/projects[/{id}]` | Correct. |
| `POST/PUT/DELETE /api/v9/workspaces/{id}/clients[/{id}]` | Correct. Client workspace field is still `wid`. |
| `/api/v9/tags`, `/api/v9/tasks` | **Wrong** — must be workspace-scoped. |
| `POST /api/v9/timeline` | Path correct; payload is the v8 shape (see 0.1); host unverified. |
| `POST /api/v9/feedback`, `/api/v9/feedback/web` | Both exist, both multipart. Prefer `/feedback`. |
| `POST /api/v9/desktop_login_tokens`, `GET /api/v9/desktop_login` | Correct. |
| `GET /api/v9/status`, `POST /api/v9/signup`, `GET /api/v9/countries` | Correct. |
| `POST /api/v9/me/enable_sso` | Correct. |
| `GET /api/v9/auth/saml2/login` | Correct as **GET** with query params — which is what the code already does. |
| Reports API | v2 is gone; only `/reports/api/v3/...` exists. Not currently used by this app. |
