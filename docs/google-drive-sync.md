# Google Drive Book Sync

This document explains the Google Drive sync feature: a menu action that pulls books from a
Google Drive folder onto the device over WiFi, skipping anything already present. It covers
the architecture, the service-account auth (JWT-bearer), the on-disk formats, and how the
pieces fit together — enough for a contributor (or an AI agent) to understand and safely
modify the code without reading every line.

## Overview

**Google Drive Sync** (from Settings → System, or Home → File Transfer) runs a one-shot sync:

1. First run only: the device writes an editable `/.crosspoint/gdrive.json` template; the user
   pastes a Google **service-account JSON** (client_email + private_key) and the **Folder ID**
   in a text editor on a PC. On the next run the device obfuscates the private key in place.
2. Connect to WiFi.
3. **Authenticate** without any phone/consent step: sync the clock via NTP, build a JWT, sign it
   RS256 with the service-account key, and exchange it for an access token (OAuth2 JWT-bearer
   grant). See [Service-account authentication](#service-account-authentication-jwt-bearer).
4. List the folder and **download new/changed books**, skipping files already on the card.

The device **only reads** from Drive (scope `drive.readonly`) and **only downloads** —
it never writes, deletes, or uploads. Books are pulled **as-is**; the device does not
optimize/transcode them (see [Image optimization is out of scope](#image-optimization-is-out-of-scope)).

> **Why a service account?** Google's OAuth **device-code (QR) flow forbids `drive.readonly`**
> (`invalid_scope`), and the allowed `drive.file` scope can't list a user-populated folder — so
> the QR approach is a dead end for this use case. A service account sidesteps OAuth consent
> entirely, and because a service account only sees what's **shared with it**, sharing just the
> books folder scopes the device to exactly that folder.

> **Setting it up?** End-user steps (create the service account + key, share the folder, find
> the Folder ID) are in [google-drive-sync-setup.md](google-drive-sync-setup.md). This document
> is the developer/architecture reference.

## Components

| File | Role |
|------|------|
| `src/network/GoogleJwtAuth.{h,cpp}` | Mints access tokens from the service-account key (RS256 JWT-bearer, mbedTLS) |
| `src/network/GoogleDriveClient.{h,cpp}` | Stateless Drive v3 list/download (network + JSON only, no UI/SD-listing) |
| `src/GoogleDriveStore.{h,cpp}` | Persistent config + dedup manifest (`/.crosspoint/gdrive.json`) |
| `src/activities/network/GoogleDriveSyncActivity.{h,cpp}` | The UI/state-machine that drives the flow |
| `src/network/HttpDownloader.{h,cpp}` | Shared HTTP client; extended here with `postForm()` + Bearer-token auth |
| `src/activities/settings/SettingsActivity.{h,cpp}` | Settings entry point (`SettingAction::GoogleDriveSync`) |
| `src/activities/network/NetworkModeSelectionActivity.{h,cpp}` | "File Transfer" entry point (`NetworkMode::GOOGLE_DRIVE_SYNC`) |
| `src/activities/network/CrossPointWebServerActivity.cpp` | Launches the sync activity when that option is picked |
| `lib/I18n/translations/english.yaml` | `STR_GDRIVE_*` / `STR_GOOGLE_DRIVE_SYNC` UI strings |

**Two entry points:** *Settings → System → Google Drive Sync*, and *Home → File Transfer →
Google Drive Sync* (the latter is the quicker path). Both launch the same
`GoogleDriveSyncActivity`.

### Diagnostics

Failures are surfaced three ways: an on-screen message (word-wrapped), the serial log
(`GDRIVE`/`GJWT` tags), and an appended line in **`/.crosspoint/gdrive_log.txt`** on the SD card
(kept under 32 KB). Both `GoogleJwtAuth::lastError()` (auth) and `GoogleDriveClient::lastError()`
(listing/download) hold a detailed reason — failing step + HTTP status + Google's JSON
`error`/`error_description` (e.g. `invalid_grant`) — which the activity shows instead of the
generic "Authentication failed" so the cause is diagnosable without serial.

These mirror existing patterns: the activity is modelled on `FontDownloadActivity`
(synchronous downloads with an input-pumping progress callback), the store mirrors
`WifiCredentialStore`/`OpdsServerStore` (obfuscated JSON), and TLS reuses the same
`HttpDownloader` + `esp_crt_bundle_attach` path as `OtaUpdater`.

## State machine

`GoogleDriveSyncActivity` (see the `State` enum in the header) progresses:

```text
onEnter ─► checkConfigAndStart() ─► GoogleDriveStore::loadConfig()
  │
  ├─ NoFile      ─► writeConfigTemplate() ─► CONFIG_NEEDED ("created, edit on PC")
  ├─ Incomplete  ─────────────────────────► CONFIG_NEEDED ("incomplete, edit & rerun")
  ├─ Invalid     ─────────────────────────► ERROR ("config invalid", file left intact)
  └─ Ready ───────────────────────────────► WIFI_SELECTION
                                                  │ (connected)
                                                  ▼
                                          authenticateAndSync()
                                          ┌───────┴────────────────┐
                                  AUTHENTICATING            (NTP sync the clock,
                                          │                 GoogleJwtAuth::getAccessToken)
                                  ┌───────┴────────┐
                               ok │            failed │
                                  ▼                 ▼
                              runSync()           ERROR
                                  │
                                  ▼
                          LISTING ─► SYNCING ─► COMPLETE
                                 (any failure) ─► ERROR
```

- **CONFIG_NEEDED** is a static instruction screen (config file missing or incomplete). The
  user edits `/.crosspoint/gdrive.json` on a PC and re-opens the menu item to re-read it;
  **Back** exits. The template is written only when the file is absent, so an edited file is
  never clobbered.
- **AUTHENTICATING** is non-interactive: it NTP-syncs the clock then mints a token (blocking,
  a few seconds). No phone/code step. Failure routes to ERROR with the detailed reason.
- **LISTING/SYNCING** are synchronous — the blocking network calls run on the main loop, and
  the download progress callback pumps `mappedInput.update()` + `requestUpdate()` so the
  screen stays live and **Back** cancels mid-download (same approach as `FontDownloadActivity`).
- **onExit** calls `silentRestart()` when WiFi was brought up, to reclaim heap fragmentation
  from the TLS/WiFi session (consistent with the other network activities).

## Service-account authentication (JWT-bearer)

Implemented in `src/network/GoogleJwtAuth.{h,cpp}` using the mbedTLS C API (already linked for
TLS; SHA-256/base64 are also used by `FirmwareFlasher.cpp` / `ObfuscationUtils.cpp`).

`GoogleJwtAuth::getAccessToken(clientEmail, privateKeyPem, tokenUri, scope, outToken)`:

1. **Clock guard:** reject if `time(nullptr)` is implausibly small (clock not NTP-synced) —
   otherwise Google rejects the JWT as `invalid_grant`. The activity NTP-syncs first.
2. **Build & sign the JWT:** header `{"alg":"RS256","typ":"JWT"}` and claims
   `{iss=clientEmail, scope=drive.readonly, aud=tokenUri, iat=now, exp=now+3600}`. base64url
   both, join with `.`, SHA-256 the result (`mbedtls_sha256`), then sign with
   `mbedtls_pk_sign(MBEDTLS_MD_SHA256)` after `mbedtls_pk_parse_key` on the PEM (RNG seeded via
   entropy + ctr_drbg). The signature is base64url-appended → `header.claims.signature`.
3. **Exchange:** `POST tokenUri` with
   `grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=<jwt>` via
   `HttpDownloader::postForm`; read `access_token` from the JSON.

base64url = `mbedtls_base64_encode` then `+`→`-`, `/`→`_`, strip `=`. `HttpDownloader::postForm()`
returns the body **and HTTP status for any status code** (`requireOk = false`), so a non-200
token error (`invalid_grant` etc.) is surfaced with Google's `error_description`.

### Security note: access is folder-scoped

Unlike the rejected OAuth approaches, a **service account only sees what is explicitly shared
with it** — it has its own empty Drive. The user shares just the books folder (Viewer), so the
key on the SD card can read **only** that folder, never the user's wider Drive. The private key
is obfuscated on disk (device-tied); if the card is lost, the user revokes the key in the Cloud
console. This is the mitigation, enforced by Google's sharing model rather than by convention.

## Drive listing & download

- `listFolder(folderId, accessToken, out)` → `GET .../drive/v3/files` with
  `q='<folderId>' in parents and trashed=false` and
  `fields=nextPageToken,files(id,name,mimeType,md5Checksum,size)`, paged via `nextPageToken`
  (capped at `MAX_LIST_PAGES` so a malformed token can't loop forever). Each page is
  downloaded to a temp file first (so the TLS buffers and the full JSON aren't held at once),
  then parsed with an **ArduinoJson filter** so only the five used fields are materialized.
  Only supported book extensions are kept (`hasEpubExtension` / `hasTxtExtension` /
  `hasXtcExtension` from `lib/FsHelpers`).
- `DriveFile` uses fixed `char[]` members (`id`, `name`, `md5`) + `size_t size` — no
  `std::string` in the listing hot path.
- `downloadFile(fileId, accessToken, destPath, progressCb, cancelFlag)` →
  `GET .../files/<id>?alt=media` with a Bearer header, streamed straight to SD through
  `HttpDownloader::downloadToFile` (2 KB chunks — no full-file buffer).

## Dedup (skip already-synced books)

`GoogleDriveStore` keeps a manifest mapping `fileId → md5`. During sync, a file is **skipped**
when **all** of:

- its `fileId` is in the manifest, **and**
- the stored md5 equals Drive's current `md5Checksum` (catches files replaced/updated in
  Drive — those re-download), **and**
- the local destination file still exists via `Storage.exists()` (catches books manually
  deleted on the device — those re-download).

Otherwise the file is downloaded and the manifest entry is recorded. After a successful
download, `clearBookCache(destPath)` (`src/util/BookCacheUtils.h`) invalidates any cached
layout so the home/file browser re-parses the new book. The manifest is persisted with
`GoogleDriveStore::saveToFile()` after the run (and on a mid-run download failure, to keep the
records earned so far).

## On-disk format & config lifecycle: `/.crosspoint/gdrive.json`

Configuration is **file-based** — the user pastes Google's service-account JSON on a PC.
`GoogleDriveStore` has three relevant entry points:

- `writeConfigTemplate()` — writes an editable, pretty-printed template when no file exists:

  ```jsonc
  {
    "_instructions": "Paste your Google service-account JSON … (folder-share note)",
    "folderId": "", "syncFolder": "/",
    "serviceAccount": { "client_email": "", "private_key": "", "token_uri": "https://oauth2.googleapis.com/token" }
  }
  ```

  `serviceAccount` mirrors Google's key-file shape, so the user can copy fields straight across.
  `private_key` here is **plaintext** PEM (with `\n` escapes that ArduinoJson turns into real
  newlines).

- `loadConfig()` → `ConfigStatus { NoFile, Invalid, Incomplete, Ready }`. It reads the obfuscated
  `privateKey_obf` if present; otherwise it falls back to the plaintext
  `serviceAccount.private_key` (the just-pasted key) and flags a re-save. Returns `Incomplete` if
  any of client_email / private_key / folderId is blank, `Invalid` on a JSON parse error (the file
  is **not** overwritten in that case), else `Ready`.

- `saveToFile()` — writes the configured form, **never** the plaintext key:

  ```jsonc
  {
    "_instructions":  "Configured. The private key is obfuscated …",
    "folderId":       "1Ab…",                              // plaintext
    "syncFolder":     "/",                                 // SD destination, plaintext
    "serviceAccount": { "client_email": "…@….iam.gserviceaccount.com", "token_uri": "…" },
    "privateKey_obf": "…",                                 // obfuscated PEM
    "manifest": [ { "id": "<fileId>", "md5": "<32 hex>" }, … ]
  }
  ```

The **plaintext→obfuscated migration** is the key behaviour: the first `loadConfig()` after the
user pastes the key sees plaintext `serviceAccount.private_key`, returns `Ready`, and immediately
calls `saveToFile()` — which writes `privateKey_obf` and drops the plaintext key. Using distinct
keys (`serviceAccount.private_key` vs `privateKey_obf`) makes "is it obfuscated yet?" unambiguous.
Obfuscation reuses `obfuscation::obfuscateToBase64`/`deobfuscateFromBase64`
(`lib/Serialization/ObfuscationUtils.h`) — XOR with the device MAC then base64: not cryptographic,
but ties the key to the device and prevents casual reading off the card.

## Image optimization is out of scope

The device pulls books **verbatim**. The EPUB image optimizer the project ships is
**browser-side JavaScript** in `src/network/html/FilesPage.html` (`convertEpubFile()` →
JSZip + `<canvas>` re-encode → re-zip); the firmware has no JPEG **encoder** and no
deflate/re-zip path, and not enough RAM (≈380 KB, no PSRAM) to add them. So whatever lands in
the synced Drive folder should already be optimized.

The intended companion is a small **Cloud Run service** that runs that same `FilesPage.html`
headlessly to optimize books server-side before they reach the synced folder — see the
project plan. That service is **not** part of the firmware and lives outside this repo's
device code.

## Memory & resource notes

- TLS/WiFi reuses the proven `OtaUpdater` path (`HttpDownloader` + `esp_crt_bundle_attach`).
- No full-file buffering: listings and downloads stream to SD; listing JSON is field-filtered.
- Form bodies/URLs are built with `std::string` concatenation in the (non-hot) network layer;
  the per-file listing path avoids `std::string` via `DriveFile`'s fixed buffers.
- `silentRestart()` on exit reclaims WiFi/TLS heap fragmentation.

## Testing checklist (on device)

1. Create a service account + JSON key, enable the Drive API, and **share the books folder with
   the service account's `client_email`** (Viewer).
2. Open Google Drive Sync once → edit `/.crosspoint/gdrive.json` on a PC: paste the JSON under
   `serviceAccount` and set `folderId`. Re-open to obfuscate the key in place.
3. Connect WiFi; watch serial (`LOG_LEVEL=2`, tags `GDRIVE`/`GJWT`) for the NTP sync, token
   mint, and list/download/skip logs. Confirm no phone step is needed.
4. Confirm books appear in the file browser and open/render.
5. Re-run sync → already-present files are **skipped** (md5 match).
6. Replace a file in Drive → next sync re-downloads only that file.
7. Failure paths: wrong/garbled key → `Authenticate: invalid private_key`; unshared folder →
   `List folder: request failed`; both also land in `/.crosspoint/gdrive_log.txt`.
8. Heap stays healthy across repeated syncs (`ESP.getFreeHeap()`), no leak.
