// tigomonitord — listens to a Tigo RS485 bus and publishes to Home Assistant.
//
//   tigomonitord [--state-dir DIR] [--serial DEV] [--replay FILE [--speed X]]
//                [--capture FILE] [--verbose]

#include "log.h"
#include "bus_master.h"
#include "discovery_policy.h"
#include "pv_store.h"
#include "discovery_store.h"
#include "table_store.h"
#include "tap_store.h"
#include "monitor.h"
#include "cca_ble.h"
#include "cca_http.h"
#include "cloud.h"
#include "history.h"
#include "housekeeping_store.h"
#include "http_server.h"
#include "mqtt_publisher.h"
#include "serial_port.h"
#include "settings.h"

#include <atomic>
#include <random>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <systemd/sd-daemon.h>
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
               "  --port N          web UI port, overriding settings.json (0 turns it off)\n"
               "  --serial DEV      serial port, overriding settings.json\n"
               "  --tls-cert FILE   HTTPS certificate (with its chain), overriding settings.json\n"
               "  --tls-key FILE    its private key\n"
               "  --watchdog S      without systemd's watchdog, exit if the main loop stops\n"
               "                    for S seconds, for Docker to restart it (default 60, 0 off)\n"
               "  --health-check P  check the daemon on port P, as Docker's HEALTHCHECK does\n"
               "  --replay FILE     read bus bytes from a capture instead of a port\n"
               "  --speed X         replay speed (1 = bus speed, 0 = as fast as possible)\n"
               "  --capture FILE    also append every byte read to FILE (raw, so it holds\n"
               "                    a TAP's network key if one is read or written)\n"
               "  --verbose         debug logging\n"
               "  --version\n");
}

static int64_t steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static std::string hostname() {
  char buf[256] = {0};
  if (gethostname(buf, sizeof(buf) - 1) != 0 || buf[0] == '\0') return "tigomonitor";
  std::string h(buf);
  return h.substr(0, h.find('.'));
}

// Said with a new optimizer: running the bus without the CCA's optimizer
// writes, it gets no #00w255, and a fresh one may then produce nothing.
static std::string new_optimizer_note(const Settings &s) {
  if (!runs_bus(s) || s.master_optimizer_writes) return "";
  return "; master_optimizer_writes is off, so it gets no w255 and may produce nothing until it's on";
}

// Running in a container: its MAC is the container's (from its bridge
// address, or new each start), not this machine's.
static bool in_container() {
  if (access("/.dockerenv", F_OK) == 0 || access("/run/.containerenv", F_OK) == 0) return true;
  if (FILE *f = fopen("/proc/1/cgroup", "r")) {
    char line[512];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f))
      found = strstr(line, "docker") || strstr(line, "containerd") || strstr(line, "kubepods") || strstr(line, "libpod");
    fclose(f);
    if (found) return true;
  }
  return false;
}

// What a new TAP's PAN is made from (pan_for takes its last two bytes): this
// machine's MAC on bare metal, as a CCA takes its serial; in a container, a
// random seed made once and kept in the state directory, so each install's
// differs and stays the same across restarts.
static esphome::tigo_monitor::master::Bytes pan_seed(const std::string &state_dir);

// This machine's MAC, from the interface the default route uses: what a new
// TAP's PAN is made from (a CCA uses its own serial). Empty if none is found.
static esphome::tigo_monitor::master::Bytes system_mac() {
  esphome::tigo_monitor::master::Bytes out;
  FILE *f = fopen("/proc/net/route", "r");
  if (!f) return out;
  char line[256], iface[64] = "";
  while (fgets(line, sizeof(line), f)) {
    char name[64];
    unsigned long dest = 1;
    if (sscanf(line, "%63s %lx", name, &dest) == 2 && dest == 0) {
      snprintf(iface, sizeof(iface), "%s", name);
      break;
    }
  }
  fclose(f);
  if (!iface[0]) return out;
  std::string path = std::string("/sys/class/net/") + iface + "/address";
  if (FILE *a = fopen(path.c_str(), "r")) {
    unsigned b[6];
    if (fscanf(a, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6)
      for (unsigned v : b) out.push_back(static_cast<uint8_t>(v));
    fclose(a);
  }
  return out;
}

static esphome::tigo_monitor::master::Bytes pan_seed(const std::string &state_dir) {
  if (!in_container()) {
    auto mac = system_mac();
    if (mac.size() >= 2) return mac;
  }
  const std::string path = state_dir + "/pan-seed";
  unsigned v = 0;
  if (FILE *f = fopen(path.c_str(), "r")) {
    if (fscanf(f, "%x", &v) != 1) v = 0;
    fclose(f);
  }
  if (!v || v >= 0xFFFF) {
    std::random_device rd;
    do v = rd() & 0xFFFF; while (!v || v == 0xFFFF);
    std::string tmp = path + ".tmp";
    if (FILE *f = fopen(tmp.c_str(), "w")) {
      fprintf(f, "%04X\n", v);
      fclose(f);
      rename(tmp.c_str(), path.c_str());
    }
  }
  return {static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
}

int main(int argc, char **argv) {
  std::string state_dir = "/var/lib/tigomonitor";
  std::string web_dir = "/usr/share/tigomonitor/web";
  int port = -1;  // -1: from settings.json
  std::string serial_override, replay, capture;
  std::string tls_cert_override, tls_key_override;
  double speed = 1.0;
  int watchdog_s = 60;
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
    else if (a == "--tls-cert") tls_cert_override = next();
    else if (a == "--tls-key") tls_key_override = next();
    else if (a == "--watchdog") watchdog_s = std::atoi(next().c_str());
    else if (a == "--health-check") return health_check(std::atoi(next().c_str()));
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
  struct stat settings_stat;
  const bool first_start = ::stat(settings_path.c_str(), &settings_stat) != 0;
  Settings settings = load_settings(settings_path);
  // A new install: a day of setup mode (settings.h), saved at once so a
  // restart doesn't start the day again.
  if (first_start) {
    settings.setup_open_until = static_cast<int64_t>(::time(nullptr)) + 24 * 3600;
    if (save_settings(settings_path, settings)) LOGI("settings: a new install; System setup needs no password for a day");
  }
  if (settings.device_id.empty()) settings.device_id = hostname();
  if (!serial_override.empty()) settings.serial_port = serial_override;
  // As the Docker compose file with certbot sets them: the certificate
  // comes with the deployment, not from the web UI.
  const bool tls_from_args = !tls_cert_override.empty() || !tls_key_override.empty();
  auto apply_tls_override = [&](Settings &s) {
    if (!tls_from_args) return;
    s.tls_cert = tls_cert_override;
    s.tls_key = tls_key_override;
  };
  apply_tls_override(settings);
  LOGI("tigomonitord %s, device id %s", TIGOD_VERSION, settings.device_id.c_str());

  Monitor monitor(state_dir + "/state.json", settings.power_calibration, static_cast<size_t>(settings.max_nodes));
  monitor.load();
  monitor.set_stale_timeout_min(settings.stale_timeout_min);
  monitor.set_night_mode_timeout_min(settings.night_mode_timeout_min);
  monitor.set_reset_at_midnight(settings.reset_at_midnight);
  monitor.set_new_optimizer_note(new_optimizer_note(settings));

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
  // What the CCA sends that the master copies (its 0x41s) is learned while
  // listening beside it; the master's own frames teach it nothing.
  HousekeepingStore housekeeping(state_dir + "/cca-0x41.json");
  housekeeping.load();
  // PV output off or on, as last asked or heard: each new bus master starts
  // from it, so a restart or a settings save never turns PV back on.
  PvStore pv(state_dir + "/pv.json");
  pv.load();
  // And the discovery switch, for the same reason.
  DiscoveryStore discovery(state_dir + "/discovery.json");
  // The tunnel sequence, carried from one master to the next and across
  // restarts, as the CCA's runs on unbroken. The TAP only echoes it (bench
  // and analysis, 2026-11-11); this just keeps us counting the CCA's way.
  const std::string seq_path = state_dir + "/tunnel-seq";
  uint8_t carried_seq = 0;
  if (FILE *f = fopen(seq_path.c_str(), "r")) {
    unsigned v = 0;
    if (fscanf(f, "%u", &v) == 1 && v < 0xFF) carried_seq = static_cast<uint8_t>(v);
    fclose(f);
  }
  auto save_seq = [&](uint8_t s) {
    std::string tmp = seq_path + ".tmp";
    if (FILE *f = fopen(tmp.c_str(), "w")) {
      fprintf(f, "%u\n", static_cast<unsigned>(s));
      fclose(f);
      rename(tmp.c_str(), seq_path.c_str());
    }
  };
  discovery.load();
  TableStore table_store(state_dir + "/table-remove.json");
  TapStore tap_store(state_dir + "/taps.json");
  // The TAP an optimizer (MAC, hex) last reported through, from the Monitor's
  // link stats: 0 if it never has, which a table change takes as the first.
  auto tap_of_mac = [&monitor](const std::string &mac) -> uint16_t {
    std::string addr;
    for (const Node &n : monitor.nodes())
      if (strcasecmp(n.long_address.c_str(), mac.c_str()) == 0) addr = n.addr;
    if (addr.empty()) return 0;
    auto links = monitor.links();
    auto it = links.links().find(addr);
    return it == links.links().end() ? 0 : it->second.tap;
  };
  // tap if the master runs it, else 0 (the first): a link heard before
  // gateway_id changed names a TAP that isn't polled.
  auto known_tap = [](const BusMaster &m, uint16_t tap) -> uint16_t {
    for (const auto &t : m.taps())
      if (t.id == tap) return tap;
    return 0;
  };
  // Every frame, named, and an alarm for one that reconfigures the TAP.
  BusLog bus_log(state_dir + "/bus-alarm.json", state_dir + "/versions.json");
  bus_log.load();
  auto on_data = [&](const char *data, size_t n) {
    if (capture_out) capture_out->write(data, static_cast<std::streamsize>(n)).flush();
    monitor.feed(data, n);
    bus_log.feed_rx(data, n);
  };
  auto on_listen = [&](const char *data, size_t n) {
    on_data(data, n);
    housekeeping.feed(data, n);
  };
  // What the bus master sends goes in the capture too, for reading back
  // what was said on the bench.
  auto on_sent = [&](const char *data, size_t n, uint32_t late_ms) {
    if (capture_out) capture_out->write(data, static_cast<std::streamsize>(n)).flush();
    bus_log.feed_tx(data, n, late_ms);
  };

  std::mutex settings_mu;  // settings is read by the web server's threads
  std::atomic<bool> input_done{false};
  std::atomic<bool> input_stop{false};
  // The master role has no input thread: the main loop ticks it below, so
  // nothing renews the TAP's discovery window if this loop ever stops.
  // bus_master is read by the web server's threads. The mutex guards the
  // pointer only: each side takes a copy and works on that, so the web UI and
  // the healthcheck never wait on a tick that is waiting on the bus.
  std::mutex master_mu;
  std::shared_ptr<BusMaster> bus_master;
  auto start_input = [&]() {
    input_stop = false;
    input_done = false;
    std::string dev = settings.serial_port;
    int baud = settings.baud_rate;
    if (replay.empty() && runs_bus(settings)) {
      auto m = std::make_unique<BusMaster>(on_data, on_sent, static_cast<uint16_t>(settings.gateway_id),
                                           settings.master_bench, settings.master_startup,
                                           settings.master_housekeeping, housekeeping.entries(), [&monitor] {
                                             // The optimizers heard in the last day, so one
                                             // removed from the roof drops out by itself.
                                             std::vector<uint16_t> tuids;
                                             int64_t since = (static_cast<int64_t>(time(nullptr)) - 86400) * 1000;
                                             for (const Panel &p : monitor.panels())
                                               if (p.updated_unix_ms >= since)
                                                 tuids.push_back(static_cast<uint16_t>(strtoul(p.addr.c_str(), nullptr, 16)));
                                             return tuids;
                                           },
                                           settings.master_queries, settings.master_optimizer_writes);
      // From reading the PV state to swapping the new master in, no request
      // may land: one taken by the old master would be lost, and the new one
      // would start from the state before it. The requests take this too.
      std::unique_lock<std::mutex> swap(master_mu);
      if (bus_master) carried_seq = bus_master->seq();  // the master this one replaces
      m->set_seq(carried_seq);
      // The TAPs saved by a herd; none, the one at gateway_id. With more
      // than one (saved, or after a herd), each TAP's 0x41s go to the
      // optimizers heard through it.
      auto saved_taps = tap_store.load();
      if (!saved_taps.empty()) m->set_taps(saved_taps);
      m->set_tap_targets([&monitor](uint16_t tap) {
        std::vector<uint16_t> tuids;
        int64_t since = static_cast<int64_t>(time(nullptr)) - 86400;
        auto links = monitor.links();
        for (const auto &kv : links.links())
          if (kv.second.tap == tap && kv.second.last_heard_s >= since)
            tuids.push_back(static_cast<uint16_t>(strtoul(kv.first.c_str(), nullptr, 16)));
        return tuids;
      });
      m->set_pv(pv.state(), [&pv](const PvState &s) { pv.save(s); }, pv.unreadable());
      m->set_discovery(discovery.renew(), [&discovery](bool renew) { discovery.save(renew); });
      m->set_read_tap_config(settings.master_read_tap_config);
      m->set_table_writes(settings.master_table_writes);
      // A herd's TAPs, saved so each gets its id back next time.
      m->set_adopt_taps(settings.master_adopt_taps, [&tap_store](const std::vector<BusMaster::TapEntry> &taps) {
        if (tap_store.save(taps)) LOGI("bus master: saved %zu TAP%s", taps.size(), taps.size() == 1 ? "" : "s");
      });
      // A replace's new optimizer, once it has joined, takes the old one's
      // place in the layout, as a CCA's cloud does.
      m->set_on_replaced([&monitor](const std::string &old_mac, const std::string &new_mac) {
        std::string why;
        if (monitor.take_place(old_mac, new_mac, why))
          LOGI("layout: the replacement took its place");
        else
          LOGW("layout: the replacement joined, but its place wasn't moved: %s", why.c_str());
        // The old one, its place handed on, leaves the Devices list.
        monitor.forget_optimizer(old_mac);
      });
      // A retired optimizer leaves the Devices list at once; one that drops
      // off the TAP any other way stays, tagged, so a failure shows.
      m->set_on_removed([&monitor](const std::string &mac) {
        if (monitor.forget_optimizer(mac)) LOGI("devices: forgot the retired optimizer");
      });
      {
        // A removal that stopped between its erase and its write back is
        // finished by this master, from the list saved before the erase.
        esphome::tigo_monitor::master::Bytes resume_mac;
        std::vector<esphome::tigo_monitor::master::TableRecord> resume;
        bool unfinished = table_store.load(resume_mac, resume);
        m->set_table_saved([&table_store](const esphome::tigo_monitor::master::Bytes &mac,
                                          const std::vector<esphome::tigo_monitor::master::TableRecord> *stay) {
          return table_store.save(mac, stay);
        }, resume_mac, unfinished ? &resume : nullptr);
      }
      // Standing by, it listens as the listen role does, learning the 0x41s
      // another master sends, and takes the latest of them when it takes over.
      if (settings.bus_role == "standby")
        m->set_standby(static_cast<uint32_t>(settings.standby_takeover_s), on_listen,
                       [&housekeeping] { return housekeeping.entries(); });
      if (dev.empty()) LOGW("bus master: no serial port chosen yet; pick the RS485 adapter under Tools");
      else m->open(dev, baud);
      bus_master = std::move(m);
      return std::thread();
    }
    {
      std::lock_guard<std::mutex> lock(master_mu);
      if (bus_master) carried_seq = bus_master->seq();
      bus_master.reset();
    }
    return std::thread([&, dev, baud] {
      if (!replay.empty()) run_replay(replay, speed, input_stop, on_listen);
      else run_serial(dev, baud, input_stop, on_listen);
      input_done = true;
    });
  };
  std::thread input = start_input();

  std::atomic<bool> settings_changed{false};
  std::atomic<bool> restart_requested{false};
  History history(state_dir + "/history.db");
  history.open();
  CcaBle cca(state_dir + "/cca.json");
  cca.start();
  Topology topology(state_dir + "/topology.json");
  topology.load();
  CcaHttp cca_http(monitor, topology);
  cca_http.start();
  // sync_cca_on_startup, for a CCA reached over HTTP.
  if (settings.sync_cca_on_startup && !settings.cca_ip.empty()) cca_http.request_refresh(settings.cca_ip);
  Cloud cloud(state_dir + "/cloud.json");
  cloud.load();
  // The bus master's status, for the web UI and Home Assistant alike.
  auto master_status = [&] {
    std::shared_ptr<BusMaster> bm;
    {
      std::lock_guard<std::mutex> lock(master_mu);
      bm = bus_master;
    }
    MasterStatus m = bm ? bm->status() : MasterStatus{};
    if (!bm) m.learned_41 = static_cast<uint32_t>(housekeeping.entries().size());
    return m;
  };
  mqtt.set_master_status(master_status);
  mqtt.set_tap_reconfigured([&bus_log] { return bus_log.alarm_active(); });
  mqtt.set_setting_changes([&bus_log] {
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
    return bus_log.changes_since(now_ms - 24 * 3600 * 1000LL);
  });
  // Under master_mu, so it can't fall between start_input's read of the
  // state and its swap. request_pv never waits on the bus.
  mqtt.set_pv_command([&](bool off) {
    std::lock_guard<std::mutex> lock(master_mu);
    return bus_master && bus_master->request_pv(off);
  });
  // HTTPS, if a certificate is set and loads. One that doesn't is no reason
  // to lock everyone out of the web UI they would fix it from. It is tried
  // again every 10 s, and once it loads the service restarts to serve it:
  // a certbot container issuing the first certificate takes a minute or
  // two, and starts alongside this one.
  std::unique_ptr<TlsCerts> tls, tls_waiting;
  if (!settings.tls_cert.empty() && !settings.tls_key.empty()) {
    tls = std::make_unique<TlsCerts>(settings.tls_cert, settings.tls_key);
    std::string err = tls->load();
    if (!err.empty()) {
      LOGE("web: HTTPS is off, serving plain HTTP until the certificate loads: %s", err.c_str());
      tls_waiting = std::move(tls);
    }
  }
  std::atomic<int64_t> loop_pass_ms{steady_ms()};
  HttpServer web(HttpContext{
      monitor, web_dir, settings_path, TIGOD_VERSION,
      [&] {
        std::lock_guard<std::mutex> lock(settings_mu);
        return settings;
      },
      [&] { return mqtt.connected(); }, [&] { return mqtt.last_error(); }, settings_changed, restart_requested,
      monitor, topology, history, cloud, cca, cca_http,
      master_status, tls.get(), tls_from_args, [&] { return steady_ms() - loop_pass_ms; },
      [&](bool off, std::string &why) {
        std::lock_guard<std::mutex> lock(master_mu);  // as the MQTT command's
        if (!bus_master) {
          why = "this monitor isn't running the bus";
          return 409;
        }
        if (!bus_master->request_pv(off)) {
          why = bus_master->status().fault == esphome::tigo_master::Fault::second_master
                    ? "stopped: another master is on the bus"
                    : "standing by: change it on the device running the bus";
          return 409;
        }
        return 200;
      },
      [&] { return pv.unreadable(); },
      [&] {
        std::shared_ptr<BusMaster> bm;
        {
          std::lock_guard<std::mutex> lock(master_mu);
          bm = bus_master;
        }
        return bm ? bm->setups() : std::map<uint16_t, esphome::tigo_monitor::master::Queries::Setup>{};
      },
      [&](bool renew, std::string &why) {
        std::lock_guard<std::mutex> lock(master_mu);
        if (!bus_master) {
          why = "this monitor isn't running the bus";
          return 409;
        }
        if (!bus_master->request_discovery(renew)) {
          why = "standing by: change it on the device running the bus";
          return 409;
        }
        return 200;
      },
      &bus_log,
      [&](const esphome::tigo_monitor::master::Bytes &mac, uint16_t id, bool dry_run, std::string &why, uint16_t tap) {
        std::lock_guard<std::mutex> lock(master_mu);
        if (!bus_master) {
          why = "this monitor isn't running the bus";
          return 409;
        }
        // With more than one TAP, an id is free only if no TAP lists it.
        std::set<uint16_t> avoid;
        if (bus_master->taps().size() > 1)
          for (const Node &n : monitor.nodes())
            avoid.insert(static_cast<uint16_t>(strtoul(n.addr.c_str(), nullptr, 16) & 0x7FFF));
        return bus_master->request_table_add(mac, id, dry_run, why, tap, avoid) ? 200 : 409;
      },
      [&](const esphome::tigo_monitor::master::Bytes &mac, bool dry_run, std::string &why, uint16_t tap) {
        std::lock_guard<std::mutex> lock(master_mu);
        if (!bus_master) {
          why = "this monitor isn't running the bus";
          return 409;
        }
        if (!tap) tap = known_tap(*bus_master, tap_of_mac(esphome::tigo_monitor::master::mac_hex(mac)));
        return bus_master->request_table_remove(mac, dry_run, why, tap) ? 200 : 409;
      },
      [&](const esphome::tigo_monitor::master::Bytes &old_mac, const esphome::tigo_monitor::master::Bytes &new_mac,
          bool dry_run, std::string &why, uint16_t tap) {
        std::lock_guard<std::mutex> lock(master_mu);
        if (!bus_master) {
          why = "this monitor isn't running the bus";
          return 409;
        }
        if (!tap) tap = known_tap(*bus_master, tap_of_mac(esphome::tigo_monitor::master::mac_hex(old_mac)));
        return bus_master->request_table_replace(old_mac, new_mac, dry_run, why, tap) ? 200 : 409;
      },
      [&](const esphome::tigo_monitor::master::Bytes &mac, std::string &why) {
        std::lock_guard<std::mutex> lock(master_mu);
        if (!bus_master) {
          why = "this monitor isn't running the bus";
          return 409;
        }
        if (bus_master->request_table_stop_waiting(mac)) return 200;
        why = "not waiting for that optimizer to join";
        return 409;
      },
      [&](std::string &why) {
        std::lock_guard<std::mutex> lock(master_mu);
        if (!bus_master) {
          why = "this monitor isn't running the bus";
          return 409;
        }
        return bus_master->request_herd(why) ? 200 : 409;
      },
      [&](uint16_t id, std::string &why) {
        std::lock_guard<std::mutex> lock(master_mu);
        if (!bus_master) {
          why = "this monitor isn't running the bus";
          return 409;
        }
        return bus_master->request_forget_tap(id, why) ? 200 : 409;
      },
      [&](const esphome::tigo_monitor::master::Bytes &tap_mac, uint16_t channel, uint16_t pan, uint32_t generation,
          bool identify, bool reset, bool dry_run, std::string &why, std::string &given) {
        namespace mp = esphome::tigo_monitor::master;
        std::lock_guard<std::mutex> lock(master_mu);
        if (!bus_master) {
          why = "this monitor isn't running the bus";
          return 409;
        }
        mp::RadioBlock block;
        // Not given: a channel at least two from every TAP heard on this bus,
        // and a PAN from this machine (pan_seed: its MAC on bare metal, a kept
        // random seed in a container), stepped past any in use.
        // The TAP's own aren't another's: the step again on it keeps them.
        std::set<uint16_t> channels, pans;
        uint16_t self = bus_log.tap_of_mac(mp::mac_hex(tap_mac));
        for (const auto &r : bus_log.radios()) {
          if (self && r.tap == self) continue;
          if (r.channel) channels.insert(static_cast<uint16_t>(r.channel));
          if (r.pan) pans.insert(r.pan);
        }
        block.channel = channel ? channel : mp::channel_for(channels);
        mp::Bytes own = pan_seed(state_dir);
        block.pan = pan ? pan : mp::pan_for(own.size() >= 2 ? own : tap_mac, pans);
        if (!block.channel || !block.pan) {
          why = "no free channel or PAN left";
          return 409;
        }
        // 40 optimizers: a bench TAP. A generation not asked for is the TAP's
        // own plus one, read by the job just before it writes.
        mp::map_for_profile(12, block.map, generation <= 0xFFFFFF ? generation : 0);
        block.next_generation = generation > 0xFFFFFF;
        block.skip_identify = !identify;
        block.keep_tap_profile = true;  // a TAP with a profile keeps it; one without gets 40's
        std::random_device rd;
        block.key = mp::key_from([&rd]() { return static_cast<uint8_t>(rd()); });
        given = block.describe();
        return bus_master->request_provision(tap_mac, block, reset, dry_run, why) ? 200 : 409;
      },
      [&](uint16_t node, const std::string &text, std::string &why) {
        std::lock_guard<std::mutex> lock(master_mu);
        if (!bus_master) {
          why = "this monitor isn't running the bus";
          return 409;
        }
        return bus_master->request_ask(node, text, why) ? 200 : 409;
      }});
  if (port < 0 && settings.web_port > 0) port = settings.web_port;
  // Only the default steps aside for another web server; a port someone
  // chose fails loudly instead, and systemd keeps retrying it.
  int web_port = port < 0 ? (tls ? web.start(443, 8443) : web.start(80, 8080)) : web.start(port);
  HttpsRedirect redirect;
  if (port < 0 && tls && web_port > 0) redirect.start(80, web_port);
  if (web_port < 0) {
    input_stop = true;
    if (input.joinable()) input.join();
    cca.stop();
    cca_http.stop();
    return 1;
  }

  // History writes go to the SD card, and the daily prune scans the whole
  // panel table. A worker does them, so they never hold up the main loop,
  // which in the master role is also what polls the bus and keeps the discovery window.
  std::mutex history_mu;
  std::condition_variable history_cv;
  std::deque<std::function<void()>> history_jobs;
  bool history_done = false;
  std::thread history_worker([&] {
    std::unique_lock<std::mutex> lock(history_mu);
    for (;;) {
      history_cv.wait(lock, [&] { return history_done || !history_jobs.empty(); });
      if (history_jobs.empty()) return;  // done, and drained
      auto job = std::move(history_jobs.front());
      history_jobs.pop_front();
      lock.unlock();
      job();
      lock.lock();
    }
  });
  auto post_history = [&](std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lock(history_mu);
      history_jobs.push_back(std::move(job));
    }
    history_cv.notify_one();
  };

  std::atomic<bool> bus_log_saving{false}, housekeeping_saving{false};
  auto last_discovery_check = std::chrono::steady_clock::now() - std::chrono::seconds(60);

  auto last_publish = std::chrono::steady_clock::now() - std::chrono::hours(1);
  // Snapshots land on wall-clock multiples of the interval (:00, :05, ...),
  // so every panel's rows share timestamps.
  int64_t next_snapshot = 0;
  double energy_at_snapshot = monitor.energy_kwh();
  int64_t last_prune = 0;
  BusStats last_stats{};
  auto last_stats_log = std::chrono::steady_clock::now();
  const auto started = std::chrono::steady_clock::now();
  auto last_tls_check = std::chrono::steady_clock::now();
  // systemd (Type=notify, WatchdogSec= in the unit) restarts the service if
  // this loop stops checking in. Without systemd, as in Docker, these do
  // nothing.
  sd_notify(0, "READY=1");
  uint64_t watchdog_usec = 0;
  const bool watchdog = sd_watchdog_enabled(0, &watchdog_usec) > 0;
  if (watchdog)
    LOGI("watchdog: restarted if the main loop stops for %llu s",
         static_cast<unsigned long long>(watchdog_usec / 1000000));
  // Without it, as in Docker, its own: a thread that ends the process once
  // the loop has stuck, for the restart policy to bring it back. As bus
  // master a stuck loop sends nothing (the array keeps producing all the
  // same; the TAP needs no master for that); this gets the master back,
  // and with it the monitoring. It writes straight to
  // stderr and exits without unwinding, since whatever stuck the loop may
  // hold a lock the log or a clean shutdown would wait on.
  std::atomic<bool> own_watchdog_stop{false};
  std::thread own_watchdog;
  if (!watchdog && watchdog_s > 0) {
    LOGI("watchdog: exits if the main loop stops for %d s, for whatever started it to restart it", watchdog_s);
    loop_pass_ms = steady_ms();
    own_watchdog = std::thread([&] {
      while (!own_watchdog_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (steady_ms() - loop_pass_ms > watchdog_s * 1000LL) {
          dprintf(STDERR_FILENO, "<3>watchdog: the main loop has not run for %d s; exiting\n", watchdog_s);
          std::_Exit(1);
        }
      }
    });
  }
  while (!g_stop) {
    // A second's worth of master ticks, one request each, or a plain sleep.
    auto second = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!g_stop && std::chrono::steady_clock::now() < second) {
      std::shared_ptr<BusMaster> bm;
      {
        std::lock_guard<std::mutex> lock(master_mu);
        bm = bus_master;
      }
      if (!bm) break;
      bm->tick(static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()));
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_until(second);
    // Once per pass, after the second's master ticks: a hang anywhere in
    // this loop, the master's tick included, stops it.
    if (watchdog) sd_notify(0, "WATCHDOG=1");
    loop_pass_ms = steady_ms();
    if (restart_requested) {
      LOGI("restart requested from the web UI");
      break;
    }
    if (settings_changed.exchange(false)) {
      Settings fresh = load_settings(settings_path);
      if (fresh.device_id.empty()) fresh.device_id = settings.device_id;
      apply_tls_override(fresh);
      if (!serial_override.empty()) fresh.serial_port = serial_override;
      if (differ_only_in_pages(fresh, settings)) {
        // The web UI's sidebar: nothing to restart, and no reason to make
        // MQTT reconnect and Home Assistant hear every entity again.
        std::lock_guard<std::mutex> lock(settings_mu);
        settings = fresh;
        LOGI("settings changed: the CCA Info page %s, the Tigo Cloud page %s",
             settings.show_cca_page ? "shown" : "hidden", settings.show_cloud_page ? "shown" : "hidden");
      } else {
        bool new_port = fresh.serial_port != settings.serial_port || fresh.baud_rate != settings.baud_rate ||
                        fresh.bus_role != settings.bus_role || fresh.gateway_id != settings.gateway_id ||
                        fresh.standby_takeover_s != settings.standby_takeover_s ||
                        fresh.master_bench != settings.master_bench || fresh.master_startup != settings.master_startup ||
                        fresh.master_housekeeping != settings.master_housekeeping ||
                        fresh.master_queries != settings.master_queries ||
                        fresh.master_optimizer_writes != settings.master_optimizer_writes ||
                        fresh.master_read_tap_config != settings.master_read_tap_config ||
                        fresh.master_table_writes != settings.master_table_writes ||
                        fresh.master_adopt_taps != settings.master_adopt_taps;
        if (fresh.web_port != settings.web_port) LOGI("web: the new port takes effect when the service restarts");
        if (fresh.tls_cert != settings.tls_cert || fresh.tls_key != settings.tls_key)
          LOGI("web: the HTTPS change takes effect when the service restarts");
        {
          std::lock_guard<std::mutex> lock(settings_mu);
          settings = fresh;
        }
        LOGI("settings changed; applying");
        monitor.set_power_calibration(settings.power_calibration);
        monitor.set_stale_timeout_min(settings.stale_timeout_min);
        monitor.set_night_mode_timeout_min(settings.night_mode_timeout_min);
        monitor.set_reset_at_midnight(settings.reset_at_midnight);
        monitor.set_new_optimizer_note(new_optimizer_note(settings));
        mqtt.configure(settings);
        if (new_port && replay.empty()) {
          input_stop = true;
          if (input.joinable()) input.join();
          input = start_input();
          // A new master starts wanting the window: check now, not up to a
          // minute later, so it doesn't renew one auto would let close.
          last_discovery_check = std::chrono::steady_clock::now() - std::chrono::seconds(60);
        }
      }
    }
    monitor.tick();
    // Automatic discovery, once a minute: renew the TAP's join window only
    // while an optimizer in a string hasn't reported today, in
    // daylight (discovery_policy.h). Off, the window is renewed as always.
    if (std::chrono::steady_clock::now() - last_discovery_check >= std::chrono::seconds(60)) {
      last_discovery_check = std::chrono::steady_clock::now();
      bool wanted = true;
      int missing = 0;
      if (settings.discovery_auto) {
        int64_t t = static_cast<int64_t>(time(nullptr));
        tm lt{};
        time_t tt = static_cast<time_t>(t);
        localtime_r(&tt, &lt);
        int64_t midnight = t - (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
        auto expected = monitor.expected_last_heard();
        for (int64_t h : expected) missing += h < midnight;
        wanted = esphome::tigo_monitor::discovery::window_wanted(
            expected, midnight, esphome::tigo_monitor::discovery::daylight(monitor.last_report_unix(), t));
        // What the window is held open for: at night, nothing.
        if (!wanted) missing = 0;
      }
      std::shared_ptr<BusMaster> m;
      {
        std::lock_guard<std::mutex> lock(master_mu);
        m = bus_master;
      }
      if (m) m->set_window_wanted(wanted, missing);
    }
    // On the history worker: an fsync can take a second on a slow SD card.
    if (housekeeping.needs_save() && !housekeeping_saving.exchange(true))
      post_history([&] {
        housekeeping.save_if_changed();
        housekeeping_saving = false;
      });
    if (bus_log.needs_save() && !bus_log_saving.exchange(true))
      post_history([&] {
        bus_log.save_if_changed();
        bus_log_saving = false;
      });
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
        // At night every panel reads 0 W; leave their rows out (the system
        // row still marks the time) and halve what the SD card stores.
        if (!monitor.night_mode())
          for (const auto &p : monitor.panels()) {
            if (p.barcode.empty()) continue;
            // Offline is 0 W, as on the ESP32 once a panel goes stale.
            panel_power.emplace_back(p.barcode, ago - p.updated < monitor.online_window() ? p.reading.power_out : 0.0);
          }
        double energy = monitor.energy_kwh();
        post_history([&history, t = next_snapshot, w = monitor.total_power(), kwh = energy - energy_at_snapshot,
                      panel_power = std::move(panel_power)] { history.record(t, w, kwh, panel_power); });
        energy_at_snapshot = energy;
        next_snapshot = (wall / step + 1) * step;
        if (wall - last_prune > 86400) {
          post_history([&history, days = settings.history_days] { history.prune(days); });
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
    // Two stat() calls: cheap enough to notice a renewal within seconds.
    if (tls && now - last_tls_check >= std::chrono::seconds(10)) {
      tls->reload_if_changed();
      last_tls_check = now;
    }
    if (tls_waiting && now - last_tls_check >= std::chrono::seconds(10)) {
      last_tls_check = now;
      if (tls_waiting->load().empty()) {
        LOGI("web: the certificate loads now; restarting to serve HTTPS");
        break;
      }
    }
    // A finished replay leaves the state for one last publish, then exits.
    if (input_done && !replay.empty()) {
      mqtt.publish();
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      break;
    }
  }
  own_watchdog_stop = true;
  if (own_watchdog.joinable()) own_watchdog.join();
  sd_notify(0, "STOPPING=1");
  g_stop = true;
  input_stop = true;
  if (input.joinable()) input.join();
  {
    std::lock_guard<std::mutex> lock(history_mu);
    history_done = true;
  }
  history_cv.notify_one();
  history_worker.join();
  {
    std::lock_guard<std::mutex> lock(master_mu);
    if (bus_master) carried_seq = bus_master->seq();
    bus_master.reset();
  }
  save_seq(carried_seq);
  redirect.stop();
  web.stop();
  cca.stop();
  cca_http.stop();
  monitor.save();
  housekeeping.save_if_changed();
  bus_log.save_if_changed();
  BusStats s = monitor.stats();
  LOGI("stopping: %u frames, %u bad checksums, %u readings, %zu panels, %zu nodes", s.frames, s.bad_checksums,
       s.power_packets, monitor.panels().size(), monitor.nodes().size());
  return 0;
}
