#include "GoogleJwtAuth.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include <cstring>
#include <ctime>
#include <string>

#include "HttpDownloader.h"

namespace GoogleJwtAuth {
namespace {
std::string g_lastError;

void setError(const std::string& msg) {
  g_lastError = msg;
  LOG_ERR("GJWT", "%s", msg.c_str());
}

// RFC 7515 base64url: standard base64 then +/ -> -_ and strip '=' padding.
std::string base64Url(const unsigned char* data, size_t len) {
  size_t olen = 0;
  // First call sizes the output buffer.
  mbedtls_base64_encode(nullptr, 0, &olen, data, len);
  std::string out(olen, '\0');
  if (olen > 0 && mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&out[0]), out.size(), &olen, data, len) != 0) {
    return "";
  }
  out.resize(olen);
  for (char& c : out) {
    if (c == '+')
      c = '-';
    else if (c == '/')
      c = '_';
  }
  while (!out.empty() && out.back() == '=') out.pop_back();
  return out;
}

std::string base64Url(const std::string& s) {
  return base64Url(reinterpret_cast<const unsigned char*>(s.data()), s.size());
}
}  // namespace

const std::string& lastError() { return g_lastError; }

bool getAccessToken(const std::string& clientEmail, const std::string& privateKeyPem, const std::string& tokenUri,
                    const std::string& scope, std::string& outAccessToken) {
  g_lastError.clear();

  // Need real wall-clock time for iat/exp; before NTP, time() is seconds since
  // boot and Google would reject the assertion as invalid_grant.
  const time_t now = time(nullptr);
  if (now < 1700000000) {  // ~2023-11; anything earlier means the clock isn't set
    setError("Authenticate: device clock not set (NTP sync failed?)");
    return false;
  }
  const long iat = static_cast<long>(now);
  const long exp = iat + 3600;

  // --- Build JWT header.payload ---
  const std::string header = R"({"alg":"RS256","typ":"JWT"})";
  char claims[512];
  snprintf(claims, sizeof(claims), "{\"iss\":\"%s\",\"scope\":\"%s\",\"aud\":\"%s\",\"iat\":%ld,\"exp\":%ld}",
           clientEmail.c_str(), scope.c_str(), tokenUri.c_str(), iat, exp);

  const std::string signingInput = base64Url(header) + "." + base64Url(std::string(claims));
  if (signingInput.size() < 4) {
    setError("Authenticate: failed to encode JWT");
    return false;
  }

  // --- Hash the signing input ---
  unsigned char digest[32];
  if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(signingInput.data()), signingInput.size(), digest, 0) !=
      0) {
    setError("Authenticate: SHA-256 failed");
    return false;
  }

  // --- Parse the RSA private key and sign (RS256) ---
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);

  bool ok = false;
  do {
    const char* pers = "gdrive_jwt";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, reinterpret_cast<const unsigned char*>(pers),
                              strlen(pers)) != 0) {
      setError("Authenticate: RNG seed failed");
      break;
    }
    // mbedTLS PEM parsing expects keylen to include the terminating NUL.
    const int pkRet = mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char*>(privateKeyPem.c_str()),
                                           privateKeyPem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random, &ctr_drbg);
    if (pkRet != 0) {
      setError("Authenticate: invalid private_key (check the pasted key)");
      break;
    }

    unsigned char sig[512];  // RSA-2048 signature is 256 bytes; 4096-bit fits in 512
    size_t sigLen = 0;
    if (mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, digest, sizeof(digest), sig, sizeof(sig), &sigLen,
                        mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
      setError("Authenticate: RSA signing failed");
      break;
    }

    const std::string jwt = signingInput + "." + base64Url(sig, sigLen);

    // --- Exchange the JWT for an access token ---
    const std::string body = "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=" + jwt;
    std::string resp;
    int status = 0;
    if (!HttpDownloader::postForm(tokenUri, body, resp, &status)) {
      setError("Authenticate: token request failed (network/TLS)");
      break;
    }

    JsonDocument doc;
    if (deserializeJson(doc, resp)) {
      setError("Authenticate: could not parse token response");
      break;
    }
    if (status != 200) {
      const char* err = doc["error"] | "";
      const char* desc = doc["error_description"] | "";
      std::string msg = "Authenticate: HTTP " + std::to_string(status);
      if (err[0] != '\0') {
        msg += std::string(" [") + err;
        if (desc[0] != '\0') msg += std::string(": ") + desc;
        msg += "]";
      }
      setError(msg);
      break;
    }

    outAccessToken = doc["access_token"] | std::string("");
    if (outAccessToken.empty()) {
      setError("Authenticate: token response missing access_token");
      break;
    }
    ok = true;
  } while (false);

  mbedtls_pk_free(&pk);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
  return ok;
}

}  // namespace GoogleJwtAuth
