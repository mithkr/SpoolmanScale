// This route accepts a PEM-encoded certificate in plain text and stores it
// under /certs/ for either BamBuddy or Spoolman.
static bool uploadTlsCa(WebServer& srv, TlsTarget target) {
  if (!webRequire(srv, GATE_CONFIG, T(STR_W_NAV_BACKEND))) return false;
  String body = srv.arg("plain");
  body.trim();
  if (body.length() == 0) {
    srv.send(400, "text/plain", "Empty CA certificate");
    return true;
  }

  String pem = body;
  if (pem.length() > TLS_CA_MAX_BYTES) {
    srv.send(413, "text/plain", "CA certificate too large");
    return true;
  }

  const uint8_t* data = (const uint8_t*)pem.c_str();
  if (!tlsStoreCa(target, data, pem.length())) {
    srv.send(400, "text/plain", tlsLastError());
    return true;
  }
  srv.send(200, "text/plain", "OK");
  return true;
}

static bool deleteTlsCa(WebServer& srv, TlsTarget target) {
  if (!webRequire(srv, GATE_CONFIG, T(STR_W_NAV_BACKEND))) return false;
  tlsDeleteCa(target);
  srv.send(200, "text/plain", "OK");
  return true;
}

// Accept https:// in backend host inputs and use the configured CA files for the
// HTTPS requests that go to BamBuddy and Spoolman.
static void routes(WebServer &srv) {
  srv.on("/api/host", HTTP_POST, [&srv]() {
    if (!webRequire(srv, GATE_CONFIG, T(STR_W_NAV_BACKEND))) return;
    String host = srv.arg("plain");
    host.trim();
    if (host.length() == 0) {
      srv.send(400, "text/plain", T(STR_W_HOST_EMPTY));
      return;
    }
    char clean[192];
    if (backendCleanHost(host.c_str(), clean, sizeof(clean)) == 0) {
      srv.send(400, "text/plain", T(STR_W_HOST_EMPTY));
      return;
    }
    backendApplyHost(clean);
    logSDf("Web: %s host -> %s", backendName(), clean);
    if (webJobState() == WJS_DONE) webJobTake();
    if (webJobStart(WJ_HOST_TEST, nullptr, false)) {
      srv.send(202, "application/json", "{\"queued\":true}");
    } else {
      srv.send(200, "application/json",
               String("{\"queued\":false,\"msg\":\"") + jsonEsc(T(STR_W_HOST_SAVED_ONLY)) + "\"}");
    }
  });

  srv.on("/api/tls/ca", HTTP_POST, [&srv]() {
    String target = srv.arg("target");
    target.trim();
    if (target == "bambuddy") {
      uploadTlsCa(srv, TLS_TARGET_BAMBUDDY);
      return;
    }
    if (target == "spoolman") {
      uploadTlsCa(srv, TLS_TARGET_SPOOLMAN);
      return;
    }
    srv.send(400, "text/plain", "Missing target");
  });

  srv.on("/api/tls/ca", HTTP_DELETE, [&srv]() {
    String target = srv.arg("target");
    target.trim();
    if (target == "bambuddy") { deleteTlsCa(srv, TLS_TARGET_BAMBUDDY); return; }
    if (target == "spoolman") { deleteTlsCa(srv, TLS_TARGET_SPOOLMAN); return; }
    srv.send(400, "text/plain", "Missing target");
  });
}
