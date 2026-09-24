#include "backend.h"

#include <Arduino.h>
#include <string.h>
#include <strings.h>

#include "app/app_state.h"
#include "hardware/sd_logger.h"
#include "services/app_settings.h"
#include "services/bambuddy_api.h"
#include "services/prefs_store.h"
#include "services/tls_config.h"

// NVS keys. Kept short, NVS limits key length to 15 characters.
#define NVS_BACKEND_MODE    "backend_mode"
#define NVS_FILAMAN_KEY     "filaman_key"
#define NVS_FILAMAN_DEVICE  "filaman_dev"
#define NVS_FILAMAN_HOST    "filaman_host"
#define NVS_BAMBUDDY_KEY    "bb_key"
#define NVS_BAMBUDDY_HOST   "bb_host"

static BackendMode s_mode = BACKEND_SPOOLMAN;
static char s_api_key[80]      = "";
static char s_device_token[80] = "";
static char s_filaman_host[128] = "";
static char s_filaman_base[192] = "";
static char s_filaman_scheme[8] = "http";
// BamBuddy keys look like "bb_" plus 43 base64url characters, 46 in total.
static char s_bambuddy_key[80]  = "";
static char s_bambuddy_host[128] = "";
static char s_bambuddy_base[192] = "";
static char s_bambuddy_scheme[8] = "http";

static void backendHostToBase(const char* host, const char* scheme,
                             char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  out[0] = '\0';
  if (!host || !host[0]) return;
  const char* scheme_name = (scheme && scheme[0]) ? scheme : "http";
  snprintf(out, out_size, "%s://%s", scheme_name, host);
}

static void rebuildFilamanBase() {
  backendHostToBase(s_filaman_host, s_filaman_scheme, s_filaman_base, sizeof(s_filaman_base));
}

static void rebuildBamBuddyBase() {
  backendHostToBase(s_bambuddy_host, s_bambuddy_scheme, s_bambuddy_base, sizeof(s_bambuddy_base));
}

static void backendParseScheme(const char* raw, char* scheme, size_t scheme_size,
                              char* host, size_t host_size) {
  if (!scheme || scheme_size == 0 || !host || host_size == 0) return;
  scheme[0] = '\0';
  host[0] = '\0';
  if (!raw || !raw[0]) return;

  const char* p = raw;
  while (*p == ' ' || *p == '\t') p++;
  if (strncasecmp(p, "https://", 8) == 0) {
    snprintf(scheme, scheme_size, "https");
    p += 8;
  } else if (strncasecmp(p, "http://", 7) == 0) {
    snprintf(scheme, scheme_size, "http");
    p += 7;
  } else {
    snprintf(scheme, scheme_size, "http");
  }

  // keep any path that was provided, but reject leading/trailing garbage.
  size_t n = 0;
  while (p[n] && n + 1 < host_size) {
    const char c = p[n];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == ':' ||
                    c == '-' || c == '_' || c == '/' || c == '?';
    if (!ok) break;
    n++;
  }
  memcpy(host, p, n);
  host[n] = '\0';
  while (n > 0 && host[n - 1] == '/') host[--n] = '\0';
}

void backendLoadSettings() {
  uint8_t raw = prefsGetUChar(NVS_BACKEND_MODE, BACKEND_SPOOLMAN);
  s_mode = (raw == BACKEND_FILAMAN)  ? BACKEND_FILAMAN
         : (raw == BACKEND_BAMBUDDY) ? BACKEND_BAMBUDDY
                                     : BACKEND_SPOOLMAN;

  String key = prefsGetString(NVS_FILAMAN_KEY, "");
  strncpy(s_api_key, key.c_str(), sizeof(s_api_key) - 1);
  s_api_key[sizeof(s_api_key) - 1] = '\0';

  String dev = prefsGetString(NVS_FILAMAN_DEVICE, "");
  strncpy(s_device_token, dev.c_str(), sizeof(s_device_token) - 1);
  s_device_token[sizeof(s_device_token) - 1] = '\0';

  String host = prefsGetString(NVS_FILAMAN_HOST, "");
  backendParseScheme(host.c_str(), s_filaman_scheme, sizeof(s_filaman_scheme),
                    s_filaman_host, sizeof(s_filaman_host));
  rebuildFilamanBase();

  String bb_key = prefsGetString(NVS_BAMBUDDY_KEY, "");
  strncpy(s_bambuddy_key, bb_key.c_str(), sizeof(s_bambuddy_key) - 1);
  s_bambuddy_key[sizeof(s_bambuddy_key) - 1] = '\0';

  String bb_host = prefsGetString(NVS_BAMBUDDY_HOST, "");
  backendParseScheme(bb_host.c_str(), s_bambuddy_scheme, sizeof(s_bambuddy_scheme),
                    s_bambuddy_host, sizeof(s_bambuddy_host));
  rebuildBamBuddyBase();

  char line[160];
  backendStatusLine(line, sizeof(line));
  logSDf("Backend: %s", line);
  Serial.printf("Backend: %s\n", line);
}

BackendMode backendMode() { return s_mode; }
bool backendIsFilaMan()   { return s_mode == BACKEND_FILAMAN; }
bool backendIsBamBuddy()  { return s_mode == BACKEND_BAMBUDDY; }

void backendSetMode(BackendMode mode) {
  s_mode = (mode == BACKEND_FILAMAN)  ? BACKEND_FILAMAN
         : (mode == BACKEND_BAMBUDDY) ? BACKEND_BAMBUDDY
                                      : BACKEND_SPOOLMAN;
  prefsPutUChar(NVS_BACKEND_MODE, (uint8_t)s_mode);
  logSDf("Backend: mode -> %s", backendName());
}

const char* backendBaseUrl() {
  switch (s_mode) {
    case BACKEND_FILAMAN:  return s_filaman_base;
    case BACKEND_BAMBUDDY: return s_bambuddy_base;
    default:               return cfg_spoolman_base;
  }
}

const char* backendHost() {
  switch (s_mode) {
    case BACKEND_FILAMAN:  return s_filaman_host;
    case BACKEND_BAMBUDDY: return s_bambuddy_host;
    default:               return cfg_spoolman_ip;
  }
}

size_t backendCleanHost(const char* in, char* out, size_t out_size) {
  if (!in || !out || out_size == 0) return 0;
  while (*in == ' ' || *in == '\t') in++;
  const char* p = in;

  if (strncasecmp(p, "https://", 8) == 0) p += 8;
  else if (strncasecmp(p, "http://", 7) == 0) p += 7;

  size_t n = 0;
  for (; *p && n + 1 < out_size; p++) {
    const char c = *p;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == ':' ||
                    c == '-' || c == '_' || c == '/' || c == '?';
    if (ok) out[n++] = c;
  }
  while (n > 0 && (out[n-1] == '/' || out[n-1] == ' ' || out[n-1] == '\t')) n--;
  out[n] = '\0';
  return n;
}

void backendSetHost(const char* host) {
  if (!host) return;
  char clean[192];
  backendCleanHost(host, clean, sizeof(clean));
  char scheme[8] = "http";
  char normalized[192];
  backendParseScheme(clean, scheme, sizeof(scheme), normalized, sizeof(normalized));
  const char* value = normalized[0] ? normalized : clean;

  switch (s_mode) {
    case BACKEND_FILAMAN:
      strncpy(s_filaman_host, value, sizeof(s_filaman_host) - 1);
      s_filaman_host[sizeof(s_filaman_host) - 1] = '\0';
      strncpy(s_filaman_scheme, scheme, sizeof(s_filaman_scheme) - 1);
      s_filaman_scheme[sizeof(s_filaman_scheme) - 1] = '\0';
      rebuildFilamanBase();
      prefsPutString(NVS_FILAMAN_HOST, s_filaman_base);
      logSDf("Backend: FilaMan host -> %s", s_filaman_base);
      return;
    case BACKEND_BAMBUDDY:
      strncpy(s_bambuddy_host, value, sizeof(s_bambuddy_host) - 1);
      s_bambuddy_host[sizeof(s_bambuddy_host) - 1] = '\0';
      strncpy(s_bambuddy_scheme, scheme, sizeof(s_bambuddy_scheme) - 1);
      s_bambuddy_scheme[sizeof(s_bambuddy_scheme) - 1] = '\0';
      rebuildBamBuddyBase();
      prefsPutString(NVS_BAMBUDDY_HOST, s_bambuddy_base);
      logSDf("Backend: BamBuddy host -> %s", s_bambuddy_base);
      return;
    default:
      saveSpoolmanIP(value);
      return;
  }
}

const char* filamanApiKey()      { return s_api_key; }
const char* filamanDeviceToken() { return s_device_token; }

void filamanSetApiKey(const char* key) {
  if (!key) return;
  strncpy(s_api_key, key, sizeof(s_api_key) - 1);
  s_api_key[sizeof(s_api_key) - 1] = '\0';
  prefsPutString(NVS_FILAMAN_KEY, s_api_key);
  logSDf("Backend: FilaMan API key %s", s_api_key[0] ? "stored" : "cleared");
}

void filamanSetDeviceToken(const char* token) {
  if (!token) return;
  strncpy(s_device_token, token, sizeof(s_device_token) - 1);
  s_device_token[sizeof(s_device_token) - 1] = '\0';
  prefsPutString(NVS_FILAMAN_DEVICE, s_device_token);
  logSDf("Backend: FilaMan device token %s", s_device_token[0] ? "stored" : "cleared");
}

const char* bambuddyApiKey() { return s_bambuddy_key; }

void bambuddySetApiKey(const char* key) {
  if (!key) return;
  strncpy(s_bambuddy_key, key, sizeof(s_bambuddy_key) - 1);
  s_bambuddy_key[sizeof(s_bambuddy_key) - 1] = '\0';
  prefsPutString(NVS_BAMBUDDY_KEY, s_bambuddy_key);
  logSDf("Backend: BamBuddy API key %s", s_bambuddy_key[0] ? "stored" : "cleared");
}

const char* backendModeName(BackendMode mode) {
  switch (mode) {
    case BACKEND_FILAMAN:  return "FilaMan";
    case BACKEND_BAMBUDDY: return "BamBuddy";
    default:               return "Spoolman";
  }
}

const char* backendName() { return backendModeName(s_mode); }

const char* backendBadge() {
  switch (s_mode) {
    case BACKEND_FILAMAN:  return "FLM";
    case BACKEND_BAMBUDDY: return (bbInventoryMode() == BB_INV_SPOOLMAN) ? "BBS" : "BBY";
    default:               return "SPM";
  }
}

void backendCaption(char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  snprintf(out, out_size, "%s", backendName());
}

void backendStatusLine(char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  const char* host = backendHost();

  if (backendIsFilaMan()) {
    snprintf(out, out_size, "%s | host=%s | key=%s | device=%s | configured=%s",
      backendName(),
      host[0] ? host : "-",
      s_api_key[0] ? "set" : "empty",
      s_device_token[0] ? "set" : "empty",
      backendIsConfigured() ? "yes" : "no");
  } else if (backendIsBamBuddy()) {
    snprintf(out, out_size, "%s | host=%s | key=%s | configured=%s",
      backendName(),
      host[0] ? host : "-",
      s_bambuddy_key[0] ? "set" : "empty",
      backendIsConfigured() ? "yes" : "no");
  } else {
    snprintf(out, out_size, "%s | host=%s | configured=%s",
      backendName(),
      host[0] ? host : "-",
      backendIsConfigured() ? "yes" : "no");
  }
}

void backendText(const char* src, char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  out[0] = '\0';
  if (!src) return;

  if (s_mode == BACKEND_SPOOLMAN) {
    strncpy(out, src, out_size - 1);
    out[out_size - 1] = '\0';
    return;
  }

  static const char  kNeedle[] = "Spoolman";
  static const size_t kNeedleLen = sizeof(kNeedle) - 1;
  const char* name = backendName();
  const size_t name_len = strlen(name);

  size_t o = 0;
  for (const char* p = src; *p && o < out_size - 1; ) {
    if (strncmp(p, kNeedle, kNeedleLen) == 0 &&
        strncmp(p + kNeedleLen, "Scale", 5) != 0) {
      size_t room = out_size - 1 - o;
      size_t n = (name_len < room) ? name_len : room;
      memcpy(out + o, name, n);
      o += n;
      p += kNeedleLen;
      continue;
    }
    out[o++] = *p++;
  }
  out[o] = '\0';
}

bool backendIsConfigured() {
  if (strlen(backendBaseUrl()) <= 7) return false;
  if (!backendIsFilaMan()) return true;
  return s_api_key[0] != '\0' && s_device_token[0] != '\0';
}
















































































































































































































































n



















