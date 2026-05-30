#pragma once

#include <string>
#include <vector>

/**
 * Persistent configuration and sync manifest for Google Drive book sync.
 *
 * Stored at /.crosspoint/gdrive.json. The OAuth client secret and refresh token
 * are XOR-obfuscated with the device MAC and base64-encoded (same scheme as WiFi
 * passwords); the client id, folder id and destination folder are stored in the
 * clear. The manifest maps a Drive fileId to the md5Checksum last downloaded,
 * which is how the sync skips books it already has.
 */
class GoogleDriveStore {
 public:
  struct ManifestEntry {
    std::string fileId;
    std::string md5;
  };

  GoogleDriveStore(const GoogleDriveStore&) = delete;
  GoogleDriveStore& operator=(const GoogleDriveStore&) = delete;

  static GoogleDriveStore& getInstance() { return instance; }

  bool loadFromFile();
  bool saveToFile() const;

  // --- Config ---
  const std::string& getClientId() const { return clientId; }
  const std::string& getClientSecret() const { return clientSecret; }
  const std::string& getFolderId() const { return folderId; }
  const std::string& getRefreshToken() const { return refreshToken; }
  const std::string& getSyncFolder() const { return syncFolder; }

  // Setters persist only when the value actually changes (SPIFFS/SD write hygiene).
  void setClientId(const std::string& v);
  void setClientSecret(const std::string& v);
  void setFolderId(const std::string& v);
  void setRefreshToken(const std::string& v);
  void setSyncFolder(const std::string& v);

  // True once the user-supplied OAuth client + target folder are all set.
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
