# HTTPS/CA support foundation

This branch adds the TLS configuration service used by the HTTPS backend work.

- CA certificates are stored on the SD card under `/certs/`.
- BamBuddy and Spoolman have separate CA files.
- Certificates are written through a temporary file and installed only after a
  size and PEM-marker check succeeds.
- The ESP-IDF public certificate bundle is used when no private CA is present.
- Insecure TLS is opt-in and is never enabled automatically.

The HTTP request helpers and backend web upload routes still need to be wired
through this service before this branch is ready for production use.
