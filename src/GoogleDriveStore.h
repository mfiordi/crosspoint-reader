#pragma once

#include <string>
#include <vector>

/**
 * Persistent configuration and sync manifest for Google Drive book sync.
 *
 * Stored at /.crosspoint/gdrive.json. Authentication uses a Google service
 * account: the user pastes Google's service-account JSON (verbatim) under a
 * "serviceAccount" key, then shares the books folder with the account's
 * client_email. The device writes a template on first run; on the next run it
 * obfuscates the private key in place (XOR with the device MAC, then base64 —
 * same scheme as WiFi passwords) and rewrites the file. The manifest maps a
 * Drive fileId to the md5Checksum last downloaded, which is how the sync skips
 * books it already has.
 */
class GoogleDriveStore {
 public:
  // Result of loading + validating the on-SD config file.
  enum class ConfigStatus {
    NoFile,      // file does not exist — caller should write a template
    Invalid,     // file exists but is not parseable JSON — do NOT overwrite
    Incomplete,  // parsed, but client_email/private_key/folderId not all filled in
    Ready,       // all present (private key obfuscated in place if needed)
  };

  struct ManifestEntry {
    std::string fileId;
    std::string md5;
  };

  GoogleDriveStore(const GoogleDriveStore&) = delete;
  GoogleDriveStore& operator=(const GoogleDriveStore&) = delete;

  static GoogleDriveStore& getInstance() { return instance; }

  // Load + validate /.crosspoint/gdrive.json. If the private key was provided
  // in plaintext (template just filled in / pasted Google JSON), it is
  // obfuscated and the file rewritten before returning Ready.
  ConfigStatus loadConfig();

  // Write an editable template (empty serviceAccount fields + an _instructions
  // string) for the user to fill in on a PC. Only call when loadConfig()
  // returned NoFile so an edited file is never clobbered.
  bool writeConfigTemplate() const;

  bool saveToFile() const;

  // --- Config accessors ---
  const std::string& getClientEmail() const { return clientEmail; }
  const std::string& getPrivateKey() const { return privateKey; }
  const std::string& getTokenUri() const { return tokenUri; }
  const std::string& getFolderId() const { return folderId; }
  const std::string& getSyncFolder() const { return syncFolder; }

  bool hasConfig() const { return !clientEmail.empty() && !privateKey.empty() && !folderId.empty(); }

  // --- Manifest (dedup) ---
  // Returns the stored md5 for a fileId, or nullptr if not previously synced.
  const std::string* findMd5(const std::string& fileId) const;
  // Insert or update the md5 recorded for a fileId.
  void recordEntry(const std::string& fileId, const std::string& md5);
  void clearManifest() { manifest.clear(); }

 private:
  GoogleDriveStore() = default;
  static GoogleDriveStore instance;

  std::string clientEmail;
  std::string privateKey;  // PEM, real newlines; obfuscated on disk
  std::string tokenUri = "https://oauth2.googleapis.com/token";
  std::string folderId;
  std::string syncFolder = "/";
  std::vector<ManifestEntry> manifest;
};

#define GDRIVE_STORE GoogleDriveStore::getInstance()
