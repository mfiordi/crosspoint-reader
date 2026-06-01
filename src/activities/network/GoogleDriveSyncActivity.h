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
 * Once configured: connect WiFi -> refresh the stored token, or run the OAuth
 * device flow if there is none -> list the folder -> download anything new or
 * changed, skipping files already present (md5 + existence dedup). Network
 * listing/downloading runs synchronously while a progress screen is shown (the
 * FontDownloadActivity pattern); the interactive device-code authorization is
 * polled from loop() so the user can cancel and the screen keeps refreshing.
 */
class GoogleDriveSyncActivity final : public Activity {
 public:
  explicit GoogleDriveSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool preventAutoSleep() override {
    return state_ == AUTH_DEVICE_CODE || state_ == LISTING || state_ == SYNCING || state_ == COMPLETE ||
           state_ == ERROR;
  }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    CONFIG_NEEDED,     // config file missing/incomplete — instruct user to edit it on a PC
    WIFI_SELECTION,    // WifiSelectionActivity sub-activity is up
    AUTH_DEVICE_CODE,  // showing user code + QR, polling for authorization
    LISTING,           // fetching the folder listing
    SYNCING,           // downloading new/changed files
    COMPLETE,
    ERROR,
  };

  State state_ = CONFIG_NEEDED;

  // OAuth device-flow state
  GoogleDriveClient::DeviceCodeInfo deviceCode_;
  std::string accessToken_;
  unsigned long nextPollMs_ = 0;
  unsigned long authExpiryMs_ = 0;
  int pollIntervalSec_ = 5;

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

  // After WiFi is up: refresh the token or kick off the device flow.
  void beginAuthOrSync();
  void startDeviceAuth();
  void pollAuth();

  // Synchronous listing + download. Sets state_/errorMessage_ on failure.
  void runSync();

  std::string destPathFor(const char* fileName) const;
  static std::string formatSize(size_t bytes);
};
