#pragma once

#include <string>

/**
 * Mints Google API access tokens from a service-account private key using the
 * OAuth 2.0 JWT-bearer grant (RFC 7523):
 *   1. build a JWT {iss=client_email, scope, aud=token_uri, iat, exp},
 *   2. sign it RS256 with the service account's RSA private key (mbedTLS),
 *   3. POST it to the token endpoint and read back a short-lived access token.
 *
 * Unlike the OAuth device-code flow, the JWT-bearer grant *does* permit the
 * drive.readonly scope, and access is limited to whatever the service account
 * has been shared — so sharing only the books folder scopes the device to
 * exactly that folder. Requires a correct wall clock (NTP) for iat/exp.
 */
namespace GoogleJwtAuth {

// scope is space-delimited; for book sync use Drive read-only.
constexpr const char* SCOPE_DRIVE_READONLY = "https://www.googleapis.com/auth/drive.readonly";

// Sign a JWT for the service account and exchange it for an access token.
// privateKeyPem is the PEM text (real newlines, not \n escapes). Returns true
// and fills outAccessToken on success; on failure returns false and sets a
// human-readable reason in lastError().
bool getAccessToken(const std::string& clientEmail, const std::string& privateKeyPem, const std::string& tokenUri,
                    const std::string& scope, std::string& outAccessToken);

// Detail for the most recent failure (for on-screen display + SD log).
const std::string& lastError();

}  // namespace GoogleJwtAuth
