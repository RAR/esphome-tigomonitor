// tigomonitord — listens to a Tigo RS485 bus and publishes to Home Assistant.
//
//   tigomonitord [--state-dir DIR] [--serial DEV] [--replay FILE [--speed X]]
//                [--capture FILE] [--verbose]

#include "log.h"
#include "monitor.h"
#include "cloud.h"
#include "history.h"
#include "http_server.h"
#include "mqtt_publisher.h"
#include "serial_port.h"
#include "settings.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef TIGOD_VERSION
#define TIGOD_VERSION "dev"
#endif

namespace tigod {
bool g_verbose = false;
}

using namespace tigod;

static std::atomic<bool> g_stop{false};

static void on_signal(int) { g_stop = true; }

static void usage() {
  std::fprintf(stderr,
               "usage: tigomonitord [options]\n"
               "  --state-dir DIR   settings and state (default /var/lib/tigomonitor)\n"
               "  --web-dir DIR     where app.html is (default /usr/share/tigomonitor/web)\n"
               "  --port N          web UI port (default 80; 0 turns it off)\n"
               "  --serial DEV      serial port, overriding settings.json\n"
               "  --replay FILE     read bus bytes from a capture instead of a port\n"
               "  --speed X         replay speed (1 = bus speed, 0 = as fast as possible)\n"
               "  --capture FILE    also append every byte read to FILE\n"
               "  --verbose         debug logging\n"
               "  --version\n");
}

static std::string hostname() {
  char buf[256] = {0};
  if (gethostname(buf, sizeof(buf) - 1) != 0 || buf[0] == '\0') return "tigomonitor";
  std::string h(buf);
  return h.substr(0, h.find('.'));
}

int main(int argc, char **argv) {
  std::string state_dir = "/var/lib/tigomonitor";
  std::string web_dir = "/usr/share/tigomonitor/web";
  int port = 80;
  std::string serial_override, replay, capture;
  double speed = 1.0;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--state-dir") state_dir = next();
    else if (a == "--web-dir") web_dir = next();
    else if (a == "--port") port = std::atoi(next().c_str());
    else if (a == "--serial") serial_override = next();
    else if (a == "--replay") replay = next();
    else if (a == "--speed") speed = std::atof(next().c_str());
    else if (a == "--capture") capture = next();
    else if (a == "--verbose" || a == "-v") g_verbose = true;
    else if (a == "--version") {
      std::printf("tigomonitord %s\n", TIGOD_VERSION);
      return 0;
    } else {
      usage();
      return a == "--help" || a == "-h" ? 0 : 2;
    }
  }
  setvbuf(stderr, nullptr, _IOLBF, 0);

  ::mkdir(state_dir.c_str(), 0755);
  const std::string settings_path = state_dir + "/settings.json";
  Settings settings = load_settings(settings_path);
  if (settings.device_id.empty()) settings.device_id = hostname();
  if (!serial_override.empty()) settings.serial_port = serial_override;
  LOGI("tigomonitord %s, device id %s", TIGOD_VERSION, settings.device_id.c_str());

  Monitor monitor(state_dir + "/state.json", settings.power_calibration, static_cast<size_t>(settings.max_nodes));
  monitor.load();

  MqttPublisher mqtt(monitor, TIGOD_VERSION);
  mqtt.configure(settings);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  std::unique_ptr<std::ofstream> capture_out;
  if (!capture.empty()) {
    capture_out = std::make_unique<std::ofstream>(capture, std::ios::binary | std::ios::app);
    if (!*capture_out) {
      LOGE("cannot open capture file %s", capture.c_str());
      return 1;
    }
    LOGI("capturing raw bus bytes to %s", capture.c_str());
  }
  auto on_data = [&](const char *data, size_t n) {
    if (capture_out) capture_out->write(data, static_cast<std::streamsize>(n)).flush();
    monitor.feed(data, n);
  };

  std::mutex settings_mu;  // settings is read by the web server's threads
  std::atomic<bool> input_done{false};
  std::atomic<bool> input_stop{false};
  auto start_input = [&]() {
    input_stop = false;
    input_done = false;
    std::string dev = settings.serial_port;
    int baud = settings.baud_rate;
    return std::thread([&, dev, baud] {
      if (!replay.empty()) run_replay(replay, speed, input_stop, on_data);
      else run_serial(dev, baud, input_stop, on_data);
      input_done = true;
    });
  };
  std::thread input = start_input();

  std::atomic<bool> settings_changed{false};
  std::atomic<bool> restart_requested{false};
  History history(state_dir + "/history.db");
  history.open();
  Cloud cloud(state_dir + "/cloud.json");
  cloud.load();
  Topology topology(state_dir + "/topology.json");
  topology.load();
  HttpServer web(HttpContext{
      monitor, web_dir, settings_path, TIGOD_VERSION,
      [&] {
        std::lock_guard<std::mutex> lock(settings_mu);
        return settings;
      },
      [&] { return mqtt.connected(); }, [&] { return mqtt.last_error(); }, settings_changed, restart_requested,
      monitor, topology, history, cloud});
  if (port > 0 && !web.start(port)) return 1;

  auto last_publish = std::chrono::steady_clock::now() - std::chrono::hours(1);
  // Snapshots land on wall-clock multiples of the interval (:00, :05, ...),
  // so every panel's rows share timestamps.
  int64_t next_snapshot = 0;
  double energy_at_snapshot = monitor.energy_kwh();
  int64_t last_prune = 0;
  BusStats last_stats{};
  auto last_stats_log = std::chrono::steady_clock::now();
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (restart_requested) {
      LOGI("restart requested from the web UI");
      break;
    }
    if (settings_changed.exchange(false)) {
      Settings fresh = load_settings(settings_path);
      if (fresh.device_id.empty()) fresh.device_id = settings.device_id;
      bool new_port = fresh.serial_port != settings.serial_port || fresh.baud_rate != settings.baud_rate;
      {
        std::lock_guard<std::mutex> lock(settings_mu);
        settings = fresh;
      }
      LOGI("settings changed; applying");
      monitor.set_power_calibration(settings.power_calibration);
      mqtt.configure(settings);
      if (new_port && replay.empty()) {
        input_stop = true;
        input.join();
        input = start_input();
      }
    }
    monitor.tick();
    auto now = std::chrono::steady_clock::now();
    if (now - last_publish >= std::chrono::seconds(settings.publish_interval_s)) {
      mqtt.publish();
      last_publish = now;
    }
    int64_t wall = static_cast<int64_t>(time(nullptr));
    int64_t step = settings.history_interval_min * 60;
    if (wall > 1577836800 /* 2020: the clock is set */) {
      if (next_snapshot == 0) next_snapshot = (wall / step + 1) * step;
      if (wall >= next_snapshot) {
        auto ago = std::chrono::steady_clock::now();
        std::vector<std::pair<std::string, double>> panel_power;
        for (const auto &p : monitor.panels()) {
          if (p.barcode.empty()) continue;
          // Offline is 0 W, as on the ESP32 once a panel goes stale.
          panel_power.emplace_back(p.barcode, ago - p.updated < Monitor::ONLINE_WINDOW ? p.reading.power_out : 0.0);
        }
        double energy = monitor.energy_kwh();
        history.record(next_snapshot, monitor.total_power(), energy - energy_at_snapshot, panel_power);
        energy_at_snapshot = energy;
        next_snapshot = (wall / step + 1) * step;
        if (wall - last_prune > 86400) {
          history.prune(settings.history_days);
          last_prune = wall;
        }
      }
    }
    if (now - last_stats_log >= std::chrono::minutes(5)) {
      BusStats s = monitor.stats();
      LOGI("bus: %u frames (%u bad checksum), %u readings, %u Frame 27 in the last 5 min; %.0f W",
           s.frames - last_stats.frames, s.bad_checksums - last_stats.bad_checksums,
           s.power_packets - last_stats.power_packets, s.frame27 - last_stats.frame27, monitor.total_power());
      last_stats = s;
      last_stats_log = now;
    }
    // A finished replay leaves the state for one last publish, then exits.
    if (input_done && !replay.empty()) {
      mqtt.publish();
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      break;
    }
  }
  g_stop = true;
  input_stop = true;
  input.join();
  web.stop();
  monitor.save();
  BusStats s = monitor.stats();
  LOGI("stopping: %u frames, %u bad checksums, %u readings, %zu panels, %zu nodes", s.frames, s.bad_checksums,
       s.power_packets, monitor.panels().size(), monitor.nodes().size());
  return 0;
}
