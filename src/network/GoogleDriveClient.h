#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "HttpDownloader.h"

/**
 * Minimal Google Drive v3 + OAuth 2.0 client for the e-reader.
 *
 * Stateless: every call takes the credentials/tokens it needs and returns
 * results through out-parameters, so there is no UI or SD coupling here. Auth
 * uses the OAuth 2.0 "TV and Limited Input" device flow (read-only scope), and
 * listing/downloading hit the Drive v3 REST API over the shared HttpDownloader
 * TLS path. Network responses are parsed with ArduinoJson using a field filter
 * to keep peak RAM bounded.
 */
class GoogleDriveClient {
 public:
  // Read-only scope: the device never modifies anything in Drive.
  static constexpr const char* SCOPE = "https://www.googleapis.com/auth/drive.readonly";

  // Returned by requestDeviceCode(); shown to the user so they can authorize on
  // a second device. Fixed buffers (no std::string) keep this cheap to hold.
  struct DeviceCodeInfo {
    char deviceCode[128] = {};      // opaque code polled against the token endpoint
    char userCode[32] = {};         // short code the user types on their phone
    char verificationUrl[96] = {};  // URL the user opens to authorize
    int interval = 5;               // seconds to wait between poll attempts
    int expiresIn = 1800;           // seconds until the device code expires
  };

  // Outcome of a single token-poll attempt during the device flow.
  enum class PollStatus {
    SUCCESS,    // tokens issued — outRefreshToken/outAccessToken populated
    PENDING,    // user has not authorized yet — keep polling
    SLOW_DOWN,  // polling too fast — increase the interval and keep polling
    DENIED,     // user declined authorization — stop
    EXPIRED,    // device code expired before authorization — restart the flow
    ERROR,      // transport or unexpected error — stop
  };

  // One book entry from a Drive folder listing. Fixed buffers avoid std::string
  // churn while paging a potentially large library.
  struct DriveFile {
    char id[72] = {};
    char name[160] = {};
    char md5[40] = {};  // Drive md5Checksum (32 hex chars) for dedup
    size_t size = 0;
  };

  // --- OAuth 2.0 device flow ---

  // Step 1: request a device/user code pair for the given OAuth client.
  static bool requestDeviceCode(const std::string& clientId, DeviceCodeInfo& out);

  // Step 2: poll until the user authorizes (or the flow fails). On SUCCESS the
  // refresh token (persist this) and a short-lived access token are returned.
  static PollStatus pollForToken(const std::string& clientId, const std::string& clientSecret,
                                 const std::string& deviceCode, std::string& outRefreshToken,
                                 std::string& outAccessToken);

  // Exchange a stored refresh token for a fresh access token (called once per
  // sync). The access token is kept only in RAM.
  static bool refreshAccessToken(const std::string& clientId, const std::string& clientSecret,
                                 const std::string& refreshToken, std::string& outAccessToken);

  // --- Drive v3 ---

  // List the supported book files directly inside folderId (handles paging).
  static bool listFolder(const std::string& folderId, const std::string& accessToken, std::vector<DriveFile>& out);

  // Stream a file's content to destPath on the SD card.
  static HttpDownloader::DownloadError downloadFile(const std::string& fileId, const std::string& accessToken,
                                                    const std::string& destPath,
                                                    HttpDownloader::ProgressCallback progress = nullptr,
                                                    bool* cancelFlag = nullptr);
};
