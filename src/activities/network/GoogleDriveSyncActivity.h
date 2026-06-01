#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "network/GoogleDriveClient.h"

/**
 * Pulls books from a configured Google Drive folder over WiFi.
 *
 * Config is supplied via a JSON file on the SD card (typing OAuth IDs on the
 * e-ink keyboard is impractical): on first run the device writes an editable
 * template to /.crosspoint/gdrive.json and asks the user to fill it in on a PC;
 * on the next run it loads the file, obfuscating the plaintext secret in place.
 *
 * Once configured: connect WiFi -> sync the clock (NTP) and mint an access
 * token from the service-account key (JWT-bearer) -> list the folder ->
 * download anything new or changed, skipping files already present (md5 +
 * existence dedup). Network listing/downloading runs synchronously while a
 * progress screen is shown (the FontDownloadActivity pattern).
 */
class GoogleDriveSyncActivity final : public Activity {
 public:
  explicit GoogleDriveSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool preventAutoSleep() override {
    return state_ == AUTHENTICATING || state_ == LISTING || state_ == SYNCING || state_ == COMPLETE || state_ == ERROR;
  }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    CONFIG_NEEDED,   // config file missing/incomplete — instruct user to edit it on a PC
    WIFI_SELECTION,  // WifiSelectionActivity sub-activity is up
    AUTHENTICATING,  // syncing clock + minting a service-account access token
    LISTING,         // fetching the folder listing
    SYNCING,         // downloading new/changed files
    COMPLETE,
    ERROR,
  };

  State state_ = CONFIG_NEEDED;

  std::string accessToken_;

  // Listing / sync progress
  std::vector<GoogleDriveClient::DriveFile> files_;
  size_t fileIndex_ = 0;        // 1-based index of the file currently downloading
  size_t downloadedCount_ = 0;  // files actually fetched this run
  size_t skippedCount_ = 0;     // files already present and skipped
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  std::string currentFileName_;

  std::string errorMessage_;
  std::string configMessage_;  // shown in the CONFIG_NEEDED state
  bool cancelRequested_ = false;
  bool wifiStarted_ = false;

  // Load the SD config file and route to the right state (CONFIG_NEEDED / ERROR
  // / WIFI_SELECTION). Writes the editable template when no file exists.
  void checkConfigAndStart();

  void startWifi();
  void onWifiSelectionComplete(bool connected);

  // After WiFi is up: sync the clock, mint a service-account access token, then
  // run the sync. Sets state_/errorMessage_ on failure.
  void authenticateAndSync();

  // Synchronous listing + download. Sets state_/errorMessage_ on failure.
  void runSync();

  std::string destPathFor(const char* fileName) const;
  std::string booksPathFor(const char* fileName) const;
  static std::string formatSize(size_t bytes);

  // Returns GoogleDriveClient's detailed last-error if present, else fallback.
  static std::string detailOr(const char* fallback);
};
