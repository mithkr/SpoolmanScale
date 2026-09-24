# HTTPS backend support implementation plan and status

## Scope

This document records the intended implementation for HTTPS connections from
SpoolmanScale to BamBuddy and Spoolman, including private CA certificates used
by reverse proxies such as Synology DSM.

The feature covers outbound connections only:

```text
SpoolmanScale --HTTPS--> BamBuddy
SpoolmanScale --HTTPS--> Spoolman
```

It does not make the SpoolmanScale configuration web server itself HTTPS.

## Intended behavior

- Existing `http://` backend configurations continue to work.
- `https://hostname` backend URLs are accepted and retained.
- BamBuddy and Spoolman may use different CA certificates.
- Publicly trusted certificates use the ESP-IDF certificate bundle.
- Private CA certificates are uploaded through the authenticated web UI.
- Custom certificates are stored on the SD card:
  - `/certs/bambuddy-ca.pem`
  - `/certs/spoolman-ca.pem`
- Certificate verification remains enabled by default.
- Insecure TLS must never be enabled automatically.
- AMS requests continue to use BamBuddy; HTTPS secures the Scale-to-BamBuddy
  connection and does not change the AMS assignment architecture.

## Required implementation areas

### 1. TLS configuration service

Files:

- `src/services/tls_config.h`
- `src/services/tls_config.cpp`

Responsibilities:

- Store, delete, and load CA certificates.
- Enforce a certificate-size limit.
- Validate PEM certificate markers and preferably parse the certificate with
  mbedTLS.
- Configure `WiFiClientSecure` with a custom CA or the public certificate
  bundle.
- Keep insecure mode disabled by default.
- Avoid logging certificate contents or credentials.

### 2. Shared secure HTTP setup

All outbound BamBuddy and Spoolman HTTP methods must select the correct client:

- `WiFiClient` for HTTP.
- `WiFiClientSecure` for HTTPS.

The secure client must remain alive for the entire `HTTPClient` request. The
same setup must be used for GET, POST, PATCH, PUT, and DELETE requests, including
JSON streaming and progress-stream paths.

### 3. Backend URL handling

Backend settings must preserve the URL scheme. Existing legacy values without a
scheme must continue to mean HTTP.

Accepted examples:

```text
192.168.1.20:8000
http://192.168.1.20:8000
https://bambuddy.example.local
https://spoolman.example.local
```

For TLS hostname validation, users should use a hostname contained in the
certificate SAN rather than an IP address unless the certificate explicitly
contains that IP.

### 4. BamBuddy integration

All BamBuddy requests must use the BamBuddy TLS target, including:

- `/api/v1/settings/spoolman`
- `/api/v1/printers/...`
- `/api/v1/inventory/...`
- `/api/v1/spoolman/inventory/...`
- `/api/v1/spoolbuddy/...`

The direct Spoolman side-channel used for `extra.last_dried` must use the
Spoolman TLS target, not the BamBuddy target.

### 5. Spoolman integration

All native Spoolman requests must use the Spoolman TLS target, including health,
version, inventory, native tag, and write endpoints.

### 6. Web UI and routes

The Backend page must expose:

- HTTPS-capable backend URL entry.
- BamBuddy CA upload and status.
- Spoolman CA upload and status.
- CA deletion.
- Clear TLS connection-test errors.

The upload must be authenticated with `GATE_CONFIG`, size-limited, validated,
written to a temporary file, and atomically installed only after validation.

Recommended API shape:

```text
POST   /api/tls/ca?target=bambuddy
POST   /api/tls/ca?target=spoolman
DELETE /api/tls/ca?target=bambuddy
DELETE /api/tls/ca?target=spoolman
```

The CA controls must be added to the existing page route registration and must
not replace the page's existing backend, credentials, settings, and host-test
routes.

## Validation checklist

Before opening a pull request:

- Build with PlatformIO environment `wt32-sc01-plus`.
- Verify existing HTTP Spoolman operation.
- Verify existing HTTP BamBuddy operation.
- Verify public-CA HTTPS operation.
- Verify private Synology CA upload and HTTPS operation.
- Verify separate BamBuddy and Spoolman CA files.
- Verify wrong CA, expired certificate, and hostname mismatch failures.
- Verify AMS status and assignment through HTTPS BamBuddy.
- Verify direct HTTPS Spoolman `last_dried` access.
- Confirm no API key or certificate contents appear in logs.
- Confirm the generated firmware image is produced by a successful build.

## Current branch status

The branch contains the initial TLS service and subsequent experimental HTTPS
changes. It has **not yet been verified as buildable**. Before using this branch
for a pull request or flashing a device, inspect and repair the integration so
that existing source files and route registrations are preserved. In
particular, avoid replacing complete `backend.cpp`, `spoolman_api.cpp`,
`bambuddy_api.cpp`, or `page_backend.cpp` files with partial implementations.

The authoritative baseline before the experimental integration is commit:

```text
83d3fd55302cb5836f222bc559d815e78e1b2f4f
```

A local build should be run with:

```bash
pio run -e wt32-sc01-plus
```
