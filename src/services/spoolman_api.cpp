#include "spoolman_api.h"
#include "http_progress.h"
#include "tag_uid.h"
#include "services/tls_config.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <string.h>

static bool hasBaseUrl(const char* base_url) {
  return base_url && strlen(base_url) > 4;
}

static bool beginHttpConfiguration(HTTPClient& http, const String& url, TlsTarget tls_target,
                                  WiFiClient& plain, WiFiClientSecure& secure) {
  if (url.startsWith("https://")) {
    if (!tlsConfigureClient(tls_target, secure)) return false;
    return http.begin(secure, url);
  }
  return http.begin(plain, url);
}

static int patchJson(const String& url, const String& body, uint32_t timeout_ms,
                    TlsTarget tls_target) {
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  if (!beginHttpConfiguration(http, url, tls_target, plain, secure)) return -1;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(timeout_ms);
  int code = http.PATCH(body);
  http.end();
  return code;
}

static int putJson(const String& url, const String& body, uint32_t timeout_ms,
                   TlsTarget tls_target) {
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  if (!beginHttpConfiguration(http, url, tls_target, plain, secure)) return -1;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(timeout_ms);
  int code = http.PUT(body);
  http.end();
  return code;
}

static int postJson(const String& url, const String& body, uint32_t timeout_ms,
                    TlsTarget tls_target) {
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  if (!beginHttpConfiguration(http, url, tls_target, plain, secure)) return -1;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(timeout_ms);
  int code = http.POST(body);
  http.end();
  return code;
}

static int postJsonDoc(const String& url, const String& body, JsonDocument& doc,
                        uint32_t timeout_ms, DeserializationError* out_err,
                        TlsTarget tls_target) {
  if (out_err) *out_err = DeserializationError::Ok;
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  if (!beginHttpConfiguration(http, url, tls_target, plain, secure)) return -1;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(timeout_ms);
  int code = http.POST(body);
  if (code <= 0) { http.end(); return code; }

  DeserializationError err = deserializeJson(doc, *http.getStreamPtr());
  http.end();
  if (out_err) *out_err = err;
  return code;
}

static int deleteReq(const String& url, uint32_t timeout_ms, TlsTarget tls_target) {
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  if (!beginHttpConfiguration(http, url, tls_target, plain, secure)) return -1;
  http.setTimeout(timeout_ms);
  int code = http.sendRequest("DELETE");
  http.end();
  return code;
}

static int getJson(const String& url, JsonDocument& doc, uint32_t timeout_ms,
                    JsonDocument* filter, DeserializationError* out_err,
                    TlsTarget tls_target) {
  if (out_err) *out_err = DeserializationError::Ok;
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  if (!beginHttpConfiguration(http, url, tls_target, plain, secure)) return -1;
  http.setTimeout(timeout_ms);
  int code = http.GET();
  if (code != 200) { http.end(); return code; }

  DeserializationError err = DeserializationError::Ok;
  if (httpProgressActive()) {
    HttpProgressStream ps(*http.getStreamPtr());
    err = filter ? deserializeJson(doc, ps, DeserializationOption::Filter(*filter))
                 : deserializeJson(doc, ps);
  } else {
    err = filter ? deserializeJson(doc, *http.getStreamPtr(), DeserializationOption::Filter(*filter))
                 : deserializeJson(doc, *http.getStreamPtr());
  }
  http.end();
  if (out_err) *out_err = err;
  return err ? -2 : 200;
}

int spoolmanGetJson(const char* base_url, const char* path, JsonDocument& doc,
                     uint32_t timeout_ms, JsonDocument* filter, DeserializationError* out_err) {
  if (!hasBaseUrl(base_url) || !path) {
    if (out_err) *out_err = DeserializationError::InvalidInput;
    return -1;
  }
  return getJson(String(base_url) + path, doc, timeout_ms, filter, out_err, TLS_TARGET_SPOOLMAN);
}

int spoolmanGetSpoolJson(const char* base_url, int spool_id, JsonDocument& doc,
                          uint32_t timeout_ms, DeserializationError* out_err) {
  if (spool_id <= 0) {
    if (out_err) *out_err = DeserializationError::InvalidInput;
    return -1;
  }
  return getJson(String(base_url) + "/api/v1/spool/" + spool_id, doc, timeout_ms, nullptr, out_err, TLS_TARGET_SPOOLMAN);
}

int spoolmanGetSpoolListJson(const char* base_url, bool allow_archived, JsonDocument& doc,
                              uint32_t timeout_ms, JsonDocument* filter, DeserializationError* out_err) {
  return spoolmanGetJson(base_url,
    allow_archived ? "/api/v1/spool?allow_archived=true" : "/api/v1/spool?allow_archived=false",
    doc, timeout_ms, filter, out_err);
}

static String urlEncode(const char* s) {
  String out;
  for (const char* p = s; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out += (char)c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

int spoolmanFindSpoolByExtraField(const char* base_url, const char* key, const char* value,
                                   JsonDocument& doc, uint32_t timeout_ms,
                                   JsonDocument* filter, DeserializationError* out_err) {
  if (!key || !key[0] || !value || !value[0]) return -1;
  String path = String("/api/v1/spool?allow_archived=false&extra.") + key + "=" + urlEncode(value);
  return spoolmanGetJson(base_url, path.c_str(), doc, timeout_ms, filter, out_err);
}

int spoolmanHasTagApi(const char* base_url, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url)) return -1;
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  const String url = String(base_url) + "/api/v1/tag/reader";
  if (!beginHttpConfiguration(http, url, TLS_TARGET_SPOOLMAN, plain, secure)) return -1;
  http.setTimeout(timeout_ms);
  int code = http.GET();
  http.end();
  return code;
}

int spoolmanTagScan(const char* base_url, const char* uid, const char* reader_id,
                     const char* reader_name, const char* format, JsonDocument& doc,
                     uint32_t timeout_ms, DeserializationError* out_err) {
  if (!hasBaseUrl(base_url) || !uid || !uid[0]) return -1;
  String body = String("{\"uid\":\"") + uid + "\"";
  if (reader_id && reader_id[0]) body += String(",\"reader_id\":\"") + reader_id + "\"";
  if (reader_name && reader_name[0]) body += String(",\"name\":\"") + reader_name + "\"";
  if (format && format[0]) body += String(",\"format\":\"") + format + "\"";
  body += "}";

  return postJsonDoc(String(base_url) + "/api/v1/tag/scan", body, doc, timeout_ms, out_err, TLS_TARGET_SPOOLMAN);
}

int spoolmanLinkTag(const char* base_url, int spool_id, const char* uid,
                     const char* format, int* out_conflict_spool_id,
                     uint32_t timeout_ms) {
  if (out_conflict_spool_id) *out_conflict_spool_id = 0;
  if (!hasBaseUrl(base_url) || spool_id <= 0 || !uid || !uid[0]) return -1;

  String body = String("{\"uid\":\"") + uid + "\"";
  if (format && format[0]) body += String(",\"format\":\"") + format + "\"";
  body += "}";

  JsonDocument doc;
  int code = postJsonDoc(String(base_url) + "/api/v1/spool/" + spool_id + "/tag", body, doc, timeout_ms, nullptr, TLS_TARGET_SPOOLMAN);
  if (code == 409 && out_conflict_spool_id) *out_conflict_spool_id = doc["spool_id"] | 0;
  return code;
}

int spoolmanUnlinkTag(const char* base_url, int spool_id, const char* uid,
                       uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || spool_id <= 0 || !uid || !uid[0]) return -1;
  return deleteReq(String(base_url) + "/api/v1/spool/" + spool_id + "/tag/" + urlEncode(uid), timeout_ms, TLS_TARGET_SPOOLMAN);
}

int spoolmanFindSpoolByNativeTag(const char* base_url, const char* uid,
                                  JsonDocument& doc, uint32_t timeout_ms,
                                  JsonDocument* filter, DeserializationError* out_err) {
  if (!hasBaseUrl(base_url) || !uid || !uid[0]) return -1;
  String path = String("/api/v1/spool?allow_archived=false&tag=") + urlEncode(uid);
  return spoolmanGetJson(base_url, path.c_str(), doc, timeout_ms, filter, out_err);
}

int spoolmanGetLocationsJson(const char* base_url, JsonDocument& doc,
                              uint32_t timeout_ms, DeserializationError* out_err) {
  return spoolmanGetJson(base_url, "/api/v1/location", doc, timeout_ms, nullptr, out_err);
}

int spoolmanGetSpoolFieldsJson(const char* base_url, JsonDocument& doc,
                                uint32_t timeout_ms, DeserializationError* out_err) {
  return spoolmanGetJson(base_url, "/api/v1/field/spool", doc, timeout_ms, nullptr, out_err);
}

int spoolmanGetHealthCode(const char* base_url, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url)) return -1;
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  const String url = String(base_url) + "/api/v1/health";
  if (!beginHttpConfiguration(http, url, TLS_TARGET_SPOOLMAN, plain, secure)) return -1;
  http.setTimeout(timeout_ms);
  int code = http.GET();
  http.end();
  return code;
}

bool spoolmanIsReachable(const char* base_url, uint32_t timeout_ms) {
  return spoolmanGetHealthCode(base_url, timeout_ms) == 200;
}

bool spoolmanGetVersion(const char* base_url, char* out_version, size_t out_size, uint32_t timeout_ms) {
  if (out_version && out_size > 0) out_version[0] = '\0';
  if (!hasBaseUrl(base_url) || !out_version || out_size == 0) return false;

  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  const String url = String(base_url) + "/api/v1/info";
  if (!beginHttpConfiguration(http, url, TLS_TARGET_SPOOLMAN, plain, secure)) return false;
  http.setTimeout(timeout_ms);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) return false;

  strncpy(out_version, doc["version"] | "?", out_size - 1);
  out_version[out_size - 1] = '\0';
  return true;
}

int spoolmanCountActiveSpools(const char* base_url, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url)) return -1;

  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  const String url = String(base_url) + "/api/v1/spool?allow_archived=false&limit=1";
  if (!beginHttpConfiguration(http, url, TLS_TARGET_SPOOLMAN, plain, secure)) return -1;
  http.setTimeout(timeout_ms);
  const char* collect[] = { "x-total-count" };
  http.collectHeaders(collect, 1);
  if (http.GET() != 200) { http.end(); return -1; }

  String total = http.header("x-total-count");
  http.end();
  if (total.length() == 0) return -1;
  return total.toInt();
}

int spoolmanCreateSpool(const char* base_url, int filament_id, float initial_weight,
                         float spool_weight, float remaining_weight, int* out_spool_id, uint32_t timeout_ms) {
  if (out_spool_id) *out_spool_id = 0;
  if (!hasBaseUrl(base_url) || filament_id <= 0) return -1;

  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  const String url = String(base_url) + "/api/v1/spool";
  if (!beginHttpConfiguration(http, url, TLS_TARGET_SPOOLMAN, plain, secure)) return -1;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(timeout_ms);

  char body[256];
  snprintf(body, sizeof(body),
    "{\"filament_id\":%d,\"initial_weight\":%.0f,\"spool_weight\":%.0f,\"remaining_weight\":%.0f}",
    filament_id, initial_weight, spool_weight, remaining_weight);
  int code = http.POST(body);
  if ((code == 200 || code == 201) && out_spool_id) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      *out_spool_id = doc["id"] | 0;
    }
  }
  http.end();
  return code;
}

int spoolmanCreateSpoolField(const char* base_url, const char* field_name, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || !field_name || !field_name[0]) return -1;
  JsonDocument body;
  body["name"] = field_name;
  body["field_type"] = "text";
  body["default_value"] = "\"\"";
  String payload;
  serializeJson(body, payload);
  return postJson(String(base_url) + "/api/v1/field/spool/" + urlEncode(field_name), payload, timeout_ms, TLS_TARGET_SPOOLMAN);
}

static String jsonQuoted(const char* value) {
  JsonDocument inner;
  inner.set(value ? value : "");
  String out;
  serializeJson(inner, out);
  return out;
}

int spoolmanPatchExtraField(const char* base_url, int spool_id, const char* key,
                             const char* value, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || spool_id <= 0 || !key || !key[0] || !value) return -1;
  JsonDocument body;
  body["extra"][key] = jsonQuoted(value);
  String payload;
  serializeJson(body, payload);
  return patchJson(String(base_url) + "/api/v1/spool/" + spool_id, payload, timeout_ms, TLS_TARGET_SPOOLMAN);
}

int spoolmanPatchSpoolRemaining(const char* base_url, int spool_id, float remaining, const char* last_used_iso, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || spool_id <= 0) return -1;
  char body[128];
  if (last_used_iso && last_used_iso[0]) {
    snprintf(body, sizeof(body), "{\"remaining_weight\": %.0f, \"last_used\": \"%s\"}", remaining, last_used_iso);
  } else {
    snprintf(body, sizeof(body), "{\"remaining_weight\": %.0f}", remaining);
  }
  return patchJson(String(base_url) + "/api/v1/spool/" + spool_id, String(body), timeout_ms, TLS_TARGET_SPOOLMAN);
}

int spoolmanReactivateSpool(const char* base_url, int spool_id, float remaining,
                             uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || spool_id <= 0) return -1;
  char body[80];
  snprintf(body, sizeof(body), "{\"archived\": false, \"remaining_weight\": %.0f}", remaining);
  return patchJson(String(base_url) + "/api/v1/spool/" + spool_id, String(body), timeout_ms, TLS_TARGET_SPOOLMAN);
}

int spoolmanMeasureSpool(const char* base_url, int spool_id, float gross_weight,
                          uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || spool_id <= 0) return -1;
  if (gross_weight < 0.0f) return -1;
  char body[48];
  snprintf(body, sizeof(body), "{\"weight\": %.0f}", gross_weight);
  return putJson(String(base_url) + "/api/v1/spool/" + spool_id + "/measure", String(body), timeout_ms, TLS_TARGET_SPOOLMAN);
}

int spoolmanPatchInitialWeight(const char* base_url, int spool_id, float initial_weight, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || spool_id <= 0) return -1;
  char body[80];
  snprintf(body, sizeof(body), "{\"initial_weight\": %.0f, \"remaining_weight\": %.0f}", initial_weight, initial_weight);
  return patchJson(String(base_url) + "/api/v1/spool/" + spool_id, String(body), timeout_ms, TLS_TARGET_SPOOLMAN);
}

int spoolmanPatchArchiveSpool(const char* base_url, int spool_id, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || spool_id <= 0) return -1;
  return patchJson(String(base_url) + "/api/v1/spool/" + spool_id, "{\"remaining_weight\": 0.0, \"archived\": true}", timeout_ms, TLS_TARGET_SPOOLMAN);
}

int spoolmanPatchSpoolWeight(const char* base_url, int spool_id, float spool_weight, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || spool_id <= 0) return -1;
  char body[64];
  snprintf(body, sizeof(body), "{\"spool_weight\": %.0f}", spool_weight);
  return patchJson(String(base_url) + "/api/v1/spool/" + spool_id, String(body), timeout_ms, TLS_TARGET_SPOOLMAN);
}

int spoolmanPatchFilamentSpoolWeight(const char* base_url, int filament_id, float spool_weight, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || filament_id <= 0) return -1;
  char body[64];
  snprintf(body, sizeof(body), "{\"spool_weight\": %.0f}", spool_weight);
  return patchJson(String(base_url) + "/api/v1/filament/" + filament_id, String(body), timeout_ms, TLS_TARGET_SPOOLMAN);
}

int spoolmanPatchVendorEmptySpoolWeight(const char* base_url, int vendor_id, float spool_weight, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || vendor_id <= 0) return -1;
  char body[64];
  snprintf(body, sizeof(body), "{\"empty_spool_weight\": %.0f}", spool_weight);
  return patchJson(String(base_url) + "/api/v1/vendor/" + vendor_id, String(body), timeout_ms, TLS_TARGET_SPOOLMAN);
}

int spoolmanPatchSpoolLocation(const char* base_url, int spool_id, const char* location_name, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || spool_id <= 0) return -1;
  if (!location_name || !location_name[0]) {
    return patchJson(String(base_url) + "/api/v1/spool/" + spool_id, "{\"location\":null}", timeout_ms, TLS_TARGET_SPOOLMAN);
  }
  JsonDocument body;
  body["location"] = location_name;
  String payload;
  serializeJson(body, payload);
  return patchJson(String(base_url) + "/api/v1/spool/" + spool_id, payload, timeout_ms, TLS_TARGET_SPOOLMAN);
}

int spoolmanPatchSpoolLastDried(const char* base_url, int spool_id, const char* iso_datetime, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || spool_id <= 0 || !iso_datetime || !iso_datetime[0]) return -1;
  JsonDocument body;
  body["extra"]["last_dried"] = jsonQuoted(iso_datetime);
  String payload;
  serializeJson(body, payload);
  return patchJson(String(base_url) + "/api/v1/spool/" + spool_id, payload, timeout_ms, TLS_TARGET_SPOOLMAN);
}
























































































































