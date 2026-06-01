# Google Drive Book Sync

This document explains the Google Drive sync feature: a Settings action that pulls books
from a Google Drive folder onto the device over WiFi, skipping anything already present. It
covers the architecture, the OAuth flow, the on-disk formats, and how the pieces fit
together — enough for a contributor (or an AI agent) to understand and safely modify the
code without reading every line.

## Overview

`Settings → System → Google Drive Sync` runs a one-shot sync:

1. First run only: the device writes an editable `/.crosspoint/gdrive.json` template; the user
   fills in the OAuth **Client ID**, **Client Secret**, and **Folder ID** in a text editor on a
   PC. On the next run the device obfuscates the secret in place (one-time step).
2. Connect to WiFi.
3. Authorize once via the **OAuth 2.0 device flow** (the device shows a short code + URL +
   QR; the user approves on a phone). A refresh token is stored; later syncs reuse it with
   no re-auth.
4. List the folder and **download new/changed books**, skipping files already on the card.

The device **only reads** from Drive (scope `drive.readonly`) and **only downloads** —
it never writes, deletes, or uploads. Books are pulled **as-is**; the device does not
optimize/transcode them (see [Image optimization is out of scope](#image-optimization-is-out-of-scope)).

> **Setting it up?** End-user steps for creating the OAuth credentials and finding the Folder
> ID are in [google-drive-sync-setup.md](google-drive-sync-setup.md). This document is the
> developer/architecture reference.

## Components

| File | Role |
|------|------|
| `src/network/GoogleDriveClient.{h,cpp}` | Stateless Drive v3 + OAuth client (network + JSON only, no UI/SD-listing) |
| `src/GoogleDriveStore.{h,cpp}` | Persistent config + dedup manifest (`/.crosspoint/gdrive.json`) |
| `src/activities/network/GoogleDriveSyncActivity.{h,cpp}` | The UI/state-machine that drives the flow |
| `src/network/HttpDownloader.{h,cpp}` | Shared HTTP client; extended here with `postForm()` + Bearer-token auth |
| `src/activities/settings/SettingsActivity.{h,cpp}` | Menu entry + action dispatch (`SettingAction::GoogleDriveSync`) |
| `lib/I18n/translations/english.yaml` | `STR_GDRIVE_*` / `STR_GOOGLE_DRIVE_SYNC` UI strings |

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
                                            beginAuthOrSync()
                         ┌────────────────────────┴───────────────┐
              has refresh token?                          no refresh token
                         │                                         │
                 refreshAccessToken()                       AUTH_DEVICE_CODE
                 ┌───────┴────────┐                  (show code+QR, poll token)
              ok │            failed │                          │ success
                 ▼                 ▼                            ▼
              runSync()      start device flow ────────────► runSync()
                 │
                 ▼
          LISTING ─► SYNCING ─► COMPLETE
                 (any failure) ─► ERROR
```

- **CONFIG_NEEDED** is a static instruction screen (config file missing or incomplete). The
  user edits `/.crosspoint/gdrive.json` on a PC and re-opens the menu item to re-read it;
  **Back** exits. The template is written only when the file is absent, so an edited file is
  never clobbered.
- **AUTH_DEVICE_CODE** is the only interactive-while-networking state: `loop()` polls the
  token endpoint at the server-provided interval (honoring `slow_down`) until the user
  authorizes, the code expires, or the user presses Back.
- **LISTING/SYNCING** are synchronous — the blocking network calls run on the main loop, and
  the download progress callback pumps `mappedInput.update()` + `requestUpdate()` so the
  screen stays live and **Back** cancels mid-download (same approach as `FontDownloadActivity`).
- **onExit** calls `silentRestart()` when WiFi was brought up, to reclaim heap fragmentation
  from the TLS/WiFi session (consistent with the other network activities).

## OAuth 2.0 device flow

Implemented in `GoogleDriveClient`. The OAuth client must be of type **TV and Limited Input**.

1. `requestDeviceCode(clientId)` → `POST https://oauth2.googleapis.com/device/code`
   (`scope=drive.readonly`). Returns `device_code`, `user_code`, `verification_url`,
   `interval`, `expires_in` into a fixed-buffer `DeviceCodeInfo` (no heap strings).
2. `pollForToken(...)` → `POST https://oauth2.googleapis.com/token`
   (`grant_type=urn:ietf:params:oauth:grant-type:device_code`). Returns a `PollStatus`:
   `PENDING` / `SLOW_DOWN` keep polling, `SUCCESS` yields the **refresh token** (persisted)
   + access token, and `DENIED`/`EXPIRED`/`ERROR` stop the flow.
3. `refreshAccessToken(...)` → `POST .../token` (`grant_type=refresh_token`). Called at the
   start of every sync; the access token is kept only in RAM.

`HttpDownloader::postForm()` (added for this feature) issues the
`application/x-www-form-urlencoded` POSTs and returns the body **and HTTP status for any
status code** (`requireOk = false` internally), because the device flow signals "pending"
and "slow down" through non-200 responses that carry an `error` field in the JSON.

### Security note: scope is account-wide

Google Drive has **no single-folder OAuth scope**. `drive.readonly` grants read access to the
**entire** Drive of whatever account authorizes the device; the code only restricts to one
folder by *convention* (the `q='<folderId>' in parents` query), not by a hard boundary. If the
stored refresh token were extracted from the SD card, it could read that whole account.

**Recommended mitigation (documented for users, not enforced in code):** authorize against a
**dedicated throwaway Google account** that only has the library folder shared into it, so a
leaked token exposes nothing but books. (`drive.file` was rejected because it can't list a
pre-existing shared folder without a browser Picker; an on-device service-account key would be
worse to leak.)

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

Configuration is **file-based** — the user edits this file on a PC rather than typing OAuth IDs
on the device. `GoogleDriveStore` has three relevant entry points:

- `writeConfigTemplate()` — writes an editable, pretty-printed template when no file exists:

  ```jsonc
  {
    "_instructions": "Fill in clientId, clientSecret and folderId … (security warning)",
    "clientId": "", "clientSecret": "", "folderId": "", "syncFolder": "/"
  }
  ```

  Note `clientSecret` here is a **plaintext** key — what the user pastes into.

- `loadConfig()` → `ConfigStatus { NoFile, Invalid, Incomplete, Ready }`. It reads the
  obfuscated `clientSecret_obf` if present; otherwise it falls back to the plaintext
  `clientSecret` key (the just-filled template) and flags a re-save. Returns `Incomplete` if any
  of clientId/clientSecret/folderId is blank, `Invalid` on a JSON parse error (the file is **not**
  overwritten in that case), else `Ready`.

- `saveToFile()` — writes the configured form, **never** plaintext secrets:

  ```jsonc
  {
    "_instructions":    "Configured. clientSecret and refreshToken are obfuscated …",
    "clientId":         "….apps.googleusercontent.com", // plaintext
    "folderId":         "1Ab…",                          // plaintext
    "syncFolder":       "/",                             // SD destination, plaintext
    "clientSecret_obf": "…",                             // obfuscated
    "refreshToken_obf": "…",                             // obfuscated (set after OAuth)
    "manifest": [ { "id": "<fileId>", "md5": "<32 hex>" }, … ]
  }
  ```

The **plaintext→obfuscated migration** is the key behaviour: the first `loadConfig()` after the
user fills in the template sees the plaintext `clientSecret`, returns `Ready`, and immediately
calls `saveToFile()` — which writes `clientSecret_obf` and drops the plaintext key. Using two
distinct keys (`clientSecret` vs `clientSecret_obf`) makes "is it obfuscated yet?" unambiguous.
Obfuscation reuses `obfuscation::obfuscateToBase64`/`deobfuscateFromBase64`
(`lib/Serialization/ObfuscationUtils.h`) — XOR with the device MAC then base64: not cryptographic,
but ties secrets to the device and prevents casual reading off the card.

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

1. Create an OAuth client (type **TV and Limited Input**, scope `drive.readonly`), ideally
   under a dedicated books-only Google account with the library folder shared in.
2. `Settings → System → Google Drive Sync` → enter Client ID / Secret / Folder ID once.
3. Connect WiFi, complete the device-code flow on a phone; watch serial (`LOG_LEVEL=2`,
   tag `GDRIVE`) for list/download/skip logs.
4. Confirm books appear in the file browser and open/render.
5. Re-run sync → already-present files are **skipped** (md5 match).
6. Replace a file in Drive → next sync re-downloads only that file.
7. Heap stays healthy across repeated syncs (`ESP.getFreeHeap()`), no leak.
