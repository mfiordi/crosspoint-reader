#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "HttpDownloader.h"

/**
 * Minimal Google Drive v3 client for the e-reader.
 *
 * Stateless: listing/downloading take an access token (minted by GoogleJwtAuth
 * from a service-account key) and return results through out-parameters, so
 * there is no UI or SD coupling here. Responses are parsed with ArduinoJson
 * using a field filter to keep peak RAM bounded.
 */
class GoogleDriveClient {
 public:
  // One book entry from a Drive folder listing. Fixed buffers avoid std::string
  // churn while paging a potentially large library.
  struct DriveFile {
    char id[72] = {};
    char name[160] = {};
    char md5[40] = {};  // Drive md5Checksum (32 hex chars) for dedup
    size_t size = 0;
  };

  // List the supported book files directly inside folderId (handles paging).
  static bool listFolder(const std::string& folderId, const std::string& accessToken, std::vector<DriveFile>& out);

  // Stream a file's content to destPath on the SD card.
  static HttpDownloader::DownloadError downloadFile(const std::string& fileId, const std::string& accessToken,
                                                    const std::string& destPath,
                                                    HttpDownloader::ProgressCallback progress = nullptr,
                                                    bool* cancelFlag = nullptr);

  // --- Diagnostics ---

  // Human-readable detail for the most recent failure (failing step + HTTP
  // status, when available). Empty until something fails. The activity shows
  // this on screen so the user can diagnose without serial.
  static const std::string& lastError() { return lastError_; }
  static void clearLastError() { lastError_.clear(); }

 private:
  static std::string lastError_;
};
