#include "tls_config.h"

#include <SD.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>
#include <string.h>

#include "hardware/sd_logger.h"
#include "services/prefs_store.h"

namespace {
constexpr const char* kDir = "/certs";
constexpr const char* kFiles[] = { "/certs/bambuddy-ca.pem", "/certs/spoolman-ca.pem" };
constexpr const char* kTmp[] = { "/certs/bambuddy-ca.tmp", "/certs/spoolman-ca.tmp" };
constexpr const char* kPrefs[] = { "tls_bb_insec", "tls_sm_insec" };
String s_ca[2];
String s_error;

bool validTarget(TlsTarget t) { return t == TLS_TARGET_BAMBUDDY || t == TLS_TARGET_SPOOLMAN; }

bool pemLooksLikeCertificate(const uint8_t* data, size_t length) {
  if (!data || length == 0 || length > TLS_CA_MAX_BYTES) return false;
  String pem;
  pem.reserve(length + 1);
  for (size_t i = 0; i < length; ++i) pem += (char)data[i];
  return pem.indexOf("-----BEGIN CERTIFICATE-----") >= 0 &&
         pem.indexOf("-----END CERTIFICATE-----") >= 0;
}

void fail(const char* message) { s_error = message ? message : "TLS error"; }
}

bool tlsHasCustomCa(TlsTarget target) {
  if (!validTarget(target)) return false;
  const uint8_t i = (uint8_t)target;
  if (s_ca[i].length()) return true;
  if (!SD.exists(kFiles[i])) return false;
  File f = SD.open(kFiles[i], FILE_READ);
  if (!f) return false;
  const size_t n = f.size();
  f.close();
  return n > 0 && n <= TLS_CA_MAX_BYTES;
}

bool tlsStoreCa(TlsTarget target, const uint8_t* data, size_t length) {
  if (!validTarget(target) || !pemLooksLikeCertificate(data, length)) {
    fail("invalid PEM certificate");
    return false;
  }
  if (!SD.exists(kDir) && !SD.mkdir(kDir)) {
    fail("cannot create certificate directory");
    return false;
  }
  SD.remove(kTmp[(uint8_t)target]);
  File f = SD.open(kTmp[(uint8_t)target], FILE_WRITE);
  if (!f) { fail("cannot open certificate file"); return false; }
  const size_t written = f.write(data, length);
  f.close();
  if (written != length) {
    SD.remove(kTmp[(uint8_t)target]);
    fail("certificate write failed");
    return false;
  }
  SD.remove(kFiles[(uint8_t)target]);
  if (!SD.rename(kTmp[(uint8_t)target], kFiles[(uint8_t)target])) {
    SD.remove(kTmp[(uint8_t)target]);
    fail("certificate install failed");
    return false;
  }
  s_ca[(uint8_t)target] = String((const char*)data).substring(0, length);
  s_error = "";
  logSDf("TLS: installed custom CA for %s (%u bytes)",
         target == TLS_TARGET_BAMBUDDY ? "BamBuddy" : "Spoolman", (unsigned)length);
  return true;
}

bool tlsDeleteCa(TlsTarget target) {
  if (!validTarget(target)) return false;
  const uint8_t i = (uint8_t)target;
  s_ca[i] = "";
  SD.remove(kFiles[i]);
  SD.remove(kTmp[i]);
  return true;
}

size_t tlsCaSize(TlsTarget target) {
  if (!validTarget(target)) return 0;
  const uint8_t i = (uint8_t)target;
  if (s_ca[i].length()) return s_ca[i].length();
  if (!SD.exists(kFiles[i])) return 0;
  File f = SD.open(kFiles[i], FILE_READ);
  if (!f) return 0;
  const size_t n = f.size();
  f.close();
  return n;
}

bool tlsConfigureClient(TlsTarget target, WiFiClientSecure& client) {
  if (!validTarget(target)) { fail("invalid TLS target"); return false; }
  const uint8_t i = (uint8_t)target;
  if (tlsInsecure(target)) {
    client.setInsecure();
    return true;
  }
  if (s_ca[i].isEmpty() && SD.exists(kFiles[i])) {
    File f = SD.open(kFiles[i], FILE_READ);
    if (f && f.size() <= TLS_CA_MAX_BYTES) {
      s_ca[i].reserve(f.size() + 1);
      while (f.available()) s_ca[i] += (char)f.read();
    }
    f.close();
  }
  if (!s_ca[i].isEmpty()) {
    client.setCACert(s_ca[i].c_str());
    return true;
  }
  // The certificate bundle is enabled by the project's ESP-IDF build. Keep
  // the symbol declaration local so GitHub's updater can continue to expose
  // its own small trust helper without creating a dependency cycle.
  extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
  client.setCACertBundle(x509_crt_bundle_start);
  return true;
}

bool tlsInsecure(TlsTarget target) {
  if (!validTarget(target)) return false;
  return prefsGetBool(kPrefs[(uint8_t)target], false);
}

void tlsSetInsecure(TlsTarget target, bool enabled) {
  if (!validTarget(target)) return;
  prefsPutBool(kPrefs[(uint8_t)target], enabled);
  logSDf("TLS: insecure mode %s for %s", enabled ? "enabled" : "disabled",
         target == TLS_TARGET_BAMBUDDY ? "BamBuddy" : "Spoolman");
}

const char* tlsLastError() { return s_error.c_str(); }
