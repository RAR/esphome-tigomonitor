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
#include "cloud.h"
#include "history.h"
#include "monitor.h"
#include "settings.h"
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
};

class HttpServer {
 public:
  explicit HttpServer(HttpContext ctx);
  ~HttpServer();
  // Listens on port in a background thread. Returns false if it can't bind.
  bool start(int port);
  void stop();

 private:
  void routes_();
  HttpContext ctx_;
  std::unique_ptr<httplib::Server> svr_;
  std::unique_ptr<std::thread> thread_;
  std::mutex settings_mu_;  // serialises settings.json writes
};

}  // namespace tigod
