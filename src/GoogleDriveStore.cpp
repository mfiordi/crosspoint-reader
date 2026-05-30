#include "GoogleDriveStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

namespace {
constexpr char GDRIVE_FILE_JSON[] = "/.crosspoint/gdrive.json";
}  // namespace

GoogleDriveStore GoogleDriveStore::instance;

bool GoogleDriveStore::loadFromFile() {
  if (!Storage.exists(GDRIVE_FILE_JSON)) return false;

  String json = Storage.readFile(GDRIVE_FILE_JSON);
  if (json.isEmpty()) return false;

  JsonDocument doc;
  const auto err = deserializeJson(doc, json.c_str());
  if (err) {
    LOG_ERR("GDRIVE", "config parse error: %s", err.c_str());
    return false;
  }

  clientId = doc["clientId"] | std::string("");
  folderId = doc["folderId"] | std::string("");
  syncFolder = doc["syncFolder"] | std::string("/");

  bool ok = false;
  clientSecret = obfuscation::deobfuscateFromBase64(doc["clientSecret_obf"] | "", &ok);
  if (!ok) clientSecret.clear();
  ok = false;
  refreshToken = obfuscation::deobfuscateFromBase64(doc["refreshToken_obf"] | "", &ok);
  if (!ok) refreshToken.clear();

  manifest.clear();
  JsonArray arr = doc["manifest"].as<JsonArray>();
  manifest.reserve(arr.size());
  for (JsonObject obj : arr) {
    ManifestEntry e;
    e.fileId = obj["id"] | std::string("");
    e.md5 = obj["md5"] | std::string("");
    if (!e.fileId.empty()) manifest.push_back(std::move(e));
  }

  LOG_DBG("GDRIVE", "config loaded (%zu manifest entries)", manifest.size());
  return true;
}

bool GoogleDriveStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["clientId"] = clientId;
  doc["folderId"] = folderId;
  doc["syncFolder"] = syncFolder;
  doc["clientSecret_obf"] = obfuscation::obfuscateToBase64(clientSecret);
  doc["refreshToken_obf"] = obfuscation::obfuscateToBase64(refreshToken);

  JsonArray arr = doc["manifest"].to<JsonArray>();
  for (const auto& e : manifest) {
    JsonObject obj = arr.add<JsonObject>();
    obj["id"] = e.fileId;
    obj["md5"] = e.md5;
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(GDRIVE_FILE_JSON, json);
}

void GoogleDriveStore::setClientId(const std::string& v) {
  if (v == clientId) return;
  clientId = v;
  saveToFile();
}

void GoogleDriveStore::setClientSecret(const std::string& v) {
  if (v == clientSecret) return;
  clientSecret = v;
  saveToFile();
}

void GoogleDriveStore::setFolderId(const std::string& v) {
  if (v == folderId) return;
  folderId = v;
  saveToFile();
}

void GoogleDriveStore::setRefreshToken(const std::string& v) {
  if (v == refreshToken) return;
  refreshToken = v;
  saveToFile();
}

void GoogleDriveStore::setSyncFolder(const std::string& v) {
  if (v == syncFolder) return;
  syncFolder = v;
  saveToFile();
}

const std::string* GoogleDriveStore::findMd5(const std::string& fileId) const {
  for (const auto& e : manifest) {
    if (e.fileId == fileId) return &e.md5;
  }
  return nullptr;
}

void GoogleDriveStore::recordEntry(const std::string& fileId, const std::string& md5) {
  for (auto& e : manifest) {
    if (e.fileId == fileId) {
      e.md5 = md5;
      return;
    }
  }
  manifest.push_back({fileId, md5});
}
