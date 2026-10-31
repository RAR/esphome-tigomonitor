#pragma once

// Serves the same web UI as the ESP32 (components/tigo_server/web/app.html)
// and the /api/* routes it reads, in the firmware's JSON shapes — see
// site/screenshots/fixtures.mjs for the reference payloads. Routes the Pi
// has no data for yet (CCA, cloud, history database) are not served; the UI
// treats those as unavailable.
//
// Settings changes are written to settings.json and flagged for the main
// loop to apply, so MQTT is only ever reconfigured from one thread.

#include "cca_ble.h"
#include "cca_http.h"
#include "cloud.h"
#include "history.h"
#include "monitor.h"
#include "bus_master.h"
#include "settings.h"
#include "tls.h"
#include "topology.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace httplib {
class Server;
}

namespace tigod {

struct HttpContext {
  const Monitor &monitor;
  std::string web_dir;        // holds app.html
  std::string settings_path;
  std::string version;
  std::function<Settings()> current_settings;
  std::function<bool()> mqtt_connected;
  std::function<std::string()> mqtt_error;
  std::atomic<bool> &settings_changed;
  std::atomic<bool> &restart_requested;
  Monitor &mutable_monitor;
  Topology &topology;
  History &history;
  Cloud &cloud;
  CcaBle &cca;
  CcaHttp &cca_http;
  std::function<MasterStatus()> master_status;  // inactive when listening
  TlsCerts *tls = nullptr;  // HTTPS with these; null: plain HTTP
  bool tls_from_args = false;  // set by --tls-cert/--tls-key, not the web UI
};

class HttpServer {
 public:
  explicit HttpServer(HttpContext ctx);
  ~HttpServer();
  // Listens on port in a background thread, or on fallback (when nonzero)
  // if port is taken. Returns the port it got, or 0 if it got neither.
  int start(int port, int fallback = 0);
  void stop();

 private:
  void routes_();
  std::vector<std::string> health_problems_() const;
  void new_server_();
  HttpContext ctx_;
  int64_t started_unix_ms_;
  std::unique_ptr<httplib::Server> svr_;
  std::unique_ptr<std::thread> thread_;
  int port_ = 0;
  std::mutex settings_mu_;  // serialises settings.json writes
};

}  // namespace tigod
