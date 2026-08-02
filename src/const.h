// Copyright 2014 Toggl Desktop developers.

#ifndef SRC_CONST_H_
#define SRC_CONST_H_

// used later in the file
#ifndef TOGGL_BUILD_TYPE
#define TOGGL_BUILD_TYPE ""
#endif

#define kOneSecondInMicros 1000000

#define kMaxTimeEntryDurationSeconds 3596400
#define kHTTPClientTimeoutSeconds 30
#define kSyncIntervalRangeSeconds 900
#define kWebsocketRestartRangeSeconds 45
#define kCheckUpdateIntervalSeconds 86400
#define kCheckInAppMessageIntervalSeconds 14400
#define kRequestThrottleSeconds 2
#define kTimerStartInterval 10
#define kTimelineSecondsToKeep 604800
#define kWindowFocusThresholdSeconds 10
#define kAutotrackerThresholdSeconds 0
#define kBetaChannelPercentage 25
#define kTimelineChunkSeconds 900
#define kEnterpriseInstall false
#define kDebianPackage (TOGGL_BUILD_TYPE == std::string("deb"))
#define kTimelineUploadIntervalSeconds 60
#define kTimelineUploadMaxBackoffSeconds (kTimelineUploadIntervalSeconds * 10)  // NOLINT
#define kMaxFileSize 5242880  // 5MB
#define kMaxDurationSeconds (999 * 3600)
#define kMaxTagsPerTimeEntry 50
#define kMinimumAllowedYear 2006
#define kMaximumAllowedYear 2030
#define kMaximumDescriptionLength 3000
#define kTimeComparisonEpsilonMicroSeconds 100000 // 100 ms

// Client-side request pacing (plan.md 1.8).
//
// The v9 rate limit is documented as roughly one request per second per
// token+IP (leaky bucket). The push path issues one request per dirty model,
// so a sync cycle after offline use would otherwise fire a burst straight into
// that limit. HTTPClient::request paces outbound requests per host to at most
// one every kMinRequestIntervalMillis.
//
// NOTE: the exact server-side threshold is unverified from this container --
// see plan.md section 8 item 9. 1000 ms is the documented "~1 req/s" figure
// with no safety margin; if live testing shows 429s still arriving, raise it.
#define kMinRequestIntervalMillis 1000
// Upper bound on how far into the future a request may be queued by the pacer.
// Bounds the worst-case blocking time of a single request when several threads
// are issuing requests at once. Requests are issued serially per thread, so in
// practice a single wait is <= kMinRequestIntervalMillis.
#define kMaxRequestPacingSeconds 10

// Incremental backoff applied to a host that answered 429 (plan.md 1.4/1.8).
// Replaces the old flat 60 second ban: 5s, 10s, 20s, 40s, 60s, 60s, ...
// The counter resets as soon as the host answers anything other than a 429.
#define kRateLimitBackoffBaseSeconds 5
#define kRateLimitBackoffMaxSeconds 60
// Cap applied to a server-supplied Retry-After value, so a bogus header
// cannot take the client offline indefinitely.
#define kRateLimitRetryAfterMaxSeconds 300

// Outgoing request bodies are logged at debug level (plan.md 2.5). Feedback
// submissions attach the raw log file, so bodies are redacted first and
// truncated to keep the log a reasonable size.
#define kMaxLoggedRequestBodyChars 8192
#define kRedactedValuePlaceholder "<redacted>"

#define kLostPasswordURL "https://toggl.com/forgot-password?desktop=true"
#define kGeneralSupportURL "https://support.toggl.com/toggl-on-my-desktop/"
#define kLinuxSupportURL "https://support.toggl.com/toggl-desktop-for-linux/"
#define kMacSupportURL "https://support.toggl.com/toggl-desktop-for-mac-osx/"
#define kTOSURL "https://toggl.com/legal/terms/"
#define kPrivacyPolicyURL "https://toggl.com/legal/privacy/"

#define kContentTypeMultipartFormData "multipart/form-data"
#define kContentTypeApplicationJSON "application/json"

// Data validation errors
#define kOverMaxDurationError "Max allowed duration per 1 time entry is 999 hours"
#define kMaxTagsPerTimeEntryError "Tags are limited to 50 per task"
#define kInvalidStartTimeError "Start time year must be between 2006 and 2030"
#define kInvalidStopTimeError "Stop time year must be between 2006 and 2030"
#define kInvalidDateError "Date year must be between 2006 and 2030"
#define kStartNotBeforeStopError "Stop time must be after start time"
#define kMaximumDescriptionLengthError "Maximum length for description (3000 chars) exceeded"
#define kForeignEntityLostError "Assigned foreign entity could not be found"

#define kCheckYourSignupError "Signup failed - please check your details. The e-mail might be already taken."  // NOLINT
#define kEndpointGoneError "The API endpoint used by this app is gone. Please contact Toggl support!"  // NOLINT
#define kForbiddenError "Invalid e-mail or password!"
#define kUnsupportedAppError "This version of the app is not supported any more. Please visit Toggl website to download a supported app." // NOLINT
#define kUnauthorizedError "Unauthorized! Please login again."
#define kCannotConnectError "Cannot connect to Toggl"
#define kCannotSyncInTestEnv "Cannot sync in test env"
#define kBackendIsDownError "Backend is down"
#define kBackendIsSendingInvalidData "Backend is sending invalid data"
#define kBadRequestError "Data that you are sending is not valid/acceptable"
// HTTP 422. The server understood the request and rejected the *contents*.
// Deliberately distinct from kBadRequestError (400) so the two can be told
// apart in logs, and deliberately NOT a networking error: resending the same
// payload can never succeed, so it must not be reported as "you are offline"
// nor retried forever. See error.cc IsNetworkingError / IsUserError.
#define kUnprocessableEntityError "Data that you are sending was rejected as invalid by the server"  // NOLINT
#define kRequestIsNotPossible "Request is not possible"
#define kPaymentRequiredError "Requested action allowed only for Non-Free workspaces. Please upgrade!"  // NOLINT
#define kCannotAccessWorkspaceError "cannot access workspace"
#define kEmailNotFoundCannotLogInOffline "Login failed. Are you online?"  // NOLINT
#define kInvalidPassword "Invalid password"
#define kCannotEstablishProxyConnection "Cannot establish proxy connection"
#define kCertificateVerifyFailed "certificate verify failed"
#define kCheckYourProxySetup "Check your proxy setup"
#define kCheckYourFirewall "Check your firewall"
#define kProxyAuthenticationRequired "Proxy Authentication Required"
#define kCertificateValidationError "Certificate validation error"
#define kUnacceptableCertificate "Unacceptable certificate from www.toggl.com"
#define kCannotUpgradeToWebSocketConnection "Cannot upgrade to WebSocket connection"  // NOLINT
#define kSSLException "SSL Exception"
// HTTP 429. This is the error StatusCodeToError returns for a rate-limited
// request, and the one HTTPClient returns while a host is in rate-limit
// backoff. It is deliberately NOT a networking error: the request reached the
// server and got an authoritative answer, so reporting it as "offline" (and
// clearing trigger_sync_ with it) is wrong. The delay is no longer a flat
// minute -- see kRateLimitBackoffBaseSeconds.
#define kRateLimit "Too many requests, sync delayed"
#define kCannotWriteFile "Cannot write file"
#define kIsSuspended "is suspended"
#define kRequestToServerFailedWithStatusCode403 "Request to server failed with status code: 403"  // NOLINT
#define kMissingWorkspaceID "Missing workspace ID"
#define kCannotContinueDeletedTimeEntry "Cannot continue deleted time entry"
#define kCannotDeleteDeletedTimeEntry "Cannot delete deleted time entry"
#define kErrorRuleAlreadyExists "rule already exists"
#define kPleaseSelectAWorkspace "Please select a workspace"
#define kClientNameMustNotBeEmpty "Client name must not be empty"
#define kProjectNameMustNotBeEmpty "Project name must not be empty"
#define kProjectNameAlready "Project name already"
#define kProjectNameAlreadyExists "Project name already exists"
#define kClientNameAlreadyExists "Client name already exists"
#define kDatabaseDiskMalformed "The database disk image is malformed"
#define kMissingWS "You no longer have access to your last workspace"
#define kOutOfDatePleaseUpgrade "Your version of Toggl Track is out of date, please upgrade!"
#define kThisEntryCantBeSavedPleaseAdd "This entry can't be saved - please add"
#define kOneLoginAttemptLeft "Incorrect email or password. One more try before account gets locked for 5 minutes."
#define kAccountIsLocked "Incorrect email or password. Account is locked for 5 minutes. Account owner has been notified."
#define kIncorrectEmailOrPassword "Incorrect email or password. Please try again."
#define kSSONotConfigure "SSO is not configured for this email address"
#define kBetterSSONotConfigure "Single Sign On is not configured for your email address. Please try a different login method or contact your administrator."

#define kModelAutotrackerRule "autotracker_rule"
#define kModelClient "client"
#define kModelProject "project"
#define kModelSettings "settings"
#define kModelTag "tag"
#define kModelTask "task"
#define kModelTimeEntry "time_entry"
#define kModelTimelineEvent "timeline_event"
#define kModelUser "user"
#define kModelWorkspace "workspace"

#define kChangeTypeInsert "insert"
#define kChangeTypeUpdate "update"
#define kChangeTypeDelete "delete"

#define kAutocompleteItemTE  0
#define kAutocompleteItemTask 1
#define kAutocompleteItemProject 2
#define kAutocompleteItemWorkspace 3

#define kTogglDesktopClientID_MacOS "toggldesktop.TogglDesktop"
// API v8 was shut down by Toggl on 2024-05-23, v9 is the only version left
#define kAPIV9 "v9"
#define kGoogleProvider "google"
#define kAppleProvider "apple"
#define kGoogleAccessToken "google_access_token"
#define kAppleAccessToken "apple_token"

// there was a typo in the initial set of flags, use both variants
#define kSyncStrategyLegacy1 "dekstop_sync_client"
#define kSyncStrategyLegacy2 "desktop_sync_client"
#define kTimelineUi "desktop_timeline_ui"

#define kDesktopClient "desktop"

#endif  // SRC_CONST_H_
