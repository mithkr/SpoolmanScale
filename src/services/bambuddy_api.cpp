#include "bambuddy_api.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

#include "hardware/sd_logger.h"
#include "services/device_name.h"
#include "services/http_progress.h"
#include "services/tag_uid.h"
#include "services/tls_config.h"
#include "services/wifi_manager.h"
#include "services/time_service.h"
#include "services/user_options.h"

namespace {
struct SpiRamAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override {
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!ptr) ptr = malloc(size);
    return ptr;
  }
  void deallocate(void* pointer) override { heap_caps_free(pointer); }
  void* reallocate(void* ptr, size_t new_size) override {
    void* p = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
    if (!p) p = realloc(ptr, new_size);
    return p;
  }
};
}  // namespace

#define BB_DEVICE_BASE  "/api/v1/spoolbuddy"

static BbInventoryMode s_mode = BB_INV_LOCAL;
static char s_spoolman_url[96] = "";

static bool hasBaseUrl(const char* base_url) {
  return base_url && strlen(base_url) > 7;
}

static void addKey(HTTPClient& http, const char* api_key) {
  if (api_key && api_key[0]) http.addHeader("X-API-Key", api_key);
}

static bool beginHttpConfiguration(HTTPClient& http, const char* url, TlsTarget target,
                                  WiFiClient& plain, WiFiClientSecure& secure) {
  if (strncasecmp(url, "https://", 8) == 0) {
    if (!tlsConfigureClient(target, secure)) return false;
    return http.begin(secure, url);
  }
  return http.begin(plain, url);
}

static int getJson(const char* url, const char* api_key, JsonDocument& doc,
                   uint32_t timeout_ms, DeserializationError* out_err,
                   JsonDocument* filter, TlsTarget target) {
  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  if (!beginHttpConfiguration(http, url, target, plain, secure)) return -1;
  http.setTimeout(timeout_ms);
  addKey(http, api_key);

  int code = http.GET();
  if (code != 200) { http.end(); return code; }

  DeserializationError err = DeserializationError::Ok;
  if (httpProgressActive()) {
    HttpProgressStream ps(http.getStream());
    err = filter ? deserializeJson(doc, ps, DeserializationOption::Filter(*filter))
                 : deserializeJson(doc, ps);
  } else {
    err = filter ? deserializeJson(doc, http.getStream(), DeserializationOption::Filter(*filter))
                 : deserializeJson(doc, http.getStream());
  }
  http.end();

  if (out_err) *out_err = err;
  if (err) {
    logSDf("BamBuddy: JSON parse failed (%s) on %s", err.c_str(), url);
    return -2;
  }
  return 200;
}

static int sendJson(const char* method, const char* url, const char* api_key,
                    const String& body, uint32_t timeout_ms, String* out_body,
                    TlsTarget target) {
  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  if (!beginHttpConfiguration(http, url, target, plain, secure)) return -1;
  http.setTimeout(timeout_ms);
  http.addHeader("Content-Type", "application/json");
  addKey(http, api_key);

  int code = http.sendRequest(method, (uint8_t*)body.c_str(), body.length());
  if (code >= 200 && code < 300) {
    if (out_body) *out_body = http.getString();
    http.end();
    return 200;
  }

  if (code > 0) {
    String err = http.getString();
    if (err.length() > 120) err = err.substring(0, 120);
    logSDf("BamBuddy: %s %s -> %d %s", method, url, code, err.c_str());
  } else {
    logSDf("BamBuddy: %s %s -> transport error %d", method, url, code);
  }
  http.end();
  return code;
}

int bbDetectInventoryMode(const char* base_url, const char* api_key,
                          uint32_t timeout_ms) {
  s_mode = BB_INV_LOCAL;
  s_spoolman_url[0] = '\0';
  if (!hasBaseUrl(base_url)) return -1;

  char url[160];
  snprintf(url, sizeof(url), "%s/api/v1/settings/spoolman", base_url);

  JsonDocument doc;
  int code = getJson(url, api_key, doc, timeout_ms, nullptr, nullptr, TLS_TARGET_BAMBUDDY);
  if (code != 200) {
    logSDf("BamBuddy: inventory mode unknown (HTTP %d), assuming local", code);
    return code;
  }

  const char* enabled = doc["spoolman_enabled"] | "false";
  if (strcasecmp(enabled, "true") == 0) {
    s_mode = BB_INV_SPOOLMAN;
    const char* url_s = doc["spoolman_url"] | "";
    strncpy(s_spoolman_url, url_s, sizeof(s_spoolman_url) - 1);
    s_spoolman_url[sizeof(s_spoolman_url) - 1] = '\0';
    size_t n = strlen(s_spoolman_url);
    while (n > 0 && s_spoolman_url[n - 1] == '/') s_spoolman_url[--n] = '\0';
  }

  static bool seen = false;
  static BbInventoryMode last = BB_INV_LOCAL;
  if (!seen || last != s_mode) {
    logSDf("BamBuddy: inventory mode %s%s%s",
           s_mode == BB_INV_SPOOLMAN ? "Spoolman" : "local",
           s_spoolman_url[0] ? " via " : "",
           s_spoolman_url[0] ? s_spoolman_url : "");
    seen = true;
    last = s_mode;
  }
  return 200;
}

BbInventoryMode bbInventoryMode() { return s_mode; }
const char* bbInventoryBase() { return (s_mode == BB_INV_SPOOLMAN) ? "/api/v1/spoolman/inventory" : "/api/v1/inventory"; }
const char* bbSpoolmanUrl() { return s_spoolman_url; }
const char* bbDeviceId() { return wifiManagerDeviceId(); }

int bbGetHealthCode(const char* base_url, const char* api_key, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url)) return -1;
  char url[160];
  snprintf(url, sizeof(url), "%s/api/v1/updates/version", base_url);
  {
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    if (!beginHttpConfiguration(http, url, TLS_TARGET_BAMBUDDY, plain, secure)) return -1;
    http.setTimeout(timeout_ms);
    int code = http.GET();
    http.end();
    if (code != 200) return code;
  }

  snprintf(url, sizeof(url), "%s/api/v1/system/info", base_url);
  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  if (!beginHttpConfiguration(http, url, TLS_TARGET_BAMBUDDY, plain, secure)) return -1;
  http.setTimeout(timeout_ms);
  addKey(http, api_key);
  int code = http.GET();
  http.end();
  return code;
}

bool bbGetVersion(const char* base_url, const char* api_key,
                   char* out_version, size_t out_size, uint32_t timeout_ms) {
  if (!out_version || out_size == 0) return false;
  out_version[0] = '\0';
  if (!hasBaseUrl(base_url)) return false;

  char url[160];
  snprintf(url, sizeof(url), "%s/api/v1/updates/version", base_url);

  JsonDocument doc;
  if (getJson(url, api_key, doc, timeout_ms, nullptr, nullptr, TLS_TARGET_BAMBUDDY) != 200) return false;

  const char* v = doc["version"] | "";
  if (!v[0]) return false;
  strncpy(out_version, v, out_size - 1);
  out_version[out_size - 1] = '\0';
  return true;
}

int bbUpdateSpoolWeight(const char* base_url, const char* api_key,
                         int spool_id, float gross_grams, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || spool_id <= 0) return -1;
  if (gross_grams < 0.0f) gross_grams = 0.0f;

  char url[160];
  snprintf(url, sizeof(url), "%s" BB_DEVICE_BASE "/scale/update-spool-weight", base_url);
  JsonDocument body;
  body["spool_id"] = spool_id;
  body["weight_grams"] = roundGrams(gross_grams);

  String out;
  serializeJson(body, out);
  return sendJson("POST", url, api_key, out, timeout_ms, nullptr, TLS_TARGET_BAMBUDDY);
}

int bbLinkTag(const char* base_url, const char* api_key, int spool_id,
               const char* tag_uid, const char* tray_uuid, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || spool_id <= 0) return -1;
  const bool has_tray = tray_uuid && tray_uuid[0];
  const bool has_uid  = tag_uid   && tag_uid[0];
  if (!has_tray && !has_uid) return -1;

  char url[224];
  JsonDocument body;

  if (s_mode == BB_INV_SPOOLMAN) {
    snprintf(url, sizeof(url), "%s/api/v1/spoolman/inventory/spools/%d/tag", base_url, spool_id);
    if (has_tray) body["tray_uuid"] = tray_uuid;
    else          body["tag_uid"]   = tag_uid;
  } else {
    snprintf(url, sizeof(url), "%s/api/v1/inventory/spools/%d/link-tag", base_url, spool_id);
    if (has_tray) body["tray_uuid"] = tray_uuid;
    if (has_uid)  body["tag_uid"]   = tag_uid;
    body["tag_type"]    = has_tray ? "bambulab" : "generic";
    body["data_origin"] = "nfc_link";
  }

  String out;
  serializeJson(body, out);
  return sendJson("PATCH", url, api_key, out, timeout_ms, nullptr, TLS_TARGET_BAMBUDDY);
}

int bbUnlinkTag(const char* base_url, const char* api_key, int spool_id,
                 uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || spool_id <= 0) return -1;

  char url[192];
  snprintf(url, sizeof(url), "%s%s/spools/%d", base_url, bbInventoryBase(), spool_id);
  return sendJson("PATCH", url, api_key, String("{\"tag_uid\":null,\"tray_uuid\":null}"), timeout_ms, nullptr, TLS_TARGET_BAMBUDDY);
}

bool bbGetDriedFromSpoolman(int spool_id, char* out_iso, size_t out_size,
                             uint32_t timeout_ms) {
  if (out_iso && out_size) out_iso[0] = '\0';
  if (!out_iso || out_size < 11 || spool_id <= 0) return false;
  if (!s_spoolman_url[0]) return false;

  char url[192];
  snprintf(url, sizeof(url), "%s/api/v1/spool/%d", s_spoolman_url, spool_id);
  JsonDocument filter;
  filter["extra"]["last_dried"] = true;
  JsonDocument doc;
  if (getJson(url, nullptr, doc, timeout_ms, nullptr, &filter, TLS_TARGET_SPOOLMAN) != 200) return false;

  String v = doc["extra"]["last_dried"].as<String>();
  v.replace("\"", "");
  v.trim();
  if (v.length() < 10) return false;

  strncpy(out_iso, v.c_str(), out_size - 1);
  out_iso[out_size - 1] = '\0';
  return true;
}

int bbRegisterDevice(const char* base_url, const char* api_key, const char* ip,
                      const char* firmware, int32_t tare_offset,
                      float calibration_factor, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url)) return -1;
  JsonDocument body;
  body["device_id"]          = bbDeviceId();
  body["hostname"]           = deviceLabel();
  body["ip_address"]         = (ip && ip[0]) ? ip : "0.0.0.0";
  body["firmware_version"]   = firmware ? firmware : "";
  body["has_nfc"]            = true;
  body["has_scale"]          = g_scale_fitted;
  body["tare_offset"]        = tare_offset;
  body["calibration_factor"] = calibration_factor;
  body["nfc_reader_type"]    = "PN532";
  body["nfc_connection"]     = "i2c";
  body["has_backlight"]      = true;

  char url[160];
  snprintf(url, sizeof(url), "%s" BB_DEVICE_BASE "/devices/register", base_url);
  String out;
  serializeJson(body, out);
  int code = sendJson("POST", url, api_key, out, timeout_ms, nullptr, TLS_TARGET_BAMBUDDY);
  if (code == 200) logSDf("BamBuddy: registered as %s", bbDeviceId());
  return code;
}

int bbHeartbeat(const char* base_url, const char* api_key, bool nfc_ok,
                 bool scale_ok, uint32_t uptime_s, const char* ip,
                 const char* firmware, char* out_command, size_t out_command_size,
                 int* out_write_spool_id, uint32_t timeout_ms) {
  if (out_command && out_command_size > 0) out_command[0] = '\0';
  if (out_write_spool_id) *out_write_spool_id = 0;
  if (!hasBaseUrl(base_url)) return -1;

  JsonDocument body;
  body["nfc_ok"] = nfc_ok;
  body["scale_ok"] = scale_ok;
  body["uptime_s"] = uptime_s;
  body["ip_address"] = (ip && ip[0]) ? ip : "0.0.0.0";
  body["firmware_version"] = firmware ? firmware : "";
  body["nfc_reader_type"] = "PN532";
  body["nfc_connection"] = "i2c";

  char url[192];
  snprintf(url, sizeof(url), "%s" BB_DEVICE_BASE "/devices/%s/heartbeat", base_url, bbDeviceId());

  String out, resp;
  serializeJson(body, out);
  int code = sendJson("POST", url, api_key, out, timeout_ms, &resp, TLS_TARGET_BAMBUDDY);
  if (code != 200) return code;

  JsonDocument doc;
  if (deserializeJson(doc, resp)) return -2;

  const char* cmd = doc["pending_command"] | "";
  if (out_command && out_command_size > 0 && cmd[0]) {
    strncpy(out_command, cmd, out_command_size - 1);
    out_command[out_command_size - 1] = '\0';
  }
  if (out_write_spool_id) {
    *out_write_spool_id = doc["pending_write_payload"]["spool_id"] | 0;
  }
  return 200;
}

int bbTagScanned(const char* base_url, const char* api_key, const char* tag_uid,
                  const char* tray_uuid, int* out_spool_id, uint32_t timeout_ms) {
  if (out_spool_id) *out_spool_id = 0;
  if (!hasBaseUrl(base_url)) return -1;
  const bool has_tray = tray_uuid && tray_uuid[0];
  const bool has_uid  = tag_uid   && tag_uid[0];
  if (!has_tray && !has_uid) return -1;

  JsonDocument body;
  body["device_id"] = bbDeviceId();
  body["tag_uid"] = has_uid ? tag_uid : tray_uuid;
  if (has_tray) body["tray_uuid"] = tray_uuid;

  char url[160];
  snprintf(url, sizeof(url), "%s" BB_DEVICE_BASE "/nfc/tag-scanned", base_url);

  String out, resp;
  serializeJson(body, out);
  int code = sendJson("POST", url, api_key, out, timeout_ms, &resp, TLS_TARGET_BAMBUDDY);
  if (code != 200) return code;

  JsonDocument doc;
  if (deserializeJson(doc, resp)) return -2;
  if (out_spool_id) *out_spool_id = doc["spool_id"] | 0;
  return 200;
}

int bbTagRemoved(const char* base_url, const char* api_key, const char* tag_uid,
                  uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || !tag_uid || !tag_uid[0]) return -1;
  JsonDocument body;
  body["device_id"] = bbDeviceId();
  body["tag_uid"] = tag_uid;

  char url[160];
  snprintf(url, sizeof(url), "%s" BB_DEVICE_BASE "/nfc/tag-removed", base_url);

  String out;
  serializeJson(body, out);
  return sendJson("POST", url, api_key, out, timeout_ms, nullptr, TLS_TARGET_BAMBUDDY);
}

int bbScaleReading(const char* base_url, const char* api_key, float grams,
                    bool stable, int32_t raw_adc, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url)) return -1;

  JsonDocument body;
  body["device_id"] = bbDeviceId();
  body["weight_grams"] = grams;
  body["stable"] = stable;
  body["raw_adc"] = raw_adc;

  char url[160];
  snprintf(url, sizeof(url), "%s" BB_DEVICE_BASE "/scale/reading", base_url);
  String out;
  serializeJson(body, out);
  return sendJson("POST", url, api_key, out, timeout_ms, nullptr, TLS_TARGET_BAMBUDDY);
}

int bbSetTare(const char* base_url, const char* api_key, int32_t tare_offset,
               uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url)) return -1;

  JsonDocument body;
  body["tare_offset"] = tare_offset;
  char url[192];
  snprintf(url, sizeof(url), "%s" BB_DEVICE_BASE "/devices/%s/calibration/set-tare", base_url, bbDeviceId());
  String out;
  serializeJson(body, out);
  return sendJson("POST", url, api_key, out, timeout_ms, nullptr, TLS_TARGET_BAMBUDDY);
}

int bbCommandResult(const char* base_url, const char* api_key,
                    const char* command, bool success, const char* message,
                    uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || !command || !command[0]) return -1;
  JsonDocument body;
  body["command"] = command;
  body["success"] = success;
  if (message && message[0]) body["message"] = message;

  char url[192];
  snprintf(url, sizeof(url), "%s" BB_DEVICE_BASE "/devices/%s/system/command-result", base_url, bbDeviceId());
  String out;
  serializeJson(body, out);
  return sendJson("POST", url, api_key, out, timeout_ms, nullptr, TLS_TARGET_BAMBUDDY);
}

int bbDiagnosticResult(const char* base_url, const char* api_key,
                        const char* diagnostic, bool success, const char* output,
                        int exit_code, uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || !diagnostic || !diagnostic[0]) return -1;
  JsonDocument body;
  body["diagnostic"] = diagnostic;
  body["success"] = success;
  body["output"] = output ? output : "";
  body["exit_code"] = exit_code;

  char url[192];
  snprintf(url, sizeof(url), "%s" BB_DEVICE_BASE "/diagnostics/%s/result", base_url, bbDeviceId());
  String out;
  serializeJson(body, out);
  return sendJson("POST", url, api_key, out, timeout_ms, nullptr, TLS_TARGET_BAMBUDDY);
}

int bbWriteTagResult(const char* base_url, const char* api_key, int spool_id,
                    const char* tag_uid, bool success, const char* message,
                    uint32_t timeout_ms) {
  if (!hasBaseUrl(base_url) || spool_id <= 0) return -1;
  JsonDocument body;
  body["device_id"] = bbDeviceId();
  body["spool_id"] = spool_id;
  body["tag_uid"] = (tag_uid && strlen(tag_uid) >= 8) ? tag_uid : "00000000";
  body["success"] = success;
  if (message && message[0]) body["message"] = message;

  char url[160];
  snprintf(url, sizeof(url), "%s" BB_DEVICE_BASE "/nfc/write-result", base_url);
  String out;
  serializeJson(body, out);
  return sendJson("POST", url, api_key, out, timeout_ms, nullptr, TLS_TARGET_BAMBUDDY);
}





















































































