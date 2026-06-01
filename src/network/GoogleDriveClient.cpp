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
