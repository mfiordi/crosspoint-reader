#include "GoogleDriveStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

namespace {
constexpr char GDRIVE_FILE_JSON[] = "/.crosspoint/gdrive.json";

// Shown inside the JSON itself so a user who opens the file knows what to do.
// Plain ASCII, single line (valid JSON string value — no comments allowed).
constexpr char TEMPLATE_INSTRUCTIONS[] =
    "Fill in clientId, clientSecret and folderId, then run Google Drive Sync again on the "
    "device. WARNING: these credentials grant READ access to the ENTIRE Google Drive of the "
    "account you authorize - use a dedicated account that only holds your books. See "
    "docs/google-drive-sync-setup.md.";
constexpr char CONFIGURED_NOTE[] =
    "Configured. clientSecret and refreshToken are obfuscated for this device. To reconfigure, "
    "delete this file and run Google Drive Sync again.";
}  // namespace

GoogleDriveStore GoogleDriveStore::instance;

GoogleDriveStore::ConfigStatus GoogleDriveStore::loadConfig() {
  if (!Storage.exists(GDRIVE_FILE_JSON)) return ConfigStatus::NoFile;

  String json = Storage.readFile(GDRIVE_FILE_JSON);
  if (json.isEmpty()) return ConfigStatus::Invalid;

  JsonDocument doc;
  const auto err = deserializeJson(doc, json.c_str());
  if (err) {
    LOG_ERR("GDRIVE", "config parse error: %s", err.c_str());
    return ConfigStatus::Invalid;
  }

  clientId = doc["clientId"] | std::string("");
  folderId = doc["folderId"] | std::string("");
  syncFolder = doc["syncFolder"] | std::string("/");
  if (syncFolder.empty()) syncFolder = "/";

  // Secret: prefer the obfuscated form; fall back to a plaintext "clientSecret"
  // field (the template the user just filled in) and flag for re-save so it gets
  // obfuscated in place.
  bool needsResave = false;
  bool ok = false;
  clientSecret = obfuscation::deobfuscateFromBase64(doc["clientSecret_obf"] | "", &ok);
  if (!ok || clientSecret.empty()) {
    clientSecret = doc["clientSecret"] | std::string("");
    if (!clientSecret.empty()) needsResave = true;
  }

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

  if (!hasConfig()) {
    LOG_DBG("GDRIVE", "config incomplete");
    return ConfigStatus::Incomplete;
  }

  // Rewrite so the plaintext secret is replaced by its obfuscated form (and the
  // plaintext key is dropped). saveToFile() never writes plaintext secrets.
  if (needsResave) {
    LOG_DBG("GDRIVE", "obfuscating plaintext secret in config file");
    saveToFile();
  }

  LOG_DBG("GDRIVE", "config ready (%zu manifest entries)", manifest.size());
  return ConfigStatus::Ready;
}

bool GoogleDriveStore::writeConfigTemplate() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["_instructions"] = TEMPLATE_INSTRUCTIONS;
  doc["clientId"] = "";
  doc["clientSecret"] = "";
  doc["folderId"] = "";
  doc["syncFolder"] = "/";

  String json;
  serializeJsonPretty(doc, json);  // pretty so it's easy to edit on a PC
  return Storage.writeFile(GDRIVE_FILE_JSON, json);
}

bool GoogleDriveStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["_instructions"] = CONFIGURED_NOTE;
  doc["clientId"] = clientId;
  doc["folderId"] = folderId;
  doc["syncFolder"] = syncFolder;
  // Secrets are written only in obfuscated form; never persist plaintext.
  doc["clientSecret_obf"] = obfuscation::obfuscateToBase64(clientSecret);
  doc["refreshToken_obf"] = obfuscation::obfuscateToBase64(refreshToken);

  JsonArray arr = doc["manifest"].to<JsonArray>();
  for (const auto& e : manifest) {
    JsonObject obj = arr.add<JsonObject>();
    obj["id"] = e.fileId;
    obj["md5"] = e.md5;
  }

  String json;
  serializeJsonPretty(doc, json);
  return Storage.writeFile(GDRIVE_FILE_JSON, json);
}

void GoogleDriveStore::setRefreshToken(const std::string& v) {
  if (v == refreshToken) return;
  refreshToken = v;
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
