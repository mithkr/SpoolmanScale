#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>

// TLS targets have independent CA files because BamBuddy may proxy a
// Spoolman instance through a different reverse proxy.
enum TlsTarget : uint8_t {
  TLS_TARGET_BAMBUDDY = 0,
  TLS_TARGET_SPOOLMAN = 1,
};

#define TLS_CA_MAX_BYTES 8192

bool tlsHasCustomCa(TlsTarget target);
bool tlsStoreCa(TlsTarget target, const uint8_t* data, size_t length);
bool tlsDeleteCa(TlsTarget target);
size_t tlsCaSize(TlsTarget target);

// Configures a secure client. Custom CA files take precedence over the ESP-IDF
// public certificate bundle. Returns false when the configured CA is invalid.
bool tlsConfigureClient(TlsTarget target, WiFiClientSecure& client);

// Explicitly intended for diagnostics only. It is disabled by default and is
// never enabled automatically after a certificate failure.
bool tlsInsecure(TlsTarget target);
void tlsSetInsecure(TlsTarget target, bool enabled);

const char* tlsLastError();
