#include "GoogleDriveStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

namespace {
constexpr char GDRIVE_FILE_JSON[] = "/.crosspoint/gdrive.json";
constexpr char DEFAULT_TOKEN_URI[] = "https://oauth2.googleapis.com/token";

// Shown inside the JSON itself so a user who opens the file knows what to do.
// Plain ASCII, single line (valid JSON string value — no comments allowed).
constexpr char TEMPLATE_INSTRUCTIONS[] =
    "Paste your Google service-account JSON contents into 'serviceAccount' (client_email, "
    "private_key, token_uri), set folderId, then run Google Drive Sync again. SHARE the books "
    "folder with the service account's client_email (read access). The key only grants access "
    "to what you share. See docs/google-drive-sync-setup.md.";
constexpr char CONFIGURED_NOTE[] =
    "Configured. The service-account private key is obfuscated for this device. To reconfigure, "
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

  folderId = doc["folderId"] | std::string("");
  syncFolder = doc["syncFolder"] | std::string("/");
  if (syncFolder.empty()) syncFolder = "/";

  // The user pastes Google's service-account JSON under "serviceAccount".
  JsonObject sa = doc["serviceAccount"].as<JsonObject>();
  clientEmail = sa["client_email"] | std::string("");
  tokenUri = sa["token_uri"] | std::string(DEFAULT_TOKEN_URI);
  if (tokenUri.empty()) tokenUri = DEFAULT_TOKEN_URI;

  // Private key: prefer the obfuscated form; fall back to the plaintext
  // "private_key" (the freshly-pasted key) and flag for re-save so it gets
  // obfuscated in place. ArduinoJson already turns the JSON \n escapes into real
  // newlines, so the PEM is usable as-is.
  bool needsResave = false;
  bool ok = false;
  privateKey = obfuscation::deobfuscateFromBase64(doc["privateKey_obf"] | "", &ok);
  if (!ok || privateKey.empty()) {
    privateKey = sa["private_key"] | std::string("");
    if (!privateKey.empty()) needsResave = true;
  }

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

  // Rewrite so the plaintext key is replaced by its obfuscated form (and the
  // plaintext serviceAccount.private_key is dropped). saveToFile() never writes
  // the plaintext key.
  if (needsResave) {
    LOG_DBG("GDRIVE", "obfuscating plaintext private key in config file");
    saveToFile();
  }

  LOG_DBG("GDRIVE", "config ready (%zu manifest entries)", manifest.size());
  return ConfigStatus::Ready;
}

bool GoogleDriveStore::writeConfigTemplate() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["_instructions"] = TEMPLATE_INSTRUCTIONS;
  doc["folderId"] = "";
  doc["syncFolder"] = "/";
  JsonObject sa = doc["serviceAccount"].to<JsonObject>();
  sa["client_email"] = "";
  sa["private_key"] = "";
  sa["token_uri"] = DEFAULT_TOKEN_URI;

  String json;
  serializeJsonPretty(doc, json);  // pretty so it's easy to edit on a PC
  return Storage.writeFile(GDRIVE_FILE_JSON, json);
}

bool GoogleDriveStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["_instructions"] = CONFIGURED_NOTE;
  doc["folderId"] = folderId;
  doc["syncFolder"] = syncFolder;
  // Keep the non-secret service-account fields visible; the private key is
  // written only in obfuscated form (never plaintext).
  JsonObject sa = doc["serviceAccount"].to<JsonObject>();
  sa["client_email"] = clientEmail;
  sa["token_uri"] = tokenUri;
  doc["privateKey_obf"] = obfuscation::obfuscateToBase64(privateKey);

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
