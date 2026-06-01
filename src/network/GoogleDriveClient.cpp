#include "GoogleDriveClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr const char* DEVICE_CODE_URL = "https://oauth2.googleapis.com/device/code";
constexpr const char* TOKEN_URL = "https://oauth2.googleapis.com/token";
constexpr const char* FILES_URL = "https://www.googleapis.com/drive/v3/files";
// Listing pages are downloaded here before parsing, mirroring the font manifest
// flow, so the TLS buffers and the full JSON are never held at once.
constexpr const char* LIST_TMP = "/.crosspoint/gdrive_list.tmp";
// Append-only diagnostic log; survives the silentRestart() on activity exit so
// the user can read what happened on a PC. Trimmed if it grows too large.
constexpr const char* LOG_FILE = "/.crosspoint/gdrive_log.txt";
constexpr size_t LOG_MAX_BYTES = 32 * 1024;
// Safety cap so a malformed nextPageToken loop can't run forever.
constexpr int MAX_LIST_PAGES = 50;

// Append one line to the SD log file (timestamped with millis since boot). Best
// effort: never throws, never blocks the sync on failure.
void logToFile(const char* line) {
  Storage.mkdir("/.crosspoint");
  // Keep the file bounded: if it got large, start fresh.
  if (Storage.exists(LOG_FILE)) {
    HalFile probe;
    if (Storage.openFileForRead("GDRIVE", LOG_FILE, probe) && probe.fileSize() > LOG_MAX_BYTES) {
      probe.close();
      Storage.remove(LOG_FILE);
    }
  }
  HalFile f = Storage.open(LOG_FILE, O_WRITE | O_CREAT | O_APPEND);
  if (!f) return;
  char buf[320];
  const int n = snprintf(buf, sizeof(buf), "[%lu] %s\n", static_cast<unsigned long>(millis()), line);
  if (n > 0) f.write(buf, static_cast<size_t>(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
  f.close();
}

// Percent-encode a value for a query string or form body (RFC 3986 unreserved
// set passes through). grant_type and the folder query contain ':' '/' and
// spaces, so every interpolated value must go through this.
std::string urlEncode(const std::string& in) {
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() + in.size() / 2);
  for (unsigned char c : in) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(HEX_DIGITS[c >> 4]);
      out.push_back(HEX_DIGITS[c & 0x0F]);
    }
  }
  return out;
}

void copyStr(char* dst, size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  strncpy(dst, src ? src : "", dstSize - 1);
  dst[dstSize - 1] = '\0';
}

// The device renders these; the optimizer pipeline produces .epub, but accept
// the other natively-supported formats too in case the user drops them in.
bool isSupportedBook(std::string_view name) {
  return FsHelpers::hasEpubExtension(name) || FsHelpers::hasTxtExtension(name) || FsHelpers::hasXtcExtension(name);
}

// Pull Google's "error"/"error_description" out of a JSON error body, if any,
// so the on-screen message names the actual cause (e.g. "invalid_client").
std::string extractJsonError(const std::string& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return "";
  const char* err = doc["error"] | "";
  const char* desc = doc["error_description"] | "";
  std::string out = err;
  if (desc[0] != '\0') {
    if (!out.empty()) out += ": ";
    out += desc;
  }
  return out;
}
}  // namespace

std::string GoogleDriveClient::lastError_;

namespace {
// Record a failure: store it for the UI (GoogleDriveClient::lastError_, via the
// setter below), mirror it to the serial log, and append it to the SD log file.
// Defined as a free function so the anonymous-namespace logToFile is visible;
// the member setter forwards here.
void recordError(std::string& dest, const std::string& msg) {
  dest = msg;
  LOG_ERR("GDRIVE", "%s", msg.c_str());
  logToFile(msg.c_str());
}
}  // namespace

bool GoogleDriveClient::requestDeviceCode(const std::string& clientId, DeviceCodeInfo& out) {
  clearLastError();
  const std::string body = "client_id=" + urlEncode(clientId) + "&scope=" + urlEncode(SCOPE);

  std::string resp;
  int status = 0;
  if (!HttpDownloader::postForm(DEVICE_CODE_URL, body, resp, &status)) {
    recordError(lastError_, "Get code: network/TLS failed (no internet, DNS, or captive portal?)");
    return false;
  }
  if (status != 200) {
    std::string detail = extractJsonError(resp);
    std::string msg = "Get code: HTTP " + std::to_string(status);
    if (status == 401 || status == 400) {
      msg += " - check Client ID and that the OAuth client type is 'TV and Limited Input devices'";
    } else if (status == 403) {
      msg += " - check the Google Drive API is enabled for this project";
    }
    if (!detail.empty()) msg += " [" + detail + "]";
    recordError(lastError_, msg);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    recordError(lastError_, "Get code: could not parse Google's response");
    return false;
  }

  copyStr(out.deviceCode, sizeof(out.deviceCode), doc["device_code"] | "");
  copyStr(out.userCode, sizeof(out.userCode), doc["user_code"] | "");
  // Google has used both "verification_url" (legacy) and "verification_uri".
  const char* url = doc["verification_url"] | (doc["verification_uri"] | "");
  copyStr(out.verificationUrl, sizeof(out.verificationUrl), url);
  out.interval = doc["interval"] | 5;
  out.expiresIn = doc["expires_in"] | 1800;

  if (out.deviceCode[0] == '\0' || out.userCode[0] == '\0') {
    recordError(lastError_, "Get code: response missing device_code/user_code");
    return false;
  }
  logToFile("Get code: ok, waiting for authorization");
  return true;
}

GoogleDriveClient::PollStatus GoogleDriveClient::pollForToken(const std::string& clientId,
                                                              const std::string& clientSecret,
                                                              const std::string& deviceCode,
                                                              std::string& outRefreshToken,
                                                              std::string& outAccessToken) {
  const std::string body = "client_id=" + urlEncode(clientId) + "&client_secret=" + urlEncode(clientSecret) +
                           "&device_code=" + urlEncode(deviceCode) +
                           "&grant_type=" + urlEncode("urn:ietf:params:oauth:grant-type:device_code");

  std::string resp;
  int status = 0;
  if (!HttpDownloader::postForm(TOKEN_URL, body, resp, &status)) {
    recordError(lastError_, "Authorize: network/TLS failed while polling for token");
    return PollStatus::ERROR;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    recordError(lastError_, "Authorize: could not parse token response");
    return PollStatus::ERROR;
  }

  if (status == 200) {
    outAccessToken = doc["access_token"] | std::string("");
    // refresh_token is only present on first authorization; preserve any prior
    // value if the response omits it.
    const std::string refresh = doc["refresh_token"] | std::string("");
    if (!refresh.empty()) outRefreshToken = refresh;
    if (outAccessToken.empty()) {
      recordError(lastError_, "Authorize: token response missing access_token");
      return PollStatus::ERROR;
    }
    logToFile("Authorize: ok, token received");
    return PollStatus::SUCCESS;
  }

  const char* err = doc["error"] | "";
  // These are expected, non-fatal poll states — don't treat as errors.
  if (strcmp(err, "authorization_pending") == 0) return PollStatus::PENDING;
  if (strcmp(err, "slow_down") == 0) return PollStatus::SLOW_DOWN;
  if (strcmp(err, "access_denied") == 0) {
    recordError(lastError_, "Authorize: access denied (approve on the phone with the account you set up)");
    return PollStatus::DENIED;
  }
  if (strcmp(err, "expired_token") == 0) {
    recordError(lastError_, "Authorize: the code expired before approval");
    return PollStatus::EXPIRED;
  }
  {
    std::string detail = extractJsonError(resp);
    std::string msg = "Authorize: HTTP " + std::to_string(status);
    if (detail.find("invalid_client") != std::string::npos)
      msg += " - check the Client Secret";
    else if (!detail.empty())
      msg += " [" + detail + "]";
    recordError(lastError_, msg);
  }
  return PollStatus::ERROR;
}

bool GoogleDriveClient::refreshAccessToken(const std::string& clientId, const std::string& clientSecret,
                                           const std::string& refreshToken, std::string& outAccessToken) {
  const std::string body = "client_id=" + urlEncode(clientId) + "&client_secret=" + urlEncode(clientSecret) +
                           "&refresh_token=" + urlEncode(refreshToken) + "&grant_type=refresh_token";

  std::string resp;
  int status = 0;
  if (!HttpDownloader::postForm(TOKEN_URL, body, resp, &status)) {
    recordError(lastError_, "Refresh token: network/TLS failed");
    return false;
  }
  if (status != 200) {
    // A revoked/expired refresh token comes back as invalid_grant; the caller
    // then falls back to a fresh device-code authorization.
    std::string detail = extractJsonError(resp);
    std::string msg = "Refresh token: HTTP " + std::to_string(status);
    if (!detail.empty()) msg += " [" + detail + "]";
    recordError(lastError_, msg);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    recordError(lastError_, "Refresh token: could not parse response");
    return false;
  }

  outAccessToken = doc["access_token"] | std::string("");
  if (outAccessToken.empty()) {
    recordError(lastError_, "Refresh token: response missing access_token");
    return false;
  }
  logToFile("Refresh token: ok");
  return true;
}

bool GoogleDriveClient::listFolder(const std::string& folderId, const std::string& accessToken,
                                   std::vector<DriveFile>& out) {
  out.clear();
  Storage.mkdir("/.crosspoint");

  const std::string q = "'" + folderId + "' in parents and trashed=false";
  std::string pageToken;

  for (int page = 0; page < MAX_LIST_PAGES; ++page) {
    std::string url = std::string(FILES_URL) + "?q=" + urlEncode(q) +
                      "&fields=" + urlEncode("nextPageToken,files(id,name,mimeType,md5Checksum,size)") +
                      "&pageSize=200&spaces=drive&supportsAllDrives=true&includeItemsFromAllDrives=true";
    if (!pageToken.empty()) url += "&pageToken=" + urlEncode(pageToken);

    const auto dl =
        HttpDownloader::downloadToFile(url, LIST_TMP, nullptr, nullptr, /*username=*/"", /*password=*/"", accessToken);
    if (dl != HttpDownloader::OK) {
      recordError(lastError_, "List folder: request failed - check the Folder ID and that the account can see it");
      Storage.remove(LIST_TMP);
      return false;
    }

    HalFile file;
    if (!Storage.openFileForRead("GDRIVE", LIST_TMP, file)) {
      recordError(lastError_, "List folder: could not read the downloaded listing from SD");
      Storage.remove(LIST_TMP);
      return false;
    }

    // Filter so only the fields we use are materialized, bounding peak RAM.
    JsonDocument filter;
    filter["nextPageToken"] = true;
    filter["files"][0]["id"] = true;
    filter["files"][0]["name"] = true;
    filter["files"][0]["mimeType"] = true;
    filter["files"][0]["md5Checksum"] = true;
    filter["files"][0]["size"] = true;

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, file, DeserializationOption::Filter(filter));
    file.close();
    Storage.remove(LIST_TMP);
    if (err) {
      recordError(lastError_, std::string("List folder: could not parse listing (") + err.c_str() + ")");
      return false;
    }

    JsonArray files = doc["files"].as<JsonArray>();
    out.reserve(out.size() + files.size());
    for (JsonObject obj : files) {
      const char* name = obj["name"] | "";
      if (!isSupportedBook(name)) continue;

      DriveFile df;
      copyStr(df.id, sizeof(df.id), obj["id"] | "");
      copyStr(df.name, sizeof(df.name), name);
      copyStr(df.md5, sizeof(df.md5), obj["md5Checksum"] | "");
      // Drive returns size as a decimal string.
      df.size = strtoull(obj["size"] | "0", nullptr, 10);
      if (df.id[0] != '\0') out.push_back(df);
    }

    pageToken = doc["nextPageToken"] | std::string("");
    if (pageToken.empty()) break;
  }

  char line[64];
  snprintf(line, sizeof(line), "List folder: ok, %u book(s)", static_cast<unsigned>(out.size()));
  logToFile(line);
  LOG_DBG("GDRIVE", "listed %zu book file(s)", out.size());
  return true;
}

HttpDownloader::DownloadError GoogleDriveClient::downloadFile(const std::string& fileId, const std::string& accessToken,
                                                              const std::string& destPath,
                                                              HttpDownloader::ProgressCallback progress,
                                                              bool* cancelFlag) {
  const std::string url = std::string(FILES_URL) + "/" + fileId + "?alt=media&supportsAllDrives=true";
  return HttpDownloader::downloadToFile(url, destPath, std::move(progress), cancelFlag, /*username=*/"",
                                        /*password=*/"", accessToken);
}
