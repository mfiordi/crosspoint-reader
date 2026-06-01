#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files. Built on
 * esp_http_client: https is verified against the CA bundle, plain http is
 * used for local servers (transport is chosen from the URL scheme).
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "");

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Download a file to the SD card with optional credentials. When bearer is
   * non-empty an "Authorization: Bearer <token>" header is sent instead of
   * Basic auth (used for Google Drive's authenticated GET endpoints).
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      const std::string& bearer = "");

  /**
   * POST an application/x-www-form-urlencoded body and capture the response.
   * Used for OAuth 2.0 device-flow endpoints (device/code, token). The full
   * response body is returned in outResponse regardless of HTTP status, and the
   * status code is written to *outStatus when provided, so the caller can tell a
   * pending-authorization response (HTTP 428) from success (200) or a hard
   * error. An optional Bearer token may be sent. Returns true when the HTTP
   * exchange completed and a response was read.
   */
  static bool postForm(const std::string& url, const std::string& formBody, std::string& outResponse,
                       int* outStatus = nullptr, const std::string& bearer = "");
};
