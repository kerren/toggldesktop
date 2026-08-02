# Toggl API v9 migration plan

Working reference for finishing the migration of TogglDesktop off the shut-down
Toggl API v8, structured for delegation to coding agents.

Branch: `claude/api-v8-v9-upgrade-436x05`.

**Baseline re-verified in this container on 2026-08-01:**

- `cmake -S . -B build -DTOGGL_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release` → configures clean.
- `cmake --build build --target TogglDesktopLibrary TogglAppTest` → builds clean.
- `./src/test/TogglAppTest` → **71 tests from 14 test cases, 71 passed.**
- No `/api/v8` path, no `kAPIV8`, and no v8-only payload shape remains anywhere in `src/`.

Two of the endpoint changes already on the branch are still known to be wrong and
are corrected in Phase 0.

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
| Direct read of this working tree (2026-08-01) | every code claim, line number and build/test result below | High — verified in container |

The spec's `host` field is a `localhost:8080` placeholder, so it does **not**
confirm which public host serves any given path. Host choices remain inferred.

`engineering.toggl.com` returns **403** from this container, so the spec cannot be
re-fetched here. Every spec-derived claim below carries over from the earlier
session at the trust level shown above; the code claims have all been re-checked.

---

## 2. Already done

Commits `d6f5882` and `c496215` (merged to `master` via PR #1):

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

### Verified healthy — do not "fix" these

Re-checked in this working tree; they look wrong at a glance but are correct:

- **`Client::SaveToJSON` sends `wid`, not `workspace_id`** (`src/model/client.cc:60`).
  v9 genuinely kept the old name for clients. There is already a comment saying so.
- **The read path is dual-shape by design.** `LoadFromJSON` in `tag.cc:35`,
  `task.cc:43-49`, `project.cc:111-125`, `client.cc:48-50` and `time_entry.cc:479-481`
  all branch `isMember("wid") ? v8 : v9` (and `hex_color`/`color`, `pid`/`project_id`,
  `cid`/`client_id`). Keep both branches — the local SQLite cache still holds
  v8-shaped rows for existing installs.
- **`GET /api/v9/me/time_entries?since=` at `src/context.cc:5306`** is already the
  correct v9 per-collection form. It is the model for fixing 1.1.
- **`silentGet` / `silentPost`** (`src/https_client.cc:570-580`) already bypass the
  `ServerStatus` gate by calling `HTTPClient::request` directly. This is the existing
  mechanism to reuse for 1.2 — the fix is call-site selection, not new plumbing.
- **The GUI layers hold no API knowledge.** `src/ui/{linux,osx,windows}` contain no
  `/api/` paths and no Toggl API hosts. The migration is entirely inside the C++ core.
- **No Reports API usage anywhere.** Nothing to migrate off Reports v2.

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

Current state confirmed at `src/model/timeline_event.cc:70-97`: the `apiVersion <= 8`
branch still holds the correct fields, the `else` branch emits the wrong ones, and
`kTimelineAPIVersion = 9` (`src/timeline_uploader.h:19`) selects the wrong branch.

- Restore `filename`, `title`, `start_time`, `end_time` in
  `TimelineEvent::SaveToJSON`.
- Add `idle` — a real spec field the client has never sent. `TimelineEvent::Idle`
  already exists as a property (`src/model/timeline_event.h:30`) and is simply
  never serialised.
- Keep the `/api/v9/timeline` path and keep `desktop_id`.
- Drop `created_with` and `guid`: neither is in the schema.
- Collapse the version branch entirely rather than leaving a dead `apiVersion <= 8`
  arm — there is no v8 to fall back to.
- Update the tests added in `d6f5882` (`src/test/app_test.cc:1559,1588`).

### 0.2 Move `record_timeline` to the preferences endpoint (~1h)

`me.payload` accepts only `beginning_of_week, country_id, current_password,
default_workspace_id, email, fullname, password, timezone`. `record_timeline` is
a property of `models.AllPreferences`, served by `/me/preferences/{client}`.
There is no `/timeline_settings` path in v9.

- `Context::onTimelineUpdateServerSettings` (`src/context.cc:1838-1844`):
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
- Document the dual-shape read path as intentional (see "Verified healthy" above).
- Document the `sync.toggl.com` protocol (section 6) or mark it do-not-extend.

### 0.5 Switch feedback to `/api/v9/feedback` (~1h)

Both `/feedback` and `/feedback/web` exist and both take the multipart form the
code already builds (`src/context.cc:1944`). `/feedback` additionally carries
`device_model`, `build_number` and `operating_system` — it is the desktop-shaped one.

---

## 4. Phase 1 — correctness gaps

### 1.1 Incremental sync is dead (~1d)

`/me` returns no top-level `since` or `server_time`, so `User::Since()` never
advances, `HasValidSinceDate()` stays false, and every sync cycle does a full
pull (`src/model/user.cc:805-813`, `src/context.cc:5389-5391`).

Correct-but-heavy, and heavier requests against a ~1 req/s limit. The real fix
is per-collection `?since=`, which v9 supports on `/me/time_entries`,
`/me/workspaces`, `/me/clients` and `/me/preferences/{client}`.

Decision needed: accept full pulls for now, or implement per-collection since.
**Recommendation: accept full pulls for now**, and do 1.8 (pacing) instead — it
removes the same pressure for a fraction of the risk. Revisit if users report
slow sync.

### 1.2 One 5xx from any endpoint stalls all sync (~4h)

`TogglClient::request()` (`src/https_client.cc:542-569`) feeds **every** response
status into `ServerStatus::UpdateStatus`. Any 5xx starts the background status
checker, after which every subsequent request short-circuits with
`kBackendIsDownError` until `GET /api/v9/status` succeeds.

So a single 5xx from a low-traffic endpoint — feedback, timeline — stops time
entries syncing. Decouple best-effort endpoints from the status gate by routing
them through the existing `silentGet`/`silentPost`, which already skip the gate.

### 1.3 Timeline events are destroyed after 7 days (~3h)

`User::CompressTimeline` deletes any event older than `kTimelineSecondsToKeep`
(7 days) **unconditionally**, with no check of `Uploaded()`
(`src/model/user.cc:1397-1400`). `CompressedTimeline` then skips anything with
`DeletedAt() > 0` (`src/model/user.cc:1484-1486`), so it is gone for good.

Pre-existing, but 0.1 is exactly the scenario that triggers it: if uploads are
rejected, activity data bleeds away weekly and silently. Skip deletion for
events that have not been uploaded.

Add an upper bound too — an account that can never upload (permanently revoked
token) would otherwise grow the local DB without limit. Suggest keeping
un-uploaded events up to a hard ceiling (e.g. 90 days) and logging when the
ceiling evicts anything.

### 1.4 No 422 handling (~3h)

`HTTPClient::StatusCodeToError` (`src/https_client.cc:187-230`) handles
400/401/402/403/404/410/418/429/500-505 but not 422. Anything unmatched returns
`kCannotConnectError`, so a validation rejection is reported to the user as
"cannot connect" and retried forever. Confirm which codes v9 uses for validation
failures, then handle them explicitly.

Fold in the adjacent mapping bug: **429 also returns `kCannotConnectError`**
(`src/https_client.cc:216`), which `IsNetworkingError` (`src/error.cc:16`) then
classifies as offline. So hitting the rate limit sets `trigger_sync_ = false`
(`src/context.cc:5993`) and shows the user a connectivity error for what is
actually successful throttling. Give 429 its own error constant that is *not* a
networking error, and let the existing 60s host ban do the backoff.

### 1.5 `pushEntries` can duplicate entries server-side (~2h)

`Context::pushEntries` (`src/context.cc:6012-6028`) requires `root["id"]` in the
create response and silently `continue`s if absent, leaving `ID()` at 0 so
`NeedsPOST()` stays true. If the server *did* create the record, the next cycle
re-POSTs it — indefinitely. Needs a real create-response to confirm the shape.

Note `TimeEntry::SaveToJSON` already sends `guid` on v9 bodies
(`src/model/time_entry.cc:547`). If v9 honours `guid` as an idempotency key this
is already mitigated; if it ignores it, the duplicate risk is live. Settle this
in Phase 3 before writing the fix.

### 1.6 `ResolveError` matches hardcoded English v8 error strings (~4h)

`TimeEntry::ResolveError` and friends (`src/model/time_entry.cc:31-100`) match
substrings like `"Time entry not found"`. v9 returns plain-string bodies that may
be reworded. Conditions that used to self-correct now leave the entry stuck
unsynced, since `ValidationError` blocks `NeedsPush()`. Capture real v9 error
bodies and diff.

Independent of what the strings turn out to be, add a **fallback**: an
unrecognised 4xx body currently pins the entry as permanently unsynced with no
recovery path and no log line naming the unmatched string. Log the unmatched body
at warning and cap retries so one bad entry cannot wedge the push queue forever.

### 1.7 Bare `/tags` and `/tasks` do not exist in v9 (~1h)

`Tag::ModelURL()` (`src/model/tag.cc:45`) and `Task::ModelURL()`
(`src/model/task.cc:58`) return `/api/v9/tags` and `/api/v9/tasks`. Neither path
exists — they must be `/api/v9/workspaces/{workspace_id}/tags` and
`.../tasks`. Dead code today (`pushChanges` only calls `pushClients`,
`pushProjects` and `pushEntries` — `src/context.cc:5711,5727,5752`), but a trap
for whoever wires up tag/task sync.

### 1.8 No client-side request pacing (~4h) — *new*

Nothing throttles outbound requests, and the push path issues **one request per
dirty model** (`src/context.cc:5799,5855,5950`). A sync cycle after offline use
fires: `/me` + `/me/preferences/desktop` + one `/workspaces/{id}/preferences` per
*business* workspace (`src/context.cc:6219-6233`) + N client POSTs + N project
POSTs + N entry POSTs — back to back, against a ~1 req/s limit.

Twenty queued entries is twenty requests in a burst. The server answers 429, the
client bans the host for 60s (`src/https_client.cc:487`), and — because of the
429 mapping in 1.4 — reports it as being offline. The user sees sync stop for a
minute with a connectivity error, then repeat.

Add a minimum inter-request interval in `HTTPClient::request` (a simple
per-host token bucket at ~1 req/s), and on 429 prefer an incremental backoff over
the flat 60s ban. Pair with 1.4 — the two together are what make heavy accounts
usable.

### 1.9 Running-entry duration still uses the v8 encoding (~2h) — *new*

`TimeEntry::SetDurationInSeconds(-start)` (`src/model/time_entry.cc:307,334`)
encodes a running entry as the **negative epoch start time**, the v8 convention.
v9 documents running entries as "negative duration, `-1` recommended".

Negative epoch *is* negative, so this most likely still works — but it is an
untested assumption sitting on the app's single most important write path. Verify
in Phase 3 before changing anything; if v9 accepts it, document it and move on
rather than churning the code.

---

## 5. Phase 2 — verification infrastructure

### 2.1 Add a Linux CI job (~3h)

`.github/workflows/main.yml` is 587 lines of which every macOS and Linux job is
commented out (`main.yml:12,121,190,211,269,328,359`); only three
`windows-2019` GUI jobs run. **No CI builds or tests the C++ core at all** — the
`wid`/`pid` bug would have shipped undetected.

Proven recipe (re-verified in this container 2026-08-01, 71/71 pass):

```sh
apt-get install -y qtbase5-dev qtbase5-private-dev libqt5x11extras5-dev \
  libqt5networkauth5-dev libssl-dev libpoco-dev libjsoncpp-dev \
  libxmu-dev libxss-dev cmake build-essential pkg-config

cmake -S . -B build -DTOGGL_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target TogglDesktopLibrary TogglAppTest -j"$(nproc)"
cd build && ./src/test/TogglAppTest
```

Three gotchas worth encoding in the job:

- The binary must run with its CWD one level below the repo root; fixtures are
  loaded via relative paths like `../testdata/...` (`src/test/app_test.cc:353`).
- Qt5 must be *installed* even though neither `TogglDesktopLibrary` nor
  `TogglAppTest` links it, because `find_package(Qt5... REQUIRED)` at
  `CMakeLists.txt:52-59` is unconditional. Making that conditional would let CI
  skip Qt entirely and cut several minutes off the job.
- Build only the two named targets. `TOGGL_BUILD_TESTS=ON` also defines
  `TogglApiTest` and `TogglOnlineTest` (`src/test/CMakeLists.txt`), both of which
  make live network calls — see 2.3.

Build against **system** Poco/OpenSSL/jsoncpp, not the vendored copies.

### 2.2 Add a v9 `/me` fixture (~2h)

`testdata/me.json` is a v8 `{"since":…, "data":{…}}` envelope with `default_wid`,
feeding ~50+ tests, so the suite exercises the *fallback* branch of every
`isMember("wid") ? … : …` check and never the v9 branch. Nothing currently proves
a real v9 login response parses.

Add `testdata/me_v9.json` — flat, no wrapper, no `since`. It must exercise **all**
of the dual-shape forks listed under "Verified healthy", or it proves less than it
appears to:

| File | v8 key | v9 key |
| --- | --- | --- |
| `user.cc` | `default_wid` | `default_workspace_id` |
| `time_entry.cc:479` | `wid` | `workspace_id` |
| `project.cc:111,119,123` | `hex_color`, `wid`, `cid` | `color`, `workspace_id`, `client_id` |
| `task.cc:43,47` | `pid`, `wid` | `project_id`, `workspace_id` |
| `client.cc:48` | `wid` | `workspace_id` |
| `tag.cc:35` | `wid` | `workspace_id` |

Strongest acceptance test: a **parity test** that loads `me.json` and `me_v9.json`
into two `User` objects and asserts the resulting model state is identical field
for field. That pins both branches against each other and will catch a
half-migrated read path.

### 2.3 Stop `TogglApiTest` making live network calls (~1h)

`urls::requests_allowed_` defaults to true (`src/urls.cc:21`) and is only changed
by `Context::SetEnvironment` (`src/context.cc:2399`), which the `testing::App`
fixture (`src/test/toggl_api_test.cc:304-324`) never calls. So `toggl_login`,
`toggl_sync`, `toggl_add_project` and friends (`src/test/toggl_api_test.cc:848-996`)
issue **real** HTTP requests to staging with throwaway credentials.

One-line fix: call `toggl_set_environment(ctx_, STR("test"))` in the fixture
constructor — `TEST(toggl_api, toggl_set_environment)` at line 718 already proves
the call works. Do not add `TogglApiTest` to CI before this is fixed.

### 2.4 Add an HTTP mock seam (~1d)

`HTTPClient::makeHttpRequest` is already virtual. Add a test-only hook so tests
can capture the outgoing request (assert the URL is `/api/v9/...` and the payload
field names are right) and inject canned v9 responses. This is what would have
caught both the `wid`/`pid` bug and the timeline payload error.

Minimum coverage once the seam exists — one assertion per shape this migration
got wrong or could get wrong: time entry create/update body, project create body,
client create body (asserting `wid`, not `workspace_id`), timeline upload body,
and the preferences POST from 0.2.

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
  support. Redact `api_token` out of logged bodies before enabling body logging —
  `/me` responses contain it.

---

## 6. Phase 2b — dead non-Toggl integrations *(new section)*

Not API v8, but equally "the app calls a service that no longer exists". In scope
for *"upgrade this project fully so that it still works"*.

### 2b.1 Google Analytics is dead (~4h)

`src/analytics.cc:142-178` builds Universal Analytics hits:

```
https://ssl.google-analytics.com/collect?v=1&tid=UA-3215787-27&cid=…&t=event…
```

Universal Analytics stopped processing data on 2023-07-01; `v=1` hits against a
`UA-` property are discarded. All **31 call sites** in `src/context.cc` are
firing into a void.

Mitigating detail: these go through `silentGet` (`src/analytics.cc:161,328`),
which bypasses the `ServerStatus` gate — so unlike 1.2, dead analytics does *not*
stall sync. This is a correctness/cleanliness item, not an outage.

Also note `tid=UA-3215787-27` is **Toggl's own property**. A fork cannot send to
it and should not try.

**Recommendation: gut the implementation, keep the interface.** Make the
`runTask()`/`makeReq()` bodies no-ops (or drop the HTTP call and keep a debug log
line) so all 31 `analytics_.Track*` call sites in `context.cc` compile unchanged.
That contains the whole change to `src/analytics.{cc,h}` and keeps this slice off
the `context.cc` contention path. Deleting the call sites is a larger, riskier
diff for no additional benefit.

### 2b.2 Update check points at an archived upstream (~3h)

Two independent update paths, both aimed at the discontinued upstream project:

- `src/context.cc:1462-1463` — `https://toggl.github.io/toggldesktop/assets/updates-link.txt`,
  then follows whatever URL that returns.
- `src/context.cc:1606-1612` — `https://raw.githubusercontent.com/toggl-open-source/toggldesktop/{master,mac-deprecation-message}/releases/message.json`.

For this fork these are at best stale and at worst will surface an upstream
deprecation notice to users of a maintained fork. Decide: disable the in-app
update check, or repoint at this fork's own release metadata. Whichever is chosen,
it must fail closed and silently — `UpdateChannel` errors currently reach the UI.

### 2b.3 Trivia (~30m)

- `.travis.yml` is dead config for a CI service the project no longer uses.
- The root `Makefile` is a dead macOS-only build path superseded by CMake
  (see section 9).

---

## 7. Open question: is `sync.toggl.com` still alive?

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
`Sync.LegacyFormat` and `Sync.BatchedFormat` in `TogglAppTest` cover it offline
and must keep passing.

---

## 8. Phase 3 — live verification (requires a real API token)

Cannot be done from the dev container: egress blocks `toggl.com` hosts, and
`engineering.toggl.com` returns 403. Roughly in priority order:

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
   `pushEntries` expects, and check whether `guid` is honoured as an idempotency
   key (see 1.5).
6. Capture 2-3 real v9 error bodies; diff against `time_entry.cc`/`error.cc`
   (see 1.6).
7. Start a timer and confirm v9 accepts a **negative-epoch** duration, not just
   `-1` (see 1.9).

   *Confirmed by code inspection in W1-C (2026-08-01), encoding deliberately
   left unchanged:* running entries are sent as `duration = -start`, i.e. the
   negated Unix epoch seconds of the start time — exactly the v8 convention.
   There are **two** sites, not one: `TimeEntry::SetStartUserInput`
   (`src/model/time_entry.cc:307`) and `TimeEntry::SetDurationUserInput`
   (`:334`), the latter reaching the same state via
   `SetStartTime` + `SetDurationInSeconds(-start, true)`. Both now carry a
   comment recording that this is pending live verification. `-start` is
   genuinely negative so it should satisfy v9's "negative duration" rule, but
   this is an untested assumption on the app's most important write path.
   **Test:** start a timer against v9 and confirm the negative-epoch value is
   accepted, not just `-1`.
8. Does `GET /me?with_related_data=true` return the full time-entry history or a
   bounded window? v8 returned everything. If v9 bounds it, first sync after login
   shows fewer entries than users expect, and 1.1 stops being optional.
9. Confirm the observed 429 threshold and whether any `Retry-After`-equivalent
   header is sent — sizes the token bucket in 1.8.

   *W2-D (2026-08-01) sized the client-side pacer on an unverified assumption.*
   `kMinRequestIntervalMillis` is **1000 ms**, taken directly from the documented
   "~1 req/s per token+IP" figure with **no safety margin** — the client runs at
   100% of the stated budget, so clock jitter or reordering can still produce a
   429. If live testing shows 429s continuing under normal sync load, raise it to
   1100-1200 ms. If the real limit is higher than 1/s, lowering it makes large
   pushes much faster. Also unverified: whether v9 sends **any**
   `Retry-After`-equivalent header. The client parses `Retry-After` as integer
   seconds (capped at 300s) and falls back to incremental backoff when absent —
   confirm the header name and format, since a differently-named header (e.g.
   `X-RateLimit-Reset`) would be silently ignored.
10. `desktop.track.toggl.com/stream` still accepts the websocket upgrade
    (`src/websocket_client.cc:125`). Failure is silent and permanent: it retries
    every 45s forever and degrades to ~15-30 minute polling with no user-visible
    error.
11. **Confirm v9 uses 422 for validation rejections, and capture a real 422 body.**
    W2-D added explicit 422 handling on the assumption that v9 returns 422 (not
    400) for validation failures; the spec was not re-fetchable from the
    container. If v9 actually uses 400, the `kUnprocessableEntityError` path is
    dead code and existing 400 handling already covers it. If it uses both, the
    two must behave identically at the push call site (`context.cc` — see W2-E).

`src/test/online_test.cc` is an existing live end-to-end suite (23 tests; signs up
a throwaway user, creates entries and projects) that nothing currently runs. It is
the cheapest way to cover most of the above. It has **no production guard** —
verified: no `TOGGL_PRODUCTION_BUILD` check and no environment pinning anywhere in
`online_test.cc` or `online_test_app.cpp`. Do not build it with
`TOGGL_PRODUCTION_BUILD=ON`.

---

## 9. Explicitly not worth doing

- **Upgrading vendored Poco 1.9 / OpenSSL 1.1.0.** System libraries already build
  and pass cleanly; `USE_BUNDLED_LIBRARIES=OFF` is the default. Multi-day effort
  for no payoff. Note `third_party/CMakeLists.txt:30-31` already downgrades the
  vendored Poco to C++11 because it uses features removed in C++17.
- **The root `Makefile`.** A dead macOS-only build path superseded by CMake.
  Delete it in a separate cleanup rather than maintaining it.
- **Removing the dual-shape read path.** It looks like migration debt but it is
  load-bearing for existing local databases. See "Verified healthy".
- **Deleting the 31 `analytics_.Track*` call sites.** Gut the implementation
  instead — see 2b.1.

---

# 10. Development approach — agent-delegable workstreams

## 10.1 The rule that makes parallelism safe

`src/context.cc` is ~7000 lines and is touched by most of this work. It is the
only real merge-conflict risk in the repo.

> **Treat `src/context.cc` as a mutex: at most one in-flight workstream may own it
> at a time.** Every wave below is built around that constraint.

Secondary shared files, same rule per wave: `src/https_client.cc`,
`src/model/user.cc`, `src/test/app_test.cc`.

## 10.2 Definition of done — applies to every workstream

An agent's slice is not complete until all of these hold:

1. `cmake --build build --target TogglDesktopLibrary TogglAppTest -j"$(nproc)"` succeeds.
2. `cd build && ./src/test/TogglAppTest` reports **≥71 passed, 0 failed**. The
   count only goes up.
3. No new compiler warnings introduced.
4. `grep -rn "api/v8\|kAPIV8" src/` returns nothing.
5. Only files inside the slice's declared ownership were modified (`git diff --name-only`).
6. One commit per slice, message prefixed with the slice ID (e.g. `W1-A: …`).
7. Anything the agent could not verify without a live token is written into
   section 8 of this file rather than guessed at.

## 10.3 Wave plan

```
W0  Foundation ─────────────────────────────────► (blocks everything)
     │
     ├─ W1-A Timeline data path        ┐
     ├─ W1-B Account/prefs endpoints   ├─ parallel, disjoint files
     └─ W1-C Model URLs & payloads     ┘
          │
          ├─ W2-D Networking layer     ┐
          └─ W2-E Dead integrations    ┴─ parallel
               │
               ├─ W3-F Mock seam & push tests  ┐
               └─ W3-G Error strings & docs    ┴─ parallel
                    │
                    └─ W4 Live verification (maintainer + 1 agent)
```

---

### W0 — Test & CI foundation · 1 agent · ~1 day · **blocks all**

Nothing else should start until CI is green, because W0 defines the gate every
later slice is measured against.

**Covers:** 2.1, 2.2, 2.3
**Owns:** `.github/workflows/`, `testdata/me_v9.json`, `src/test/app_test.cc`,
`src/test/toggl_api_test.cc`, `CMakeLists.txt` (Qt guard only)
**Must not touch:** anything under `src/model/`, `src/context.cc`, `src/https_client.*`

Deliverables:
- Linux CI job running the verified recipe in 2.1, on push and PR.
- `testdata/me_v9.json` covering every fork in the 2.2 table.
- The v8/v9 parity test from 2.2.
- `toggl_set_environment(ctx_, STR("test"))` in the `testing::App` fixture, plus a
  check that no test issues live requests.

**Why first:** it is pure additive infrastructure — it cannot break the app, and
every later agent needs `TogglAppTest` as its acceptance signal.

---

### W1 — Correctness · 3 agents in parallel · ~1 day each

Three disjoint file sets. `context.cc` is held by W1-B alone.

#### W1-A · Timeline data path
**Covers:** 0.1, 1.3
**Owns:** `src/model/timeline_event.{cc,h}`, `src/model/user.cc`,
`src/timeline_uploader.{cc,h}`, timeline tests in `src/test/app_test.cc`
**Key risk:** the payload revert is a *revert* — the correct shape is already in
the repo in the `apiVersion <= 8` branch. Do not re-derive it.

#### W1-B · Account & preferences endpoints
**Covers:** 0.2, 0.3, 0.5
**Owns:** `src/context.cc` (exclusively for this wave)
**Key risk:** 0.2 changes both the URL *and* the HTTP verb (`Put` → `Post`). The
existing `kRecordTimelineEnabledJSON`/`DisabledJSON` body constants may need
reshaping for the preferences schema — check against the read path at
`src/context.cc:6303` which already parses that endpoint's response.

#### W1-C · Model URLs & payload shapes
**Covers:** 1.7, 1.9 (investigate + document only), 0.4's `Client::wid` note
**Owns:** `src/model/{tag,task,time_entry,client,project}.cc` and their headers
**Key risk:** `Tag`/`Task` `ModelURL()` need a workspace ID that the current
signature does not carry — check `Client::ModelURL()` (`src/model/client.cc:16`)
for the established pattern. **Do not change the running-entry encoding (1.9)
without live verification** — document the finding and stop.

---

### W2 — Resilience · 2 agents in parallel · ~1 day each

#### W2-D · Networking layer
**Covers:** 1.2, 1.4, 1.8, and the request-body half of 2.5
**Owns:** `src/https_client.{cc,h}`, `src/error.cc`, `src/const.h`
**Key risk:** 1.2's fix is call-site routing through the existing
`silentGet`/`silentPost`, but those call sites are in `context.cc` — which W2-E
owns this wave. Add the mechanism here; hand the call-site list to W2-E, or defer
the routing to W3.
**Watch:** changing the 429 mapping (1.4) affects `IsNetworkingError`, which gates
`trigger_sync_`. Confirm `Sync.LegacyFormat` and `Sync.BatchedFormat` still pass.

#### W2-E · Dead third-party integrations
**Covers:** 2b.1, 2b.2, 2b.3, and the `std::cerr` half of 2.5
**Owns:** `src/analytics.{cc,h}`, `src/context.cc`, `.travis.yml`
**Key risk:** keep the `Analytics` public interface intact so the 31 call sites in
`context.cc` compile untouched — see 2b.1. 2b.2 needs a product decision (disable
vs repoint); if unanswered, implement "disable, fail silent" and flag it.

---

### W3 — Verification depth · 2 agents in parallel · ~1 day each

#### W3-F · Mock seam & push-path tests
**Covers:** 2.4, and 1.5 *if* Phase 3 item 5 has been answered
**Owns:** `src/https_client.h` (test hook), `src/test/`, `src/context.cc`
**Key risk:** the seam must not change production behaviour. Gate it so a release
build has no test hook active.

#### W3-G · Error-string resilience & docs
**Covers:** 1.6 (the fallback half — the string half waits on Phase 3), 0.4
**Owns:** `src/model/time_entry.cc`, `src/error.cc`, `docs/lib/api.md`
**Key risk:** do not guess at v9 error strings. Build the retry cap and the
warning log for unmatched bodies; leave the string table alone until item 6 of
Phase 3 lands.

---

### W4 — Live verification · maintainer-gated

Section 8 in full. One agent can prepare the ground — wire `TogglOnlineTest` into
a manually-triggered CI job with a token from repository secrets, and add the
production guard it currently lacks — but the runs and the judgement calls need a
maintainer with a real account.

**This wave gates release.** 1.5, 1.6 and 1.9 cannot be closed without it.

---

## 10.4 What runs sequentially and why

Sequencing is driven by file ownership, not by technical dependency — most of
these slices are logically independent. The waves exist so that parallel agents
never contend for `context.cc`, `https_client.cc`, `user.cc` or `app_test.cc`.

If you would rather run everything with a single agent, the order is:
**W0 → W1-A → W1-B → W1-C → W2-D → W2-E → W3-F → W3-G → W4.** No wave boundary
carries a hidden dependency; the only hard gate is W0 before everything else.

## 10.5 Briefing template for an agent

Give each agent this, filled in from the slice above:

```
You are implementing slice <ID> of the Toggl API v9 migration.

Read plan.md in the repo root first — it is the source of truth.
Your slice is section <ID> of plan.md §10.3. Implement exactly the items
listed under "Covers"; do not do work belonging to other slices.

File ownership: you may modify ONLY <owned paths>. If you believe a change
outside that set is required, stop and report it instead of making it.

Definition of done: plan.md §10.2, all seven points.

Build/test:
  cmake -S . -B build -DTOGGL_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
  cmake --build build --target TogglDesktopLibrary TogglAppTest -j"$(nproc)"
  cd build && ./src/test/TogglAppTest       # must report >=71 passed, 0 failed

Anything you cannot verify without a live Toggl API token: do not guess.
Append it to plan.md §8 with what you would test and why it matters.

Commit as "<ID>: <summary>" on branch claude/api-v8-v9-upgrade-436x05.
```

## 10.6 Effort summary

| Wave | Slices | Parallel? | Elapsed (parallel) | Total agent-days |
| --- | --- | --- | --- | --- |
| W0 | 1 | — | 1d | 1.0 |
| W1 | A, B, C | yes (3) | 1d | 2.5 |
| W2 | D, E | yes (2) | 1d | 2.0 |
| W3 | F, G | yes (2) | 1d | 1.5 |
| W4 | live | maintainer | — | 0.5 + maintainer time |
| | | | **~4 days** | **~7.5 agent-days** |

Fully sequential, the same work is ~7-8 days elapsed.

---

## Appendix: spec-verified endpoint status

| Endpoint | Status |
| --- | --- |
| `GET /api/v9/me` | Correct. Only param is `with_related_data`; returns a flat user object; related collections are clients, projects, tags, tasks, time_entries, workspaces. No `since`/`server_time` in response. |
| `PUT /api/v9/me` | Exists, but **cannot** set `record_timeline`. |
| `GET/POST /api/v9/me/preferences/{client}` | Correct; `client` is `desktop` or `web`. Carries `record_timeline`. |
| `GET /api/v9/me/time_entries` | Correct. Supports `since`, `before`, `start_date`, `end_date`. Already used correctly at `context.cc:5306`. |
| `GET /api/v9/me/workspaces` | Correct. |
| `POST /api/v9/me/accept_tos` | Correct. |
| `GET/POST /api/v9/workspaces/{id}/preferences` | Correct. |
| `POST/PUT/DELETE /api/v9/workspaces/{id}/time_entries[/{id}]` | Correct. `created_with` and `workspace_id` required in body — both present. Running entry duration should be negative; `-1` is recommended, not mandatory (see 1.9). |
| `POST/PUT/DELETE /api/v9/workspaces/{id}/projects[/{id}]` | Correct. |
| `POST/PUT/DELETE /api/v9/workspaces/{id}/clients[/{id}]` | Correct. Client workspace field is still `wid`. |
| `/api/v9/tags`, `/api/v9/tasks` | **Wrong** — must be workspace-scoped. |
| `POST /api/v9/timeline` | Path correct; payload is the v8 shape (see 0.1); host unverified. |
| `POST /api/v9/feedback`, `/api/v9/feedback/web` | Both exist, both multipart. Prefer `/feedback`. |
| `POST /api/v9/desktop_login_tokens`, `GET /api/v9/desktop_login` | Correct. |
| `GET /api/v9/status`, `POST /api/v9/signup`, `GET /api/v9/countries` | Correct. |
| `POST /api/v9/me/enable_sso` | Correct. |
| `GET /api/v9/auth/saml2/login` | Correct as **GET** with query params — which is what the code already does. |
| Reports API | v2 is gone; only `/reports/api/v3/...` exists. Not used by this app. |
| `sync.toggl.com` `GET /pull`, `POST /push/{uuid}` | Outside `/api/v9`. Liveness unknown — see section 7. |
| `ssl.google-analytics.com/collect?v=1&tid=UA-…` | **Dead** — Universal Analytics shut down 2023-07-01. See 2b.1. |
| `toggl.github.io/toggldesktop/assets/updates-link.txt` | Archived upstream. See 2b.2. |
| `raw.githubusercontent.com/toggl-open-source/toggldesktop/…` | Archived upstream. See 2b.2. |
