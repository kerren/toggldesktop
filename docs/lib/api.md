# Toggl API usage in the library

All backend calls made by the library go through `HTTPClient`/`TogglClient`
(`src/https_client.cc`). The hosts they are sent to are defined in
`src/urls.cc`.

## Hosts

| Helper | Production | Staging |
| --- | --- | --- |
| `urls::Main()` | `https://track.toggl.com` | `https://track.toggl.space` |
| `urls::API()` / `urls::TrackAPI()` | `https://api.track.toggl.com` | `https://api.track.toggl.space` |
| `urls::TimelineUpload()` | `https://api.track.toggl.com` | `https://api.track.toggl.space` |
| `urls::SyncAPI()` | `https://sync.toggl.com/` | `https://sync.toggl.space/` |
| `urls::WebSocket()` | `https://desktop.track.toggl.com` | `https://desktop.track.toggl.space` |

API v8 was deprecated on 2022-09-26, the `/api/v8/*` endpoints were disabled
on 2023-04-01 and the version was shut down completely on 2024-05-23, so every
call the library makes uses `/api/v9`. The desktop-specific host
(`desktop.track.toggl.*`) is only kept for the `/stream` websocket, which is
not part of the REST API.

Authentication is HTTP basic auth with either the API token or the email as
the user name; when the API token is used, the password is the literal string
`api_token`.

## Endpoints used

| Endpoint | Used by |
| --- | --- |
| `GET /api/v9/me?with_related_data=true` | `Context::me` – login and full sync |
| `GET /api/v9/me/time_entries?since=...` | `Context::pullMoreTimeEntries` |
| `GET /api/v9/me/workspaces` | `Context::pullWorkspaces` |
| `GET/POST /api/v9/me/preferences/desktop` | desktop preferences, including `record_timeline` on/off |
| `POST /api/v9/me/accept_tos` | ToS acceptance |
| `POST /api/v9/me/enable_sso`, `GET /api/v9/auth/saml2/login` | SSO |
| `GET /api/v9/workspaces/{id}/preferences` | workspace preferences |
| `POST /api/v9/workspaces/{id}/time_entries`, `.../projects`, `.../clients`, `.../tags`, `.../tasks` | model syncing, see each model's `ModelURL()` |
| `POST /api/v9/desktop_login_tokens` + `GET {Main}/api/v9/desktop_login?login_token=...` | opening reports in the browser |
| `POST /api/v9/signup`, `GET /api/v9/countries` | sign up |
| `GET /api/v9/status` | server status checks |
| `POST /api/v9/feedback` | feedback form (multipart) |
| `POST /api/v9/timeline` | timeline upload, see below |

`GET /me` accepts **only** `with_related_data`. It does not accept `app_name` or
`since`, and its response carries no top-level `since`/`server_time`, so there is
no incremental-sync cursor on this endpoint — every call is a full pull. The
per-collection `?since=` form used by `/me/time_entries` is the supported
alternative.

`record_timeline` is **not** settable through `PUT /api/v9/me`: `me.payload`
accepts only `beginning_of_week`, `country_id`, `current_password`,
`default_workspace_id`, `email`, `fullname`, `password` and `timezone`. It is a
property of `models.AllPreferences` and is written with a flat
`{"record_timeline": <bool>}` body to `POST /api/v9/me/preferences/desktop` —
the same resource the client reads it back from. There is no `/timeline_settings`
path in v9.

All tag and task URLs are workspace-scoped. Bare `/api/v9/tags` and
`/api/v9/tasks` do **not** exist.

## Timeline upload

`POST /api/v9/timeline` is **not** part of the documented public API; it is the
endpoint the desktop clients use to sync recorded background activity. The
payload is a JSON array of the events collected by `WindowChangeRecorder`,
built by `convertTimelineToJSON` (`src/timeline_uploader.cc`) from
`TimelineEvent::SaveToJSON` (`src/model/timeline_event.cc`):

```json
[
  {
    "desktop_id": "<desktop id>",
    "filename": "Google Chrome",
    "title": "Wireshark Packet Analysis - Stack Overflow",
    "start_time": 1769616245,
    "end_time": 1769616930,
    "idle": false
  }
]
```

**Only the endpoint moved from v8 to v9 — the payload shape did not.** The
schema is `models.TimelineEvent`: `desktop_id` (string), `start_time` (int),
`end_time` (int), `filename` (string), `title` (string), `idle` (bool) and `id`
(int). An earlier pass at this migration renamed these to
`app_name`/`window_title` with ISO 8601 `start`/`end`; that was wrong and has
been reverted.

Notes:

* `filename` is the executable/application name (`TimelineEvent::Filename`) and
  `title` is the focused window title (`TimelineEvent::Title`).
* `start_time` and `end_time` are Unix timestamps, **not** ISO 8601 strings.
* `idle` is a real schema field that the client historically never sent; it is
  serialised now.
* `guid` and `created_with` are **not** part of the schema and must not be sent.
* `desktop_id` is attached per event by `convertTimelineToJSON`, not by
  `TimelineEvent::SaveToJSON`. `id` is server-assigned and is not sent on create.
* There is no `apiVersion` branch any more. v8 is gone, so there is nothing to
  fall back to and the version argument was removed rather than left dead.
* Events are compressed into `kTimelineChunkSeconds` long chunks first and only
  chunked events are uploaded; see `User::CompressTimeline` and
  `Context::CreateCompressedTimelineBatchForUpload`.

Because the endpoint is undocumented, its payload can change without notice.
`TimelineUploader` backs off exponentially (up to
`kTimelineUploadMaxBackoffSeconds`) whenever the upload fails, and events are
only marked uploaded after a successful response.

Retention is what decides whether a rejected payload costs you data.
`User::CompressTimeline` deletes events older than `kTimelineSecondsToKeep`
(7 days) **only if they were actually uploaded**. Un-uploaded events are kept
past that window up to a hard ceiling of `kTimelineSecondsToKeepUnuploaded`
(90 days), after which they are evicted and the eviction is logged at warning.
The ceiling exists so an account that can never upload — a permanently revoked
token, say — cannot grow the local database without bound.

Before that retention rule existed, deletion was unconditional, so a payload
the server rejected silently bled activity data away every 7 days. Do not
reintroduce an unconditional delete here.

## Things that look like bugs but are not

Two pieces of the model layer read as leftover v8 debt. Both are deliberate,
both are load-bearing, and both will be "fixed" into breakage by anyone who
does not read this section first.

### Clients really do still use `wid`

`Client::SaveToJSON` sends `wid`, not `workspace_id`
(`src/model/client.cc`). Every other model sends `workspace_id`. This is not an
oversight in the migration — v9 genuinely kept the old field name for the client
resource, and the endpoint rejects `workspace_id`. There is an explicit comment
at the call site saying so. Leave it alone.

### The read path is intentionally dual-shape

`LoadFromJSON` in `tag.cc`, `task.cc`, `project.cc`, `client.cc` and
`time_entry.cc` all branch on the v8 key and fall back to the v9 key:

| File | v8 key | v9 key |
| --- | --- | --- |
| `user.cc` | `default_wid` | `default_workspace_id` |
| `time_entry.cc` | `wid` | `workspace_id` |
| `project.cc` | `hex_color`, `wid`, `cid` | `color`, `workspace_id`, `client_id` |
| `task.cc` | `pid`, `wid` | `project_id`, `workspace_id` |
| `client.cc` | `wid` | `workspace_id` |
| `tag.cc` | `wid` | `workspace_id` |

This is **not** migration debt to be cleaned up. The local SQLite cache on an
existing install still holds v8-shaped rows, and those rows are parsed by the
same code that parses server responses. Deleting the v8 branch would silently
break every user upgrading from an older build. `testdata/me.json` (v8 shape)
and `testdata/me_v9.json` (v9 shape) are both loaded by a parity test that
asserts the two produce identical model state; keep both fixtures in step.

## `sync.toggl.com` — treat as live, do not extend

There is a **second, older sync protocol** alongside the REST path:
`Context::pullBatchedUserData` / `pushBatchedChanges` talk to `urls::SyncAPI()`
(`sync.toggl.com`) via `GET /pull` and `POST /push/{uuid}`. It is outside
`/api/v9` entirely, and its payloads (`SyncPayload()`/`SyncMetadata()`) are
hand-built and independent of the models' `SaveToJSON()`.

What is known:

* It is selected by `user_->AlphaFeatureSettings->IsSyncEnabled()`, which
  defaults to **false**, so REST is what runs for every account today.
* That flag is **server-sent**, read from `/me/preferences/desktop`. Toggl can
  move an account onto this path at any time without a client release.
* Its liveness is **unknown**. Whether it survived the v8 shutdown needs a
  backend answer, not engineering time.

So: it must be treated as live and must not be deleted, but it should not be
extended either. If it did die with v8, affected users get broken sync with no
client-side warning. `Sync.LegacyFormat` and `Sync.BatchedFormat` in
`TogglAppTest` cover it offline and must keep passing.

Note that a 5xx from `sync.toggl.com` no longer trips the status gate for
`api.track.toggl.com` — these calls go through the `silent*` client variants
precisely because a failure on one host should not stall sync on another.
