#include "GoogleDriveClient.h"

#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace {
constexpr const char* DEVICE_CODE_URL = "https://oauth2.googleapis.com/device/code";
constexpr const char* TOKEN_URL = "https://oauth2.googleapis.com/token";
constexpr const char* FILES_URL = "https://www.googleapis.com/drive/v3/files";
// Listing pages are downloaded here before parsing, mirroring the font manifest
// flow, so the TLS buffers and the full JSON are never held at once.
constexpr const char* LIST_TMP = "/.crosspoint/gdrive_list.tmp";
// Safety cap so a malformed nextPageToken loop can't run forever.
constexpr int MAX_LIST_PAGES = 50;

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
}  // namespace

bool GoogleDriveClient::requestDeviceCode(const std::string& clientId, DeviceCodeInfo& out) {
  const std::string body = "client_id=" + urlEncode(clientId) + "&scope=" + urlEncode(SCOPE);

  std::string resp;
  int status = 0;
  if (!HttpDownloader::postForm(DEVICE_CODE_URL, body, resp, &status)) {
    LOG_ERR("GDRIVE", "device/code request failed");
    return false;
  }
  if (status != 200) {
    LOG_ERR("GDRIVE", "device/code status %d", status);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    LOG_ERR("GDRIVE", "device/code parse error");
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
    LOG_ERR("GDRIVE", "device/code response missing fields");
    return false;
  }
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
    LOG_ERR("GDRIVE", "token poll request failed");
    return PollStatus::ERROR;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    LOG_ERR("GDRIVE", "token poll parse error");
    return PollStatus::ERROR;
  }

  if (status == 200) {
    outAccessToken = doc["access_token"] | std::string("");
    // refresh_token is only present on first authorization; preserve any prior
    // value if the response omits it.
    const std::string refresh = doc["refresh_token"] | std::string("");
    if (!refresh.empty()) outRefreshToken = refresh;
    if (outAccessToken.empty()) {
      LOG_ERR("GDRIVE", "token response missing access_token");
      return PollStatus::ERROR;
    }
    return PollStatus::SUCCESS;
  }

  const char* err = doc["error"] | "";
  if (strcmp(err, "authorization_pending") == 0) return PollStatus::PENDING;
  if (strcmp(err, "slow_down") == 0) return PollStatus::SLOW_DOWN;
  if (strcmp(err, "access_denied") == 0) return PollStatus::DENIED;
  if (strcmp(err, "expired_token") == 0) return PollStatus::EXPIRED;
  LOG_ERR("GDRIVE", "token poll error: %s (status %d)", err, status);
  return PollStatus::ERROR;
}

bool GoogleDriveClient::refreshAccessToken(const std::string& clientId, const std::string& clientSecret,
                                           const std::string& refreshToken, std::string& outAccessToken) {
  const std::string body = "client_id=" + urlEncode(clientId) + "&client_secret=" + urlEncode(clientSecret) +
                           "&refresh_token=" + urlEncode(refreshToken) + "&grant_type=refresh_token";

  std::string resp;
  int status = 0;
  if (!HttpDownloader::postForm(TOKEN_URL, body, resp, &status)) {
    LOG_ERR("GDRIVE", "token refresh request failed");
    return false;
  }
  if (status != 200) {
    LOG_ERR("GDRIVE", "token refresh status %d", status);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    LOG_ERR("GDRIVE", "token refresh parse error");
    return false;
  }

  outAccessToken = doc["access_token"] | std::string("");
  if (outAccessToken.empty()) {
    LOG_ERR("GDRIVE", "token refresh missing access_token");
    return false;
  }
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
      LOG_ERR("GDRIVE", "folder listing fetch failed (%d)", dl);
      Storage.remove(LIST_TMP);
      return false;
    }

    HalFile file;
    if (!Storage.openFileForRead("GDRIVE", LIST_TMP, file)) {
      LOG_ERR("GDRIVE", "failed to open listing temp file");
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
      LOG_ERR("GDRIVE", "listing parse error: %s", err.c_str());
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
