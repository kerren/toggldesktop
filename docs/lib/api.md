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
| `GET /api/v9/me?with_related_data=true&since=...` | `Context::me` – login and full sync |
| `GET /api/v9/me/time_entries?since=...` | `Context::pullMoreTimeEntries` |
| `GET /api/v9/me/workspaces` | `Context::pullWorkspaces` |
| `GET/POST /api/v9/me/preferences/desktop` | desktop preferences |
| `PUT /api/v9/me` | `record_timeline` on/off |
| `POST /api/v9/me/accept_tos` | ToS acceptance |
| `POST /api/v9/me/enable_sso`, `POST /api/v9/auth/saml2/login` | SSO |
| `GET /api/v9/workspaces/{id}/preferences` | workspace preferences |
| `POST /api/v9/workspaces/{id}/time_entries`, `.../projects`, `.../clients`, `/api/v9/tags`, `/api/v9/tasks` | model syncing, see each model's `ModelURL()` |
| `POST /api/v9/desktop_login_tokens` + `GET {Main}/api/v9/desktop_login?login_token=...` | opening reports in the browser |
| `POST /api/v9/signup`, `GET /api/v9/countries` | sign up |
| `GET /api/v9/status` | server status checks |
| `POST /api/v9/feedback/web` | feedback form (multipart) |
| `POST /api/v9/timeline` | timeline upload, see below |

## Timeline upload

`POST /api/v9/timeline` is **not** part of the documented public API; it is the
endpoint the desktop clients use to sync recorded background activity. The
payload is a JSON array of the events collected by `WindowChangeRecorder`,
built by `convertTimelineToJSON` (`src/timeline_uploader.cc`) from
`TimelineEvent::SaveToJSON` (`src/model/timeline_event.cc`):

```json
[
  {
    "guid": "<event guid>",
    "desktop_id": "<desktop id>",
    "app_name": "Google Chrome",
    "window_title": "Wireshark Packet Analysis - Stack Overflow",
    "start": "2026-07-28T16:04:05Z",
    "end": "2026-07-28T16:15:30Z",
    "created_with": "timeline"
  }
]
```

Notes:

* `app_name` is the executable/application name (`TimelineEvent::Filename`)
  and `window_title` is the focused window title (`TimelineEvent::Title`).
  In v8 these were called `filename` and `title`.
* `start` and `end` are ISO 8601 timestamps in UTC
  (`2006-01-02T15:04:05Z`), produced by `Formatter::Format8601`. In v8 they
  were Unix timestamps called `start_time` and `end_time`.
* Events are compressed into `kTimelineChunkSeconds` long chunks first and
  only chunked events are uploaded; see `User::CompressTimeline` and
  `Context::CreateCompressedTimelineBatchForUpload`.
* The v8 payload can still be produced by passing `8` as the `apiVersion`
  argument of `convertTimelineToJSON`, which is what the regression tests use
  to pin the old shape.

Because the endpoint is undocumented, its payload can change without notice.
`TimelineUploader` backs off exponentially (up to
`kTimelineUploadMaxBackoffSeconds`) whenever the upload fails, and the events
are only marked as uploaded after a successful response, so a shape change
shows up as repeated failures in the `timeline_uploader` log rather than as
data loss.
