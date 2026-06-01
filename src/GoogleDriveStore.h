#pragma once

#include <string>
#include <vector>

/**
 * Persistent configuration and sync manifest for Google Drive book sync.
 *
 * Stored at /.crosspoint/gdrive.json. The user supplies the OAuth client by
 * editing this file on a PC: the device writes a template on first run, the
 * user fills in clientId / clientSecret / folderId in plaintext, and on the next
 * run the device obfuscates the secret in place (XOR with the device MAC, then
 * base64 — same scheme as WiFi passwords) and rewrites the file. The refresh
 * token (obtained via OAuth) is also stored obfuscated. The manifest maps a
 * Drive fileId to the md5Checksum last downloaded, which is how the sync skips
 * books it already has.
 */
class GoogleDriveStore {
 public:
  // Result of loading + validating the on-SD config file.
  enum class ConfigStatus {
    NoFile,      // file does not exist — caller should write a template
    Invalid,     // file exists but is not parseable JSON — do NOT overwrite
    Incomplete,  // parsed, but clientId/clientSecret/folderId not all filled in
    Ready,       // all three present (secret obfuscated in place if needed)
  };

  struct ManifestEntry {
    std::string fileId;
    std::string md5;
  };

  GoogleDriveStore(const GoogleDriveStore&) = delete;
  GoogleDriveStore& operator=(const GoogleDriveStore&) = delete;

  static GoogleDriveStore& getInstance() { return instance; }

  // Load + validate /.crosspoint/gdrive.json. If the secret was provided in
  // plaintext (template just filled in by the user), it is obfuscated and the
  // file rewritten before returning Ready.
  ConfigStatus loadConfig();

  // Write an editable template (empty fields + an _instructions string) for the
  // user to fill in on a PC. Only call when loadConfig() returned NoFile so an
  // edited file is never clobbered.
  bool writeConfigTemplate() const;

  bool saveToFile() const;

  // --- Config accessors ---
  const std::string& getClientId() const { return clientId; }
  const std::string& getClientSecret() const { return clientSecret; }
  const std::string& getFolderId() const { return folderId; }
  const std::string& getRefreshToken() const { return refreshToken; }
  const std::string& getSyncFolder() const { return syncFolder; }

  // Persists only when the value actually changes (SD write hygiene). Used by
  // the OAuth flow to store the refresh token after authorization.
  void setRefreshToken(const std::string& v);

  bool hasConfig() const { return !clientId.empty() && !clientSecret.empty() && !folderId.empty(); }
  bool hasRefreshToken() const { return !refreshToken.empty(); }

  // --- Manifest (dedup) ---
  // Returns the stored md5 for a fileId, or nullptr if not previously synced.
  const std::string* findMd5(const std::string& fileId) const;
  // Insert or update the md5 recorded for a fileId.
  void recordEntry(const std::string& fileId, const std::string& md5);
  void clearManifest() { manifest.clear(); }

 private:
  GoogleDriveStore() = default;
  static GoogleDriveStore instance;

  std::string clientId;
  std::string clientSecret;
  std::string folderId;
  std::string refreshToken;
  std::string syncFolder = "/";
  std::vector<ManifestEntry> manifest;
};

#define GDRIVE_STORE GoogleDriveStore::getInstance()
