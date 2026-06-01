#include "GoogleDriveSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include "GoogleDriveStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/GoogleJwtAuth.h"
#include "util/BookCacheUtils.h"

GoogleDriveSyncActivity::GoogleDriveSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("GoogleDriveSync", renderer, mappedInput) {}

// --- Lifecycle ---

void GoogleDriveSyncActivity::onEnter() {
  Activity::onEnter();
  checkConfigAndStart();
}

void GoogleDriveSyncActivity::onExit() {
  Activity::onExit();

  // Bringing WiFi + TLS up fragments the heap; reboot to reclaim it (matches
  // FontDownloadActivity). Only when we actually started WiFi.
  if (wifiStarted_ && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

// --- Config (file-based) ---

void GoogleDriveSyncActivity::checkConfigAndStart() {
  switch (GDRIVE_STORE.loadConfig()) {
    case GoogleDriveStore::ConfigStatus::Ready:
      startWifi();
      return;
    case GoogleDriveStore::ConfigStatus::NoFile: {
      // First run: drop an editable template on the SD for the user to fill in.
      const bool wrote = GDRIVE_STORE.writeConfigTemplate();
      RenderLock lock(*this);
      state_ = CONFIG_NEEDED;
      configMessage_ = wrote ? tr(STR_GDRIVE_CONFIG_CREATED) : tr(STR_GDRIVE_CONFIG_INVALID);
      return;
    }
    case GoogleDriveStore::ConfigStatus::Incomplete: {
      RenderLock lock(*this);
      state_ = CONFIG_NEEDED;
      configMessage_ = tr(STR_GDRIVE_CONFIG_INCOMPLETE);
      return;
    }
    case GoogleDriveStore::ConfigStatus::Invalid: {
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = tr(STR_GDRIVE_CONFIG_INVALID);
      return;
    }
  }
}

// --- WiFi ---

void GoogleDriveSyncActivity::startWifi() {
  wifiStarted_ = true;
  WiFi.mode(WIFI_STA);
  {
    RenderLock lock(*this);
    state_ = WIFI_SELECTION;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void GoogleDriveSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    finish();
    return;
  }
  authenticateAndSync();
}

// --- Auth + sync ---

namespace {
// Sync the system clock from NTP. The X4 has no RTC, so time(nullptr) is only
// real epoch after this; the JWT's iat/exp need it. Same pattern as
// KOReaderSyncActivity. Blocks up to ~5s.
void syncTimeWithNTP() {
  if (esp_sntp_enabled()) esp_sntp_stop();
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();
  int retry = 0;
  const int maxRetries = 50;  // 5 seconds max
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < maxRetries) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    retry++;
  }
}
}  // namespace

void GoogleDriveSyncActivity::authenticateAndSync() {
  {
    RenderLock lock(*this);
    state_ = AUTHENTICATING;
  }
  requestUpdateAndWait();

  // JWT iat/exp need a correct wall clock; the X4 has no RTC.
  syncTimeWithNTP();

  if (!GoogleJwtAuth::getAccessToken(GDRIVE_STORE.getClientEmail(), GDRIVE_STORE.getPrivateKey(),
                                     GDRIVE_STORE.getTokenUri(), GoogleJwtAuth::SCOPE_DRIVE_READONLY, accessToken_)) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ =
        GoogleJwtAuth::lastError().empty() ? std::string(tr(STR_GDRIVE_AUTH_FAILED)) : GoogleJwtAuth::lastError();
    return;
  }
  runSync();
}

void GoogleDriveSyncActivity::runSync() {
  {
    RenderLock lock(*this);
    state_ = LISTING;
  }
  requestUpdateAndWait();

  files_.clear();
  if (!GoogleDriveClient::listFolder(GDRIVE_STORE.getFolderId(), accessToken_, files_)) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = detailOr(tr(STR_GDRIVE_LIST_FAILED));
    return;
  }

  downloadedCount_ = 0;
  skippedCount_ = 0;
  cancelRequested_ = false;
  {
    RenderLock lock(*this);
    state_ = SYNCING;
  }
  requestUpdateAndWait();

  for (size_t i = 0; i < files_.size(); i++) {
    if (cancelRequested_) break;

    const auto& f = files_[i];
    const std::string destPath = destPathFor(f.name);

    // Dedup: skip when we recorded this id with a matching md5 and the file is
    // still on the card. The md5 catches updated files; the existence check
    // catches manual deletion on the device.
    const std::string* recordedMd5 = GDRIVE_STORE.findMd5(f.id);
    if (recordedMd5 && f.md5[0] != '\0' && *recordedMd5 == f.md5 && Storage.exists(destPath.c_str())) {
      skippedCount_++;
      continue;
    }

    {
      RenderLock lock(*this);
      fileIndex_ = i + 1;
      currentFileName_ = f.name;
      fileProgress_ = 0;
      fileTotal_ = f.size;
    }
    requestUpdateAndWait();

    const auto result = GoogleDriveClient::downloadFile(
        f.id, accessToken_, destPath,
        [this](size_t downloaded, size_t total) {
          fileProgress_ = downloaded;
          fileTotal_ = total;
          mappedInput.update();
          if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
              mappedInput.wasPressed(MappedInputManager::Button::Back)) {
            cancelRequested_ = true;
          }
          requestUpdate(true);
        },
        &cancelRequested_);

    if (result == HttpDownloader::ABORTED) {
      Storage.remove(destPath.c_str());
      break;
    }
    if (result != HttpDownloader::OK) {
      LOG_ERR("GDRIVE", "download failed: %s (%d)", f.name, result);
      GDRIVE_STORE.saveToFile();  // keep the dedup records earned so far
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = std::string(tr(STR_GDRIVE_DOWNLOAD_FAILED)) + " " + f.name;
      return;
    }

    // A replaced book invalidates its cached layout; clear it so it re-renders.
    clearBookCache(destPath);
    GDRIVE_STORE.recordEntry(f.id, f.md5);
    downloadedCount_++;
  }

  GDRIVE_STORE.saveToFile();  // persist the updated manifest

  RenderLock lock(*this);
  state_ = COMPLETE;
}

// --- Helpers ---

std::string GoogleDriveSyncActivity::destPathFor(const char* fileName) const {
  std::string folder = GDRIVE_STORE.getSyncFolder();
  if (folder.empty()) folder = "/";
  if (folder.back() != '/') folder.push_back('/');
  folder += fileName;
  return folder;
}

std::string GoogleDriveSyncActivity::detailOr(const char* fallback) {
  const std::string& detail = GoogleDriveClient::lastError();
  return detail.empty() ? std::string(fallback) : detail;
}

std::string GoogleDriveSyncActivity::formatSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

// --- Input ---

void GoogleDriveSyncActivity::loop() {
  if (state_ == CONFIG_NEEDED) {
    // Re-reading the edited file is done by re-opening the menu item, so Back
    // just exits.
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
  } else if (state_ == COMPLETE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Retry the network steps if we still have a connection.
      if (wifiStarted_ && WiFi.status() == WL_CONNECTED && GDRIVE_STORE.hasConfig()) {
        authenticateAndSync();
      } else {
        finish();
      }
    }
  }
}

// --- Rendering ---

void GoogleDriveSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight) / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GOOGLE_DRIVE_SYNC));

  // Word-wrap long detail text (error reasons, file names, hints) across the
  // content width and draw it centered starting at y. Returns the y below the
  // last line drawn. drawCenteredText alone clips anything wider than the
  // screen, so anything user/server-supplied must go through here.
  const int contentWidth = pageWidth - metrics.contentSidePadding * 2;
  auto drawWrappedCentered = [&](int y, const char* text, int maxLines, EpdFontFamily::Style style) -> int {
    for (const auto& wline : renderer.wrappedText(UI_10_FONT_ID, text, contentWidth, maxLines, style)) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, wline.c_str(), true, style);
      y += lineHeight;
    }
    return y;
  };

  if (state_ == CONFIG_NEEDED) {
    int y = drawWrappedCentered(centerY - lineHeight * 2, configMessage_.c_str(), 2, EpdFontFamily::BOLD);
    drawWrappedCentered(y + metrics.verticalSpacing, tr(STR_GDRIVE_CONFIG_EDIT_HINT), 3, EpdFontFamily::REGULAR);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == AUTHENTICATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_GDRIVE_AUTHENTICATING));
  } else if (state_ == LISTING) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_GDRIVE_LISTING));
  } else if (state_ == SYNCING) {
    std::string statusText = std::string(tr(STR_GDRIVE_SYNCING)) + " (" + std::to_string(fileIndex_) + "/" +
                             std::to_string(files_.size()) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight * 2, statusText.c_str());
    if (!currentFileName_.empty()) {
      // File names can be long; wrap to a single ellipsized line so it fits.
      drawWrappedCentered(centerY - lineHeight, currentFileName_.c_str(), 1, EpdFontFamily::REGULAR);
    }

    float progress = fileTotal_ > 0 ? static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_) : 0.0f;
    const int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_GDRIVE_DONE), true, EpdFontFamily::BOLD);
    std::string summary;
    if (downloadedCount_ == 0) {
      summary = tr(STR_GDRIVE_UP_TO_DATE);
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%u %s, %u %s", (unsigned)downloadedCount_, tr(STR_GDRIVE_DOWNLOADED),
               (unsigned)skippedCount_, tr(STR_GDRIVE_SKIPPED));
      summary = buf;
    }
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, summary.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    // Put the (possibly long) detail above the title so wrapping has room to
    // grow downward toward the button hints without overrunning them.
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight * 2, tr(STR_GDRIVE_SYNC_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      drawWrappedCentered(centerY, errorMessage_.c_str(), 5, EpdFontFamily::REGULAR);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
