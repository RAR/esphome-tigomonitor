#include "master_core.h"

#include <algorithm>
#include <cctype>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <random>

namespace esphome {
namespace tigo_master {

using namespace esphome::tigo_monitor;

#define LOGE(...) log_(3, __VA_ARGS__)
#define LOGW(...) log_(4, __VA_ARGS__)
#define LOGI(...) log_(6, __VA_ARGS__)
using master::Bytes;
using master::Frame;

const char *phase_name(Phase p) {
  switch (p) {
    case Phase::running: return "running";
    case Phase::probing: return "probing";
    case Phase::discovering: return "discovering";
  }
  return "unknown";
}

const char *fault_name(Fault f) {
  switch (f) {
    case Fault::none: return "none";
    case Fault::no_port: return "no_port";
    case Fault::no_reply: return "no_reply";
    case Fault::second_master: return "second_master";
    case Fault::refused: return "refused";
    case Fault::discovery_refused: return "discovery_refused";
    case Fault::discovery_failed: return "discovery_failed";
    case Fault::extra_tap: return "extra_tap";
  }
  return "unknown";
}

const char *standby_name(Standby s) {
  switch (s) {
    case Standby::off: return "off";
    case Standby::standing_by: return "standing_by";
    case Standby::taking_over: return "taking_over";
    case Standby::active: return "active";
  }
  return "unknown";
}

static int64_t unix_now() { return static_cast<int64_t>(time(nullptr)); }
static int64_t steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

MasterCore::MasterCore(Transport *port, Log log, Sink on_rx, SentSink on_tx, uint16_t gateway_id, bool bench,
                       bool startup, bool housekeeping, std::vector<master::Learned41::Entry> learned,
                       Targets targets, bool queries, bool writes)
    : on_rx_(std::move(on_rx)), on_tx_(std::move(on_tx)), gateway_(gateway_id), bench_(bench), startup_(startup),
      housekeeping_on_(housekeeping), learned_(std::move(learned)), targets_(std::move(targets)),
      queries_on_(queries), writes_on_(queries && writes), phase_(startup ? Phase::probing : Phase::running),
      port_(port), log_sink_(std::move(log)) {
  set_taps({});
}

MasterCore::~MasterCore() { close(); }

void MasterCore::set_taps(const std::vector<TapEntry> &entries) {
  std::vector<TapEntry> list = entries;
  if (list.empty()) list.push_back({gateway_, {}});
  std::lock_guard<std::mutex> lock(mu_);
  taps_.clear();
  for (const auto &e : list) {
    auto t = std::make_unique<Tap>();
    t->id = e.id;
    t->mac = e.mac;
    t->st.id = e.id;
    t->st.mac = e.mac.empty() ? "" : master::mac_hex(e.mac);
    taps_.push_back(std::move(t));
  }
  turn_ = 0;
}

std::vector<MasterCore::TapEntry> MasterCore::taps() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<TapEntry> out;
  for (const auto &t : taps_) out.push_back({t->id, t->mac});
  return out;
}

MasterCore::Tap *MasterCore::tap_by_id_(uint16_t id) {
  if (!id) return taps_.empty() ? nullptr : taps_.front().get();
  for (auto &t : taps_)
    if (t->id == id) return t.get();
  return nullptr;
}

void MasterCore::log_(int prio, const char *fmt, ...) {
  if (!log_sink_) return;
  char buf[320];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log_sink_(prio, buf);
}

void MasterCore::set_standby(uint32_t takeover_s, Sink on_listen, Learned learned) {
  standby_ = Standby::standing_by;
  takeover_ms_ = takeover_s * 1000;
  on_listen_ = std::move(on_listen);
  learned_cb_ = std::move(learned);
}

void MasterCore::set_pv(const PvState &state, PvSaved saved, bool silent) {
  std::lock_guard<std::mutex> lock(mu_);
  pv_state_ = state;
  pv_silent_ = silent;
  status_.pv = state;
  pv_saved_ = std::move(saved);
}

bool MasterCore::request_pv(bool off) {
  PvState s;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (status_.standby == Standby::standing_by || status_.standby == Standby::taking_over) return false;
    // Stopped by another master: nothing more goes out, so it can't be done.
    if (status_.fault == Fault::second_master) return false;
    pv_state_ = {off, false, unix_now()};
    status_.pv = pv_state_;
    for (auto &t : taps_) t->st.pv_confirmed = t->st.pv_unconfirmed = false;  // until each TAP answers this one
    pv_gen_++;
    pv_silent_ = false;
    s = pv_state_;
    // A re-sort not yet begun stops before it changes anything; one already
    // past its PV off goes on with PV held off (its PV on is sent as off).
    if (off && resort_pending_) {
      resort_pending_.reset();
      resort_running_ = false;
      status_.resort.state = "idle";
      status_.resort.finished = true;
      status_.resort.ok = false;
      status_.resort.message = "PV off was asked for; the re-sort stopped before changing anything";
    } else if (off && resort_running_) {
      resort_pv_cancel_ = true;
    }
  }
  LOGW("PV output: %s, asked from this monitor", off ? "off" : "on");
  if (pv_saved_) pv_saved_(s);
  return true;
}

bool MasterCore::request_discovery(bool renew) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (status_.standby == Standby::standing_by || status_.standby == Standby::taking_over) return false;
    discovery_renewing_ = renew;
    status_.discovery_renewing = renew;
  }
  LOGW("bus master: discovery window %s, asked from this monitor",
       renew ? "renewed again" : "left to close (not renewed until asked)");
  if (discovery_saved_) discovery_saved_(renew);
  return true;
}

// ---- the re-sort (resort.h) -------------------------------------------------

void MasterCore::set_resort_saved(ResortSaved saved, const ResortFile *resume) {
  std::lock_guard<std::mutex> lock(mu_);
  resort_saved_ = std::move(saved);
  if (!resume) return;
  resort_file_ = *resume;
  resort_resume_writing_ = resume->phase == "writing";
  status_.resort.state = "waiting";
  status_.resort.started_unix = resume->started_unix;
  status_.resort.can_put_back = !resume->old_networks.empty();
}

// Under mu_.
bool MasterCore::resort_refusal_(bool dry_run, std::string &why) const {
  char m[112];
  why.clear();
  if (!known_networks_)
    why = "a re-sort needs every TAP told the others' networks (0xE1), which this monitor doesn't send";
  else if (resort_file_ && resort_file_->phase == "stopped")
    why = "a re-sort stopped part-way; put the old networks back, or stop it, first";
  else if (resort_busy_())
    why = "a re-sort is already running";
  else if (table_refusal_(dry_run, 0, why))
    return true;
  else if (taps_.size() < 2)
    why = "a re-sort needs at least two TAPs";
  else
    for (const auto &t : taps_) {  // st and radio are under mu_, as here
      if (t->st.state != "running") {
        snprintf(m, sizeof(m), "TAP %04X isn't answering; a re-sort needs every TAP", t->id);
      } else if (!t->radio) {
        snprintf(m, sizeof(m), "TAP %04X's radio settings haven't been read yet; try again in a minute", t->id);
      } else if (t->st.key != "same") {
        snprintf(m, sizeof(m), "TAP %04X's key isn't the one the others share; use the shared key first (Settings > Bus)",
                 t->id);
      } else {
        continue;
      }
      why = m;
      break;
    }
  if (why.empty() && !dry_run && !discovery_renewing_) why = "the discovery window is held closed; no optimizer could join";
  if (why.empty() && !resort_draw_) why = "no source of random numbers for the new networks";
  return !why.empty();
}

bool MasterCore::request_resort(bool dry_run, std::string &why) {
  std::lock_guard<std::mutex> lock(mu_);
  if (resort_refusal_(dry_run, why)) return false;
  std::vector<master::ResortTap> taps;
  for (const auto &t : taps_) taps.push_back({t->id, *t->radio, master::saves_with_store(t->st.version)});
  std::vector<master::RadioBlock> target = master::resort_networks(taps, resort_draw_);
  if (target.empty()) {
    why = "no free network ID left";
    return false;
  }
  resort_pending_ = std::make_unique<master::ResortJob>(taps, target, dry_run, false);
  resort_running_ = true;
  status_.resort = ResortStatus();
  status_.resort.state = "running";
  status_.resort.dry_run = dry_run;
  status_.resort.step = resort_pending_->step_text();
  status_.resort.started_unix = unix_now();
  return true;
}

bool MasterCore::request_resort_put_back(bool dry_run, std::string &why) {
  std::lock_guard<std::mutex> lock(mu_);
  char m[112];
  why.clear();
  if (!resort_file_ || resort_file_->old_networks.empty())
    why = "there are no old networks to put back";
  else if (resort_running_)
    why = "a re-sort is running; wait for its writes to finish";
  else if (status_.standby == Standby::standing_by || status_.standby == Standby::taking_over)
    why = "this monitor is standing by; only the master running the bus can change the TAPs";
  else if (table_job_running_)
    why = "a change to the table is already running";
  else if (!dry_run && !table_writes_on_)
    why = "table writes are off (master_table_writes); a dry run still works";
  else if (!dry_run && pv_state_.off)
    why = "PV output is held off; turn it back on first";
  if (!why.empty()) return false;
  std::vector<master::ResortTap> taps;
  std::vector<master::RadioBlock> target;
  for (const auto &t : taps_) {
    const master::RadioBlock *old = nullptr;
    for (const auto &o : resort_file_->old_networks)
      if (o.first == t->id) old = &o.second;
    if (!old) {
      snprintf(m, sizeof(m), "TAP %04X wasn't in the re-sort; its old network isn't on file", t->id);
      why = m;
      return false;
    }
    if (t->st.state != "running") {
      snprintf(m, sizeof(m), "TAP %04X isn't answering; the old networks need every TAP", t->id);
      why = m;
      return false;
    }
    taps.push_back({t->id, t->radio ? *t->radio : *old, master::saves_with_store(t->st.version)});
    target.push_back(*old);
  }
  for (const auto &o : resort_file_->old_networks)
    if (!std::any_of(taps_.begin(), taps_.end(), [&o](const std::unique_ptr<Tap> &t) { return t->id == o.first; })) {
      snprintf(m, sizeof(m), "TAP %04X isn't on this bus any more; the old networks need every TAP", o.first);
      why = m;
      return false;
    }
  resort_pending_ = std::make_unique<master::ResortJob>(taps, target, dry_run, true, resort_file_->waited);
  resort_running_ = true;
  status_.resort.state = "putting_back";
  status_.resort.dry_run = dry_run;
  status_.resort.finished = status_.resort.ok = false;
  status_.resort.message.clear();
  status_.resort.dry_frames.clear();
  status_.resort.step = resort_pending_->step_text();
  return true;
}

bool MasterCore::request_resort_stop(std::string &why) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!resort_file_) {
    why = "no re-sort is waiting";
    return false;
  }
  if (resort_running_) {
    why = "a re-sort is running; wait for its writes to finish";
    return false;
  }
  resort_stop_wanted_ = true;
  why.clear();
  return true;
}

bool MasterCore::resort_save_(const ResortFile *f) { return resort_saved_ && resort_saved_(f); }

std::vector<master::Bytes> MasterCore::resort_macs_(const std::vector<master::TableRecord> &records) {
  std::vector<master::Bytes> macs;
  for (const auto &r : records) macs.push_back(r.mac);
  return macs;
}

// The job's save, and its end. Bus thread.
void MasterCore::settle_resort_(uint32_t now_ms) {
  (void) now_ms;
  if (!resort_job_) return;
  master::ResortJob &job = *resort_job_;
  if (job.wants_save()) {
    ResortFile f;
    f.phase = "writing";
    f.started_unix = unix_now();
    f.waited = job.records();
    for (const auto &t : job.taps()) f.old_networks.push_back({t.tap, t.now});
    bool ok = resort_save_(&f);
    if (ok) {
      LOGW("bus master: re-sort: saved the old networks; PV off, then every TAP's new network");
      std::lock_guard<std::mutex> lock(mu_);
      resort_file_ = f;
      status_.resort.can_put_back = true;
    }
    job.saved(ok);
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    status_.resort.step = job.step_text();
  }
  // The bracket closed: every TAP's PV state goes to its PvSwitch, which
  // confirms it and tries again; the job's own PV on is one frame each.
  if (job.pv_off_sent()) {
    resort_bracket_open_ = true;
  } else if (resort_bracket_open_) {
    resort_bracket_open_ = false;
    resort_owe_pv_();
  }
  if (!job.finished()) return;
  bool dry = job.dry_run(), put_back = job.put_back(), ok = job.ok(), began = job.began();
  std::optional<ResortFile> file;
  {
    std::lock_guard<std::mutex> lock(mu_);
    file = resort_file_;
  }
  if (!dry && file) {
    if (!ok && !began && !put_back) {
      resort_save_(nullptr);  // stopped before any network frame: nothing to put back
      file.reset();
    } else if (!ok && !began) {
      file->phase = "stopped";  // a put-back that changed nothing: as it was
      resort_save_(&*file);
    } else {
      // Finished: waiting for the joins. Stopped part-way: the TAPs may be
      // on mixed networks or missing optimizers, so nothing is trimmed or
      // ended; only a put-back or the owner's stop ends it.
      file->phase = ok ? "waiting" : "stopped";
      if (put_back && ok) file->old_networks.clear();
      resort_save_(&*file);
    }
  }
  if (ok && !dry) known_networks_due_();
  if (ok)
    LOGI("bus master: re-sort: %s%s", put_back ? "put back: " : "", job.message().c_str());
  else
    LOGW("bus master: re-sort: %sstopped: %s", put_back ? "the put-back " : "", job.message().c_str());
  if (!dry && (ok || began))
    for (auto &t : taps_) {
      t->resort_read = false;  // read every table afresh
      t->resort_walk = master::TableWalk();
    }
  std::lock_guard<std::mutex> lock(mu_);
  resort_file_ = file;
  status_.resort.finished = true;
  status_.resort.ok = ok;
  status_.resort.dry_run = dry;
  status_.resort.message = job.message();
  status_.resort.dry_frames = job.dry_text();
  status_.resort.state = !file ? "idle" : file->phase == "waiting" ? "waiting" : "stopped";
  status_.resort.can_put_back = file && !file->old_networks.empty();
  resort_running_ = false;
  resort_pv_cancel_ = false;
  resort_job_.reset();
}

void MasterCore::resort_owe_pv_() {
  for (auto &t : taps_) t->resort_pv_owed = true;
}

// Another master took the bus: nothing more goes out, so the job stops
// where it is. Inside the bracket the file stays "writing", so the next
// start sends every TAP its PV state; past it, "stopped". Bus thread.
void MasterCore::resort_cut_off_(const char *why) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    resort_pending_.reset();
    if (!resort_job_) resort_running_ = false;
  }
  if (!resort_job_) return;
  bool began = resort_job_->began(), open = resort_job_->pv_off_sent(), put_back = resort_job_->put_back();
  std::optional<ResortFile> file;
  {
    std::lock_guard<std::mutex> lock(mu_);
    file = resort_file_;
  }
  if (file) {
    if (!began && !put_back) {
      resort_save_(nullptr);
      file.reset();
    } else {
      file->phase = open ? "writing" : began ? "stopped" : file->phase == "writing" ? "stopped" : file->phase;
      resort_save_(&*file);
    }
  }
  if (open) resort_owe_pv_();
  resort_bracket_open_ = false;
  LOGW("bus master: re-sort: stopped: %s%s", why, open ? "; every TAP is owed PV on" : "");
  resort_job_.reset();
  std::lock_guard<std::mutex> lock(mu_);
  resort_file_ = file;
  resort_running_ = false;
  resort_pv_cancel_ = false;
  status_.resort.finished = true;
  status_.resort.ok = false;
  status_.resort.message = why;
  status_.resort.state = !file ? "idle" : file->phase == "waiting" ? "waiting" : "stopped";
  status_.resort.can_put_back = file && !file->old_networks.empty();
}

// Under no lock; takes mu_.
void MasterCore::sync_resort_status_() {
  std::optional<ResortFile> file;
  {
    std::lock_guard<std::mutex> lock(mu_);
    file = resort_file_;
  }
  if (!file) return;
  std::map<uint16_t, std::vector<master::TableRecord>> tables;
  std::vector<std::pair<uint16_t, int>> joined;
  for (const auto &t : taps_) {
    tables[t->id] = t->resort_table;
    int n = 0;
    for (const auto &r : t->resort_table) n += r.joined();
    joined.push_back({t->id, n});
  }
  std::vector<TableWaiting> missing;
  for (const auto &r : master::resort_missing(tables, resort_macs_(file->waited))) {
    TableWaiting w;
    w.mac = master::mac_hex(r.mac);
    w.id = r.id();
    w.since_unix = file->started_unix;
    missing.push_back(w);
  }
  std::lock_guard<std::mutex> lock(mu_);
  status_.resort.joined = joined;
  status_.resort.missing = missing;
}

// While a re-sort waits: a trim on a TAP whose window has closed, and the
// end once every listed optimizer has joined a TAP. Bus thread.
void MasterCore::resort_wait_(uint32_t now_ms) {
  (void) now_ms;
  std::optional<ResortFile> file;
  bool table_busy;
  {
    std::lock_guard<std::mutex> lock(mu_);
    file = resort_file_;
    table_busy = table_job_running_;
  }
  if (!file || resort_job_ || file->phase != "waiting") {
    for (auto &t : taps_) t->resort_trim_owed = false;
    return;
  }
  if (resort_trim_tap_ && !table_busy) {  // the trim has finished: read again
    resort_trim_tap_ = 0;
    for (auto &t : taps_) t->resort_read = false;
  }
  bool all = true;
  std::map<uint16_t, std::vector<master::TableRecord>> tables;
  for (const auto &t : taps_) {
    if (!t->resort_read || t->lost) all = false;
    tables[t->id] = t->resort_table;
  }
  std::map<uint16_t, std::vector<master::Bytes>> trim = master::resort_trim(tables);
  bool owed = false;
  for (auto &t : taps_) {
    t->resort_trim_owed = all && !trim[t->id].empty();
    owed = owed || t->resort_trim_owed;
  }
  // Without Allow changes to the TAPs no trim can be sent: none is waited
  // for, so the windows go on being renewed, and the page says why.
  {
    std::lock_guard<std::mutex> lock(mu_);
    status_.resort.note = table_writes_on_ || !owed ? ""
        : "Allow changes to the TAPs is off: no TAP is trimmed until it's on";
  }
  if (!table_writes_on_)
    for (auto &t : taps_) t->resort_trim_owed = false;
  if (!all || resort_trim_tap_ || table_busy || !table_writes_on_) return;
  // Joined on two TAPs: left on both, said once.
  std::map<master::Bytes, int> joined_on;
  for (const auto &kv : tables)
    for (const auto &r : kv.second)
      if (r.joined()) joined_on[r.mac]++;
  for (const auto &kv : joined_on)
    if (kv.second > 1 && resort_two_logged_.insert(master::mac_hex(kv.first)).second)
      LOGW("bus master: re-sort: %s is joined on %d TAPs; left on each", master::mac_hex(kv.first).c_str(), kv.second);
  for (auto &t : taps_) {
    if (!t->resort_trim_owed) continue;
    const master::DiscoveryWindow &w = t->window.last();
    if (!t->window.ever_read() || (w.running && w.remaining_s > 0)) continue;  // trimmed when it closes, as the CCA did
    const std::vector<master::Bytes> &macs = trim[t->id];
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (table_job_running_) return;
      start_table_job_(std::make_unique<master::TableRemove>(macs, false), t->id);
    }
    resort_trim_tap_ = t->id;
    resort_trim_job_ = true;
    LOGI("bus master: re-sort: taking %zu optimizer%s off TAP %04X that joined another TAP", macs.size(),
         macs.size() == 1 ? "" : "s", t->id);
    return;
  }
  if (owed) return;
  if (!master::resort_missing(tables, resort_macs_(file->waited)).empty()) return;
  resort_save_(nullptr);
  std::string counts;
  for (const auto &t : taps_) {
    int n = 0;
    for (const auto &r : t->resort_table) n += r.joined();
    char b[48];
    snprintf(b, sizeof(b), "%s%d on TAP %04X", counts.empty() ? "" : ", ", n, t->id);
    counts += b;
  }
  LOGI("bus master: re-sort: done: %s", counts.c_str());
  resort_two_logged_.clear();
  std::lock_guard<std::mutex> lock(mu_);
  resort_file_.reset();
  status_.resort.state = "idle";
  status_.resort.finished = status_.resort.ok = true;
  status_.resort.message = "done: " + counts;
  status_.resort.can_put_back = false;
  status_.resort.missing.clear();
}

void MasterCore::set_window_wanted(bool wanted, int missing, int64_t morning_until_unix) {
  bool changed;
  {
    std::lock_guard<std::mutex> lock(mu_);
    changed = wanted != window_wanted_;
    window_wanted_ = wanted;
    status_.window_wanted = wanted;
    status_.missing_expected = missing;
    status_.morning_until_unix = morning_until_unix;
  }
  if (changed)
    LOGI("bus master: automatic discovery: %s", wanted ? "an optimizer is missing, renewing the window"
                                                       : "every optimizer reported today, letting the window close");
}

// Under mu_: why a change to the table can't start now, or "".
bool MasterCore::table_refusal_(bool dry_run, uint16_t tap, std::string &why) const {
  bool known = !tap, answering = true;
  for (const auto &t : taps_)
    if (t->id == tap || (!tap && t == taps_.front())) {
      known = true;
      answering = t->st.state == "running";  // st is under mu_, as here
    }
  if (status_.standby == Standby::standing_by || status_.standby == Standby::taking_over)
    why = "this monitor is standing by; only the master running the bus can change the TAP's table";
  else if (resort_busy_())
    why = "a re-sort is running";
  else if (table_job_running_)
    why = status_.table_edit.interrupted ? "an interrupted removal is waiting to be finished"
                                         : "a change to the table is already running";
  else if (!known)
    why = "no such TAP on this bus";
  else if (!answering)
    why = "that TAP isn't answering; it can't be changed until it is";
  else if (!dry_run && !table_writes_on_)
    why = "table writes are off (master_table_writes); a dry run still works";
  else if (!dry_run && pv_state_.off)
    why = "PV output is held off; turn it back on first";
  else
    why.clear();
  return !why.empty();
}

// Under mu_.
void MasterCore::start_table_job_(std::unique_ptr<master::TableJob> job, uint16_t tap) {
  if (!tap && !taps_.empty()) tap = taps_.front()->id;
  status_.table_edit = TableEditStatus();
  status_.table_edit.kind = job->kind();
  status_.table_edit.active = true;
  status_.table_edit.dry_run = job->dry_run();
  status_.table_edit.mac = master::mac_hex(job->mac());
  status_.table_edit.step = job->step_text();
  status_.table_edit.started_unix = unix_now();
  status_.table_tap = tap;
  table_job_pending_ = std::move(job);
  table_tap_pending_ = tap;
  table_job_running_ = true;
}

bool MasterCore::request_table_add(const master::Bytes &mac, uint16_t id, bool dry_run, std::string &why, uint16_t tap,
                                   const std::set<uint16_t> &avoid) {
  std::lock_guard<std::mutex> lock(mu_);
  if (table_refusal_(dry_run, tap, why)) return false;
  if (mac.size() != 8) {
    why = "a MAC is 8 bytes";
    return false;
  }
  if (!dry_run && !discovery_renewing_) {
    why = "the discovery window is held closed; the optimizer could never join";
    return false;
  }
  auto job = std::make_unique<master::TableAdd>(mac, id, dry_run);
  job->set_avoid(avoid);
  start_table_job_(std::move(job), tap);
  return true;
}

bool MasterCore::request_provision(const master::Bytes &tap_mac, const master::RadioBlock &block, bool reset,
                                   bool dry_run, std::string &why) {
  std::lock_guard<std::mutex> lock(mu_);
  uint16_t tap = 0;
  std::string want = master::mac_hex(tap_mac);
  for (const auto &t : taps_)
    if (!t->st.mac.empty() && t->st.mac == want) tap = t->id;
  // A TAP polled since start may never have said its MAC; with one TAP, the
  // job asks it first and stops unless it's this one. Not when that check
  // is skipped: then the MAC must be one it has said.
  if (!tap && taps_.size() == 1 && !block.skip_identify) tap = taps_.front()->id;
  // A live system needs "Allow changes to the TAPs", as Add a TAP's step;
  // the bench needs nothing more.
  if (!bench_ && !table_writes_on_ && !dry_run)  // a check only reads
    why = "giving a TAP its radio settings needs Allow changes to the TAPs (Settings > Bus)";
  // Skipping the MAC check, or a reset, are bench tools: on a live system a
  // wrong MAC could re-network the TAP in service, and a reset restarts it.
  else if (!bench_ && (block.skip_identify || reset || block.store))
    why = "skipping the MAC check, a reset or a Store is for the bench only (master_bench)";
  else if (tap_mac.size() != 8 || !tap)
    why = "no TAP on this bus answered with that MAC";  // with several, it must have said its MAC already
  else if (!master::is_radio_block_ok(block.encode()))
    why = "the channel must be 11 to 26";
  else if (table_refusal_(true, tap, why))
    return false;
  if (!why.empty()) return false;
  master::RadioBlock b = block;
  // The key the other TAPs share (one per site): taken from their last
  // reads; refused if they disagree, or one that answers hasn't been read.
  // A lost TAP never read doesn't count. With none to share, a new network
  // keeps its own key, and a re-key (the network kept) has nothing to do.
  if (block.shared_key) {
    std::vector<const master::RadioBlock *> others;
    uint16_t unread = 0;
    for (const auto &x : taps_) {
      if (x->id == tap) {
        // Re-keying a TAP on its factory settings: no network to keep.
        if (block.keep_network && x->radio && !has_network_(*x)) {
          char m[112];
          snprintf(m, sizeof(m), "TAP %04X is still on its factory settings; Add a TAP gives it a network of its own first",
                   tap);
          why = m;
          return false;
        }
        continue;
      }
      // A TAP on its factory settings holds no key worth sharing.
      if (x->radio && !has_network_(*x)) continue;
      if (x->radio) {
        others.push_back(&*x->radio);
      } else if (x->st.state == "running" && !x->cfg_gave_up) {  // st: written under mu_, held here
        others.push_back(nullptr);
        if (!unread) unread = x->id;
      }
    }
    std::array<uint8_t, 16> key{};
    switch (master::shared_key(others, key)) {
      case master::KeyMatch::alone:
        if (block.keep_network) {
          why = "there's no other TAP to share a key with";
          return false;
        }
        break;
      case master::KeyMatch::different:
        why = "your other TAPs don't share a key";
        return false;
      case master::KeyMatch::unread: {
        char m[96];
        snprintf(m, sizeof(m), "TAP %04X's radio settings haven't been read yet; try again in a minute", unread);
        why = m;
        return false;
      }
      case master::KeyMatch::same:
        b.key = key;
        break;
    }
  }
  for (const auto &t : taps_)
    if (t->id == tap) b.saved_by_store = master::saves_with_store(t->st.version);
  start_table_job_(std::make_unique<master::ProvisionJob>(tap_mac, b, reset, dry_run), tap);
  return true;
}

bool MasterCore::request_period(const master::Bytes &tap_mac, uint8_t code, bool dry_run, std::string &why) {
  std::lock_guard<std::mutex> lock(mu_);
  uint16_t tap = 0;
  std::string want = master::mac_hex(tap_mac);
  for (const auto &t : taps_)
    if (!t->st.mac.empty() && t->st.mac == want) tap = t->id;
  if (!bench_ && !table_writes_on_ && !dry_run)
    why = "setting a TAP's report period needs Allow changes to the TAPs (Settings > Bus)";
  else if (!master::period_code_hyperslots(code))
    why = "the period code is 1 to 11 (1 s to 30 s)";
  else if (tap_mac.size() != 8 || !tap)
    why = "no TAP on this bus has said that MAC";
  else if (table_refusal_(true, tap, why))
    return false;
  if (!why.empty()) return false;
  start_table_job_(std::make_unique<master::PeriodJob>(tap_mac, code, dry_run), tap);
  return true;
}

bool MasterCore::request_table_remove(const master::Bytes &mac, bool dry_run, std::string &why, uint16_t tap) {
  return request_table_remove(std::vector<master::Bytes>{mac}, dry_run, why, tap);
}

bool MasterCore::request_table_remove(const std::vector<master::Bytes> &macs, bool dry_run, std::string &why,
                                      uint16_t tap) {
  std::lock_guard<std::mutex> lock(mu_);
  if (table_refusal_(dry_run, tap, why)) return false;
  if (macs.empty() || std::any_of(macs.begin(), macs.end(), [](const master::Bytes &m) { return m.size() != 8; })) {
    why = "a MAC is 8 bytes";
    return false;
  }
  if (!dry_run && !table_saved_) {
    why = "nowhere to save the optimizers that stay, so nothing is erased";
    return false;
  }
  if (macs.size() == 1) start_table_job_(std::make_unique<master::TableRemove>(macs[0], dry_run), tap);
  else start_table_job_(std::make_unique<master::TableRemove>(macs, dry_run), tap);
  return true;
}

bool MasterCore::request_table_replace(const master::Bytes &old_mac, const master::Bytes &new_mac, bool dry_run,
                                       std::string &why, uint16_t tap) {
  std::lock_guard<std::mutex> lock(mu_);
  if (table_refusal_(dry_run, tap, why)) return false;
  if (old_mac.size() != 8 || new_mac.size() != 8) {
    why = "a MAC is 8 bytes";
    return false;
  }
  if (!dry_run && !table_saved_) {
    why = "nowhere to save the optimizers that stay, so nothing is erased";
    return false;
  }
  if (!dry_run && !discovery_renewing_) {
    why = "the discovery window is held closed; the new optimizer could never join";
    return false;
  }
  start_table_job_(std::make_unique<master::TableReplace>(old_mac, new_mac, dry_run), tap);
  return true;
}

bool MasterCore::request_table_handover(const std::vector<master::TableRecord> &from_old_tap, bool dry_run,
                                        std::string &why, uint16_t tap) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!tap || !tap_by_id_(tap)) {
    char b[64];
    snprintf(b, sizeof(b), "no TAP %04X on this monitor's list", tap);
    why = b;
    return false;
  }
  if (table_refusal_(dry_run, tap, why)) return false;
  if (from_old_tap.empty()) {
    why = "no optimizers to hand over";
    return false;
  }
  for (const auto &r : from_old_tap)
    if (r.mac.size() != 8 || r.id() < 2 || r.id() > 1023) {
      why = "each optimizer needs its 8-byte MAC and an address from 0002 to 03FF";
      return false;
    }
  if (!dry_run && !table_saved_) {
    why = "nowhere to save the table before its erase, so nothing is erased";
    return false;
  }
  if (!dry_run && !discovery_renewing_) {
    why = "the discovery window is held closed; the optimizers could never join";
    return false;
  }
  start_table_job_(std::make_unique<master::TableHandover>(from_old_tap, dry_run), tap);
  return true;
}

bool MasterCore::request_herd(std::string &why) {
  std::lock_guard<std::mutex> lock(mu_);
  if (status_.standby == Standby::standing_by || status_.standby == Standby::taking_over)
    why = "this monitor is standing by; only the master running the bus can look for TAPs";
  else if (!startup_)
    why = "bring-up is off (master_startup); a herd re-addresses every TAP";
  else if (!adopt_taps_)
    why = "adopting TAPs is off (master_adopt_taps)";
  else if (table_job_running_)
    why = "a change to a table is running";
  else if (resort_busy_())
    why = "a re-sort is running";
  else
    why.clear();
  if (!why.empty()) return false;
  herd_wanted_ = true;
  return true;
}

bool MasterCore::request_ask(uint16_t node, const std::string &text, std::string &why) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!bench_)
    why = "asking an optimizer a one-off question is for the bench only (master_bench)";
  else if (!queries_on_)
    why = "queries to optimizers are off";
  else if (!master::Queries::is_read_text(text))
    why = "only a read: ^00 and a name";
  else
    why.clear();
  if (!why.empty()) return false;
  ask_wanted_.push_back({node, text});
  return true;
}

bool MasterCore::request_forget_tap(uint16_t id, std::string &why) {
  std::lock_guard<std::mutex> lock(mu_);
  const Tap *t = nullptr;
  for (const auto &x : taps_)
    if (x && x->id == id) t = x.get();
  char b[96];
  if (!t) {
    snprintf(b, sizeof(b), "no TAP %04X on this monitor's list", id);
    why = b;
  } else if (taps_.size() <= 1) {
    why = "it is the only TAP";
  } else if (t->st.state == "running") {  // st is under mu_, as status() reads it
    snprintf(b, sizeof(b), "TAP %04X is answering; unplug it before retiring it", id);
    why = b;
  } else if (table_job_running_ && status_.table_tap == id) {
    why = "a change to its table is running";
  } else if (resort_busy_()) {
    why = "a re-sort is running";
  } else {
    why.clear();
  }
  if (!why.empty()) return false;
  forget_wanted_ = id;
  return true;
}

// Done in the tick: taps_ is walked there without the lock.
void MasterCore::forget_tap_(uint16_t id) {
  std::vector<TapEntry> saved;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find_if(taps_.begin(), taps_.end(), [id](const std::unique_ptr<Tap> &t) { return t && t->id == id; });
    if (it == taps_.end() || taps_.size() <= 1 || (*it)->st.state == "running") return;
    taps_.erase(it);
    turn_ = 0;
    for (const auto &t : taps_) saved.push_back({t->id, t->mac});
  }
  LOGI("bus master: TAP %04X retired: no longer polled, and left off the saved list", id);
  if (taps_saved_) taps_saved_(saved);
}

// Every TAP on the bus moved at once, as the CCA brings up a system: the
// known ones to their saved ids, a new one to the next free.
void MasterCore::start_herd_(const char *why) {
  std::vector<master::Herd::Known> known;
  for (const auto &t : taps_)
    if (!t->mac.empty()) known.push_back({t->mac, t->id});
  // Not adopting, a herd still moves our own TAPs (with more than one, the
  // way to re-address one: Discovery's SET_485_ID moves them all, and its
  // GET_MAC, hearing them all, could give one the other's id), but leaves
  // a stranger where it is.
  herd_ = std::make_unique<master::Herd>(FACTORY_ID, gateway_, known, adopt_taps_);
  discovery_.reset();
  restart_taps_();
  phase_ = Phase::discovering;
  holding_ = false;
  LOGW("bus master: %s; herding every TAP on the bus (the CCA's bring-up: polling stops for ~30 s)", why);
}

// The herd's TAPs become the ones polled, in id order: each placed one,
// and any saved one it didn't hear, kept as lost with its id. What each
// had (its join watch, its counts) goes on under the same id.
void MasterCore::herd_done_() {
  std::vector<std::string> lines;  // logged once the list is swapped in
  std::vector<TapEntry> saved;
  {
    // Under mu_ throughout: status() reads taps_ from other threads.
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::unique_ptr<Tap>> next;
    auto keep = [&](uint16_t id) -> std::unique_ptr<Tap> {
      for (auto &t : taps_)
        if (t && t->id == id) return std::move(t);
      auto t = std::make_unique<Tap>();
      t->id = id;
      t->st.id = id;
      return t;
    };
    char b[120];
    for (const auto &p : herd_->placed()) {
      if (!p.answered) continue;
      auto t = keep(p.id);
      t->mac = p.mac;
      t->readdressed = true;  // the CCA's bring-up reads follow
      t->version_asked = false;
      t->lost = false;
      t->st.mac = master::mac_hex(p.mac);
      t->st.state = "running";
      snprintf(b, sizeof(b), "bus master: TAP %04X%s%s", p.id, p.fresh ? " (new)" : "", p.stored ? ", its id stored" : "");
      lines.push_back(b);
      next.push_back(std::move(t));
    }
    for (auto &t : taps_) {
      if (!t) continue;
      snprintf(b, sizeof(b), "bus master: TAP %04X wasn't heard by the herd; keeping its id and asking it now and then",
               t->id);
      lines.push_back(b);
      t->lost = true;
      t->lost_misses = 0;
      t->removed = false;
      t->next_try_ms = 0;
      t->st.state = "lost";
      next.push_back(std::move(t));
    }
    std::sort(next.begin(), next.end(), [](const std::unique_ptr<Tap> &x, const std::unique_ptr<Tap> &y) {
      return x->id < y->id;
    });
    for (const auto &t : next) saved.push_back({t->id, t->mac});
    taps_ = std::move(next);
    turn_ = 0;
    status_.extra_taps.clear();
    known_networks_due_();  // the set of TAPs may have changed
  }
  for (const auto &l : lines) log_(l.find("wasn't heard") != std::string::npos ? 4 : 6, "%s", l.c_str());
  sync_key_words_();
  phase_ = Phase::running;
  probe_index_ = 0;
  std::vector<Bytes> extras = herd_->extras();
  herd_.reset();
  if (status().fault == Fault::discovery_failed || status().fault == Fault::extra_tap) set_fault(Fault::none, "");
  LOGI("bus master: %zu TAP%s; polling", saved.size(), saved.size() == 1 ? "" : "s");
  if (taps_saved_) taps_saved_(saved);
  report_extra_taps_(extras);
}

void MasterCore::set_table_saved(TableSaved saved, const master::Bytes &resume_mac,
                                 const std::vector<master::TableRecord> *resume) {
  std::lock_guard<std::mutex> lock(mu_);
  table_saved_ = std::move(saved);
  if (!resume) return;
  if (!table_writes_on_) {
    // Nothing is sent; the Devices page says so, and a new change is refused.
    status_.table_edit = TableEditStatus();
    status_.table_edit.kind = "remove";
    status_.table_edit.mac = master::mac_hex(resume_mac);
    status_.table_edit.interrupted = true;
    status_.table_edit.step = "interrupted";
    status_.table_edit.message =
        "a removal stopped before writing back the optimizers that stay; turn table writes on to finish it";
    table_job_running_ = true;
    LOGE("bus master: table: %s", status_.table_edit.message.c_str());
    return;
  }
  LOGW("bus master: table: finishing a removal that stopped before writing back the %zu optimizers that stay",
       resume->size());
  // The saved list names no TAP: it is finished on the first.
  start_table_job_(std::make_unique<master::TableRemove>(resume_mac, false, resume), 0);
}

bool MasterCore::request_table_stop_waiting(const master::Bytes &mac) {
  std::lock_guard<std::mutex> lock(mu_);
  bool waited = false;
  for (const auto &w : status_.table_waiting) waited = waited || w.mac == master::mac_hex(mac);
  if (waited) stop_waiting_.push_back(mac);
  return waited;
}

// Tick thread; takes mu_. t's outcomes, then every TAP's waiting list.
void MasterCore::sync_join_status_(Tap &t) {
  auto outcomes = t.join_watch.take_outcomes();
  for (const auto &o : outcomes) {
    LOGI("bus master: table: %04X %s", o.id, o.text.c_str());
    std::string mac = master::mac_hex(o.mac);
    waiting_since_.erase(mac);
    auto from = replaced_from_.find(mac);
    if (from == replaced_from_.end()) continue;
    if (o.joined && on_replaced_) on_replaced_(from->second, mac);
    replaced_from_.erase(from);
  }
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto &o : outcomes) {
    auto &j = status_.table_joins;
    if (j.size() == MasterStatus::TABLE_JOINS_KEPT) j.erase(j.begin());
    j.push_back({master::mac_hex(o.mac), o.id, o.joined, o.text, unix_now()});
  }
  status_.table_waiting.clear();
  for (const auto &tap : taps_)
    for (const auto &w : tap->join_watch.waiting()) {
      std::string mac = master::mac_hex(w.mac);
      status_.table_waiting.push_back({mac, w.id, waiting_since_.count(mac) ? waiting_since_[mac] : 0});
    }
}

void MasterCore::settle_table_job_(uint32_t now_ms) {
  if (!table_job_) return;
  Tap *tap = tap_by_id_(table_tap_);
  if (!tap) tap = &first_();
  // A removal's list of what stays, saved before its erase.
  if (table_job_->wants_save()) {
    bool ok = table_saved_ && table_saved_(table_job_->mac(), &table_job_->survivors());
    if (ok) LOGI("bus master: table: saved the %zu optimizers that stay; erasing", table_job_->survivors().size());
    table_job_->saved(ok);
  }
  bool done = table_job_->finished();
  sync_table_status_(done);
  if (!done) return;
  // Giving a TAP its radio settings touches no table: nothing to save,
  // watch or follow.
  if (std::string(table_job_->kind()) == "provision" || std::string(table_job_->kind()) == "period") {
    const char *what = std::string(table_job_->kind()) == "period" ? "report period" : "radio settings";
    if (table_job_->ok())
      LOGI("bus master: %s: %s", what, table_job_->message().c_str());
    else
      LOGW("bus master: %s: stopped: %s", what, table_job_->message().c_str());
    // A TAP's network changed: every TAP is told the others' again.
    if (table_job_->ok() && !table_job_->dry_run() && std::string(table_job_->kind()) == "provision")
      known_networks_due_();
    table_job_.reset();
    return;
  }
  bool add = std::string(table_job_->kind()) == "add";
  if (!add && !table_job_->dry_run() && !table_job_->keep_saved() && table_saved_) table_saved_(table_job_->mac(), nullptr);
  // An add listed and not joined goes on the watch; a removal takes it off.
  if (add && table_job_->ok() && static_cast<master::TableAdd *>(table_job_.get())->awaiting_join()) {
    tap->join_watch.add(table_job_->mac(), table_job_->id(), now_ms);
    waiting_since_[master::mac_hex(table_job_->mac())] = unix_now();
  }
  // A removal that took it off (not one that found it absent; a resumed one did).
  // Every one it took off: one, or several in one pass; a resumed one, its own.
  std::vector<master::Bytes> gone;
  if (!add) {
    auto *r = static_cast<master::TableRemove *>(table_job_.get());
    gone = r->removed();
    if (gone.empty() && r->resuming()) gone.push_back(table_job_->mac());
  }
  // A re-sort's trim takes an optimizer off a TAP it didn't join: no retire.
  // While one is on file every removal is a trim (the owner's are refused),
  // a resumed one too, across a restart.
  bool resorting;
  {
    std::lock_guard<std::mutex> lock(mu_);
    resorting = resort_file_.has_value();
  }
  if (std::string(table_job_->kind()) == "remove" && table_job_->ok() && !table_job_->dry_run() && on_removed_ &&
      !resort_trim_job_ && !resorting)
    for (const auto &m : gone) on_removed_(master::mac_hex(m));
  resort_trim_job_ = false;
  if (!add && table_job_->ok() && !table_job_->dry_run()) {
    for (const auto &m : gone) {
      for (auto &t : taps_) t->join_watch.drop(m);
      waiting_since_.erase(master::mac_hex(m));
    }
    // A replace's new optimizer is waited for like an add's.
    auto *r = static_cast<master::TableRemove *>(table_job_.get());
    if (r->awaiting_join()) {
      tap->join_watch.add(r->added_mac(), r->id(), now_ms);
      waiting_since_[master::mac_hex(r->added_mac())] = unix_now();
      replaced_from_[master::mac_hex(r->added_mac())] = master::mac_hex(r->mac());
    }
    // A handover's optimizers, each waited for until it joins this TAP.
    for (const auto &j : r->joining()) {
      tap->join_watch.add(j.mac, j.id(), now_ms);
      waiting_since_[master::mac_hex(j.mac)] = unix_now();
    }
  }
  sync_join_status_(*tap);
  if (!add && table_job_->keep_saved()) {
    // Held until it is finished: no other change in between.
    resume_mac_ = table_job_->mac();
    resume_stay_ = table_job_->survivors();
    resume_tap_ = table_tap_;
    resume_waiting_ = true;
    resume_at_ms_ = now_ms + RESUME_AFTER_MS;
    std::lock_guard<std::mutex> lock(mu_);
    table_job_running_ = true;
    LOGW("bus master: table: will finish the removal once the TAP has answered for %u s",
         static_cast<unsigned>(RESUME_AFTER_MS / 1000));
  }
  if (table_job_->ok())
    LOGI("bus master: table: %s", table_job_->message().c_str());
  else
    LOGW("bus master: table: the %s stopped: %s", add ? "add" : std::string(table_job_->kind()) == "replace" ? "replace" : "removal",
         table_job_->message().c_str());
  table_job_.reset();
}

// Under no lock; takes mu_.
void MasterCore::sync_table_status_(bool finished_now) {
  if (!table_job_) return;
  std::lock_guard<std::mutex> lock(mu_);
  TableEditStatus &t = status_.table_edit;
  t.id = table_job_->id();
  t.ack = table_job_->ack();
  t.step = table_job_->step_text();
  t.message = table_job_->message();
  t.dry_frames.clear();
  for (const auto &f : table_job_->dry_frames()) t.dry_frames.push_back(master::mac_hex(f));
  if (std::string(table_job_->kind()) == "provision")
    for (const auto &f : static_cast<master::ProvisionJob *>(table_job_.get())->dry_text()) t.dry_frames.push_back(f);
  if (finished_now) {
    t.active = false;
    t.finished = true;
    t.ok = table_job_->ok();
    t.interrupted = !t.ok && table_job_->keep_saved();
    t.finished_unix = unix_now();
    table_job_running_ = false;
  }
}

void MasterCore::set_discovery(bool renew, DiscoverySaved saved) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    discovery_renewing_ = renew;
    status_.discovery_renewing = renew;
    discovery_saved_ = std::move(saved);
  }
  if (!renew) LOGW("bus master: discovery window left to close, as last asked");
}

// Standing by: the PV state another master gave the TAP, kept so a takeover
// carries it on (it beats this device's own: the bus is what the TAP heard).
void MasterCore::hear_pv_(bool off) {
  PvState s;
  {
    std::lock_guard<std::mutex> lock(mu_);
    pv_heard_since_standby_ = true;
    if (pv_state_.off == off && pv_state_.heard) return;
    pv_state_ = {off, true, unix_now()};
    status_.pv = pv_state_;
    s = pv_state_;
  }
  LOGW("PV output: %s, heard from the master running the bus", off ? "off" : "on");
  if (pv_saved_) pv_saved_(s);
}

void MasterCore::sync_pv_status_(Tap &t) {
  if (!t.pv) return;
  std::lock_guard<std::mutex> lock(mu_);
  t.st.pv_confirmed = t.pv->confirmed();
  t.st.pv_unconfirmed = t.pv->unconfirmed();
  t.st.pv_last_sent = t.pv->last_sent();
  t.st.pv_last_reply = t.pv->last_reply();
}

// Every TAP back to a start: its discovery window read before anything
// renews it, housekeeping and queries made again when it is next served.
// Each TAP's key against the others': the key held by more of the read TAPs
// than any other is the site's. A TAP alone is "alone"; one with no other
// TAP read to compare with, or not read itself, "unread"; a tie, "different".
void MasterCore::sync_key_words_() {
  std::vector<std::pair<std::array<uint8_t, 16>, int>> held;
  bool waiting = false;  // a TAP with no settings yet that may still give some
  for (const auto &t : taps_) {
    if (!t->radio && !t->lost && !t->cfg_gave_up) waiting = true;
    if (!has_network_(*t)) continue;
    auto it = std::find_if(held.begin(), held.end(), [&](const auto &h) { return h.first == t->radio->key; });
    if (it == held.end()) held.push_back({t->radio->key, 1});
    else it->second++;
  }
  const std::array<uint8_t, 16> *site = nullptr;
  int best = 0;
  bool tie = false;
  for (const auto &h : held) {
    if (h.second > best) {
      best = h.second;
      site = &h.first;
      tie = false;
    } else if (h.second == best) {
      tie = true;
    }
  }
  int read = 0;
  for (const auto &h : held) read += h.second;
  std::lock_guard<std::mutex> lock(mu_);
  for (auto &t : taps_) {
    if (taps_.size() == 1) t->st.key = "alone";
    else if (!t->radio) t->st.key = "unread";
    else if (!has_network_(*t)) t->st.key = "factory";
    else if (read < 2) t->st.key = waiting ? "unread" : "alone";  // the only TAP with a network
    else t->st.key = !tie && site && t->radio->key == *site ? "same" : "different";
  }
}

// Each TAP told the other TAPs' networks (0xE1, the CCA's shape), when it's
// owed one: not while a table change runs, and only once every other TAP
// has been read or is lost (a lost one is named from its last read; one
// never read is named nowhere). Whatever the answer, it isn't asked again
// until the next reason to (no loop on a refusal).
void MasterCore::take_config_reply_(Tap &t, const Frame &reply) {
  master::TunnelReply tr;
  master::RadioBlock b;
  if (!master::parse_tunnel_reply(reply.payload, tr) || tr.sub != master::SUB_GATEWAY_CFG_REPLY || tr.data.empty() ||
      !master::RadioBlock::decode(master::Bytes(tr.data.begin(), tr.data.end() - 1), b))
    return;
  bool changed = !t.radio || t.radio->channel != b.channel || t.radio->pan != b.pan || t.radio->key != b.key;
  {
    std::lock_guard<std::mutex> lock(mu_);
    t.radio = b;
  }
  sync_key_words_();
  // A network the others haven't been told about (a TAP read for the first
  // time after start, or changed): every TAP is owed an 0xE1 again.
  if (changed) known_networks_due_();
}

// The opening asks each TAP for its settings once; one that didn't answer
// (a lost frame, or a reply in a form we don't read) is asked again each
// CONFIG_RETRY_MS, up to CONFIG_TRIES in all, then given up on, said once,
// and named in nobody's 0xE1, so the others are still told.
bool MasterCore::retry_config_read_(Tap &t, uint32_t now_ms) {
  if (!reads_config_() || !t.config_read_sent || t.radio || t.lost || t.cfg_gave_up) return false;
  if (now_ms - t.cfg_try_ms < CONFIG_RETRY_MS) return false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (table_job_ || table_job_pending_) return false;
    if (t.cfg_tries >= CONFIG_TRIES) t.cfg_gave_up = true;
  }
  if (t.cfg_gave_up) {
    LOGW("bus master: TAP %04X's radio settings couldn't be read (%u tries); it is left out of the other TAPs' networks",
         t.id, static_cast<unsigned>(t.cfg_tries));
    sync_key_words_();
    return false;
  }
  seq_ = master::next_seq(seq_);
  Bytes payload = master::tunnel_request(master::SUB_GATEWAY_CFG, seq_, {0x00, 0x00});
  // The same read the opening sends, a bring-up frame the gate takes as such.
  if (!master::may_send(master::CMD_TUNNEL, payload, bench_, /*startup=*/true)) return false;
  Frame reply;
  in_flight_ = &t;
  bool ok = transact(t.id, master::CMD_TUNNEL, payload, reply, REPLY_TIMEOUT_MS);
  in_flight_ = nullptr;
  t.cfg_tries++;
  t.cfg_try_ms = now_ms;
  if (ok) take_config_reply_(t, reply);
  return true;
}

bool MasterCore::send_known_networks_(Tap &t, uint32_t now_ms) {
  if (!known_networks_ || !t.known_due || t.lost || !t.radio) return false;
  if (t.known_next_ms && static_cast<int32_t>(now_ms - t.known_next_ms) < 0) return false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (table_job_ || table_job_pending_) return false;
  }
  std::vector<master::KnownNetwork> others;
  for (const auto &x : taps_) {
    if (x.get() == &t) continue;
    // One on a channel no network uses (a new TAP on its factory settings,
    // channel 0) is no network to join: named nowhere.
    if (x->radio && !has_network_(*x)) continue;
    if (x->radio) others.push_back({static_cast<uint8_t>(x->radio->channel), x->radio->pan, x->radio->key});
    else if (!x->lost && !x->cfg_gave_up) return false;  // wait for its read
  }
  std::string word;
  if (others.empty()) {
    t.known_due = false;
    return false;
  }
  if (others.size() > master::KNOWN_NETWORKS_MAX) {
    t.known_due = false;
    word = "not sent: more than 16 other TAPs";
  } else if (!table_writes_on_) {
    word = "waiting: Allow changes to the TAPs is off";
  }
  if (!word.empty()) {
    bool said = word.rfind("waiting", 0) == 0 && known_waiting_logged_;
    if (word.rfind("waiting", 0) == 0) known_waiting_logged_ = true;
    {
      std::lock_guard<std::mutex> lock(mu_);
      t.st.known_networks = word;
    }
    if (!said && word.rfind("waiting", 0) == 0)
      LOGW("bus master: the TAPs aren't told each other's networks: %s", word.c_str());
    else if (!said)
      LOGW("bus master: TAP %04X isn't told the other TAPs' networks: %s", t.id, word.c_str());
    return false;
  }
  known_waiting_logged_ = false;
  seq_ = master::next_seq(seq_);
  Bytes payload = master::tunnel_request(master::SUB_KNOWN_NETWORKS, seq_, master::known_networks_args(others));
  if (!master::may_send(master::CMD_TUNNEL, payload, bench_, false, false, false, table_writes_on_)) {
    set_fault(Fault::refused, "refused to send command " + std::to_string(master::CMD_TUNNEL));
    t.known_due = false;
    return false;
  }
  Frame reply;
  in_flight_ = &t;
  bool ok = transact(t.id, master::CMD_TUNNEL, payload, reply, KNOWN_NETWORKS_WAIT_MS);
  in_flight_ = nullptr;
  if (status().fault == Fault::second_master || standing_by_()) return true;
  int status_byte = ok ? master::known_networks_status(reply) : -1;
  // No answer is a frame lost, not a refusal: tried again a few times.
  if (status_byte < 0 && ++t.known_tries < KNOWN_NETWORKS_TRIES) {
    t.known_next_ms = now_ms + KNOWN_NETWORKS_RETRY_MS;
    if (!t.known_next_ms) t.known_next_ms = 1;
    return true;
  }
  t.known_due = false;
  t.known_tries = 0;
  t.known_next_ms = 0;
  std::string text = status_byte == 0 ? "accepted (" + std::to_string(others.size()) + " other TAP" +
                                            (others.size() == 1 ? ")" : "s)")
                                      : master::known_networks_status_text(status_byte);
  {
    std::lock_guard<std::mutex> lock(mu_);
    t.st.known_networks = text;
  }
  if (status_byte == 0)
    LOGI("bus master: TAP %04X told the other TAPs' networks (%zu): %s", t.id, others.size(), text.c_str());
  else
    LOGW("bus master: TAP %04X told the other TAPs' networks (%zu): %s", t.id, others.size(), text.c_str());
  return true;
}

void MasterCore::restart_taps_() {
  for (auto &t : taps_) {
    t->window = master::WindowKeeper();
    drop_housekeeping_(*t);
    t->queries.reset();
    t->readdressed = false;
    t->timeouts_in_a_row = 0;
  }
}

bool MasterCore::open() {
  close();
  std::string error;
  if (!port_->open(error)) {
    set_fault(Fault::no_port, error);
    return false;
  }
  // The TAP may have moved on while the port was closed: read the discovery window
  // before renewing anything, and drop any half-read frame.
  splitter_ = protocol::FrameSplitter<std::string>();
  restart_taps_();
  known_networks_due_();
  sync_key_words_();
  holding_ = false;
  // Or it lost power, and came back at the factory address.
  if (startup_) phase_ = Phase::probing;
  probe_index_ = 0;
  if (several_())
    LOGI("bus master on %s for %zu TAPs from %04X%s", port_->name().c_str(), taps_.size(), first_().id,
         bench_ ? " (bench mode)" : "");
  else
    LOGI("bus master on %s for gateway %04X%s", port_->name().c_str(), first_().id, bench_ ? " (bench mode)" : "");
  std::lock_guard<std::mutex> lock(mu_);
  // A change to the table, and the joins waited for, go on across a reopen.
  TableEditStatus table_edit = status_.table_edit;
  auto waiting = status_.table_waiting;
  auto joins = status_.table_joins;
  uint16_t table_tap = status_.table_tap;
  ResortStatus resort = status_.resort;  // a re-sort goes on across a reopen too
  status_ = MasterStatus{};
  status_.table_edit = table_edit;
  status_.table_waiting = waiting;
  status_.table_joins = joins;
  status_.table_tap = table_tap;
  status_.resort = resort;
  for (auto &t : taps_) {
    TapStatus fresh;
    fresh.id = t->id;
    fresh.mac = t->st.mac;
    fresh.version = t->st.version;
    fresh.key = t->st.key;  // its settings, read before, are still known
    t->st = fresh;
    t->lost = false;
    t->lost_misses = 0;
    t->removed = false;
  }
  status_.active = true;
  status_.pv = pv_state_;
  status_.discovery_renewing = discovery_renewing_;
  status_.housekeeping = housekeeping_on_;
  status_.learned_41 = static_cast<uint32_t>(learned_.size());
  status_.queries = queries_on_;
  status_.writes = writes_on_;
  // A standby listens for its full takeover time before ever sending, so
  // one that comes back finds the other already running the bus.
  if (standby_ != Standby::off) {
    standby_ = Standby::standing_by;
    status_.standby = standby_;
    status_.takeover_s = takeover_ms_ / 1000;
    heard_at_ms_ = steady_ms();
  }
  return true;
}

void MasterCore::close() {
  port_->close();
  std::lock_guard<std::mutex> lock(mu_);
  status_.active = false;
}

std::map<uint16_t, master::Queries::Setup> MasterCore::setups() const {
  std::lock_guard<std::mutex> lock(mu_);
  return setups_;
}

// The master's own state, plus each TAP's: counts summed over them, the
// first TAP's window, version and PV confirmation.
MasterStatus MasterCore::status() const {
  std::lock_guard<std::mutex> lock(mu_);
  MasterStatus m = status_;
  m.table_writes = table_writes_on_;
  int64_t heard = heard_at_ms_;
  if (m.standby == Standby::standing_by || m.standby == Standby::taking_over)
    m.master_heard_ms_ago = heard >= 0 ? steady_ms() - heard : -1;
  m.taps.clear();
  for (size_t i = 0; i < taps_.size(); i++) {
    const TapStatus &t = taps_[i]->st;
    m.taps.push_back(t);
    if (i == 0) {
      m.discovery = t.discovery;
      m.discovery_read = t.discovery_read;
      m.tap_version = t.version;
      m.pv_last_sent = t.pv_last_sent;
      m.pv_last_reply = t.pv_last_reply;
      m.pv_confirmed = true;
    }
    m.renewals += t.renewals;
    m.polls += t.polls;
    m.timeouts += t.timeouts;
    m.resyncs += t.resyncs;
    m.tap_lost += t.tap_lost;
    m.back_without_discovery += t.back_without_discovery;
    m.last_reply_unix = std::max(m.last_reply_unix, t.last_reply_unix);
    for (int64_t u : t.recent_lost_unix) m.recent_lost_unix.push_back(u);
    m.sent_41 += t.sent_41;
    m.sent_session += t.sent_session;
    m.sent_broadcast += t.sent_broadcast;
    m.sent_walk_pages += t.sent_walk_pages;
    m.walks_done += t.walks_done;
    m.sent_reads += t.sent_reads;
    m.not_taken += t.not_taken;
    m.sent_writes += t.sent_writes;
    m.answers += t.answers;
    // Confirmed only once every TAP has answered the PV byte; unconfirmed if any hasn't.
    m.pv_confirmed = m.pv_confirmed && t.pv_confirmed;
    m.pv_unconfirmed = m.pv_unconfirmed || t.pv_unconfirmed;
  }
  std::sort(m.recent_lost_unix.begin(), m.recent_lost_unix.end());
  if (m.recent_lost_unix.size() > MasterStatus::RECENT_LOST)
    m.recent_lost_unix.erase(m.recent_lost_unix.begin(),
                             m.recent_lost_unix.end() - static_cast<long>(MasterStatus::RECENT_LOST));
  return m;
}

void MasterCore::set_fault(Fault f, const std::string &detail) {
  std::lock_guard<std::mutex> lock(mu_);
  if (status_.fault != f) {
    if (f == Fault::second_master || f == Fault::refused || f == Fault::no_port || f == Fault::discovery_refused)
      LOGE("bus master: %s", detail.c_str());
    else if (f != Fault::none) LOGW("bus master: %s", detail.c_str());
  }
  status_.fault = f;
  status_.detail = detail;
  if (f == Fault::second_master || f == Fault::no_port) status_.active = false;
}

void MasterCore::ask_version_(Tap &t) {
  t.version_asked = true;
  Frame v;
  if (!transact(t.id, 0x000A, {}, v)) {
    LOGW("bus master: no answer to VERSION (0x000A)");
    return;
  }
  std::string text(v.payload.begin(), v.payload.end());
  for (char &c : text)
    if (c == '\r' || c == '\n') c = ' ';
  while (!text.empty() && text.back() == ' ') text.pop_back();
  if (several_()) LOGI("bus master: TAP %04X version: %s", t.id, text.c_str());
  else LOGI("bus master: TAP version: %s", text.c_str());
  std::lock_guard<std::mutex> lock(mu_);
  t.st.version = text;
}

bool MasterCore::transact(uint16_t dest, uint16_t cmd, const Bytes &payload, Frame &reply, uint32_t timeout_ms,
                          std::vector<Frame> *all) {
  refused_ = not_taken_ = false;
  if (standing_by_()) return false;  // a standby that just yielded sends nothing more
  master::Bytes wire = master::encode_frame(dest, cmd, payload);
  // What our own request looks like coming back, if the adapter hears itself.
  std::string sent(wire.begin() + 5, wire.end() - 2);
  std::string error;
  int64_t written_from = steady_ms();
  if (!port_->write(wire.data(), wire.size(), error)) {
    set_fault(Fault::no_port, "write failed: " + error);
    port_->close();  // reopened by tick()
    return false;
  }
  // A frame takes 10 bits a byte on the wire; past that, the drain waited
  // on the process, not the line.
  int64_t drain_from = steady_ms();
  port_->drain();
  int64_t drained = steady_ms();
  int baud = port_->baud();
  int64_t on_wire_ms = static_cast<int64_t>(wire.size()) * 10 * 1000 / (baud > 0 ? baud : 38400);
  if (drained - drain_from > on_wire_ms + OVERRUN_SLACK_MS)
    note_pause_(static_cast<uint32_t>(drained - drain_from - on_wire_ms), drained);
  if (on_tx_) {
    int64_t late = drained - (written_from + on_wire_ms);
    on_tx_(reinterpret_cast<const char *>(wire.data()), wire.size(), late > 0 ? static_cast<uint32_t>(late) : 0);
  }

  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  bool got = false, foreign = false, refused = false, not_taken = false;
  bool query = cmd == master::CMD_TUNNEL && payload.size() > 3 && payload[3] == 0x06;
  char buf[512];
  while ((all || !got) && !foreign && !refused && !not_taken) {
    int left = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    if (left <= 0) break;
    int64_t polled_from = steady_ms();
    int n = port_->read(buf, sizeof(buf), left);
    // A wait that ran past its timeout: the process was held up mid-request,
    // which the gap between ticks never sees.
    int64_t polled = steady_ms();
    if (polled - polled_from > left + OVERRUN_SLACK_MS)
      note_pause_(static_cast<uint32_t>(polled - polled_from - left), polled);
    if (n <= 0) break;
    if (on_rx_) {
      int64_t t0 = steady_ms();
      on_rx_(buf, static_cast<size_t>(n));
      int64_t t1 = steady_ms();
      note_pause_(static_cast<uint32_t>(t1 - t0), t1);
    }
    for (int i = 0; i < n; i++) {
      splitter_.feed(buf[i], [&](const std::string &body) {
        Frame f;
        if (!master::decode_frame(body, f)) return;
        if (f.dest == (dest | 0x8000) && f.cmd == 0x0006) {
          // The TAP's NAK. A gateway with no mesh behind it (a spare on the
          // bench) refuses the tunnel this way.
          if (refusals_logged_.insert(cmd).second) LOGW("bus master: the TAP refused command %04X (0x0006)", cmd);
          refused = true;
        } else if (query && f.dest == (dest | 0x8000) && f.cmd == master::CMD_NOT_TAKEN) {
          not_taken = true;  // the TAP's answer, not a late one: nothing else was asked
        } else if (f.dest == (dest | 0x8000) && f.cmd == cmd + 1) {
          // A late reply to the request before ours is skipped, not taken;
          // and so is a second answer (another TAP at the factory address).
          if (all) {
            all->push_back(f);  // a herd's GET_MAC: every TAP's answer
            reply = f;
            got = true;
          } else if (!got && is_reply(cmd, payload, f)) {
            reply = f;
            got = true;
          }
        } else if (is_request_(f) && body != sent) {
          foreign = true;  // a request we didn't send: another master
        }
      });
    }
  }
  if (foreign && standby_ != Standby::off) {
    yield_();
    return false;
  }
  if (foreign) {
    set_fault(Fault::second_master,
              "another master is on the bus (is the CCA still connected?); stopped transmitting");
    return false;
  }
  refused_ = refused && !got;
  not_taken_ = not_taken && !got;
  return got;
}

// Whether f answers this request: a tunnel reply echoes its seq; a poll
// reply, the poll's cursor (or states the TAP's whole cursor, which on_reply
// then adopts).
bool MasterCore::is_reply(uint16_t cmd, const master::Bytes &request, const Frame &f) const {
  if (cmd == master::CMD_TUNNEL) {
    master::TunnelReply t;
    return request.size() > 4 && master::parse_tunnel_reply(f.payload, t) && t.seq == request[4];
  }
  if (cmd == master::CMD_POLL) {
    master::ReplyCursor rc;
    const master::PollCursor &cursor = in_flight_ ? in_flight_->cursor : taps_.front()->cursor;
    return cursor.matches(f) || (master::reply_cursor(f.payload, rc) && rc.full);
  }
  return true;
}

void MasterCore::tick(uint32_t now_ms) {
  int64_t start = steady_ms();
  if (last_tick_end_ms_ >= 0) note_pause_(static_cast<uint32_t>(start - last_tick_end_ms_), start);
  else gap_window_start_ms_ = start;
  tick_(now_ms);
  last_tick_end_ms_ = steady_ms();
}

// A stretch the bus waited on the daemon: between ticks, or inside on_rx_
// (the Monitor decoding, and anything it does under its lock).
void MasterCore::note_pause_(uint32_t ms, int64_t now) {
  if (now - gap_window_start_ms_ >= GAP_WINDOW_MS) {
    gap_prev_window_max_ms_ = gap_window_max_ms_;
    gap_window_max_ms_ = 0;
    gap_window_start_ms_ = now;
  }
  if (ms > gap_window_max_ms_) gap_window_max_ms_ = ms;
  std::lock_guard<std::mutex> lock(mu_);
  if (ms > status_.gap_max_ms) {
    status_.gap_max_ms = ms;
    status_.gap_max_at_unix = unix_now();
  }
  status_.gap_recent_ms = std::max(gap_window_max_ms_, gap_prev_window_max_ms_);
}

// Round robin: each tick serves the next TAP. With more than one, a TAP
// that stopped answering is skipped until its next try is due; if none is
// due, nothing goes out this tick.
MasterCore::Tap *MasterCore::next_tap_(uint32_t now_ms) {
  if (!several_()) return &first_();
  for (size_t i = 0; i < taps_.size(); i++) {
    Tap *t = taps_[(turn_ + i) % taps_.size()].get();
    if (t->lost && static_cast<int32_t>(now_ms - t->next_try_ms) < 0) continue;
    turn_ = (turn_ + i + 1) % taps_.size();
    return t;
  }
  return nullptr;
}

// t answered: a lost one is back.
void MasterCore::tap_answered_(Tap &t) {
  t.timeouts_in_a_row = 0;
  t.refusals_in_a_row = 0;
  if (!t.lost) return;
  t.lost = false;
  t.known_due = true;  // it may have lost power, and its list with it
  t.known_tries = 0;
  t.known_next_ms = 0;
  if (!t.radio && t.cfg_gave_up) {
    std::lock_guard<std::mutex> lock(mu_);
    t.cfg_gave_up = false;
    t.cfg_tries = 0;
  }
  t.lost_misses = 0;
  t.removed = false;
  LOGI("bus master: TAP %04X is answering again; polling it", t.id);
  std::lock_guard<std::mutex> lock(mu_);
  t.st.state = "running";
}

// With more than one TAP: t has not answered NO_REPLY_FAULT_AFTER requests
// in a row, or a lost one missed its try. The others go on being polled.
void MasterCore::tap_unanswered_(Tap &t, uint32_t now_ms) {
  if (!t.lost) {
    if (t.timeouts_in_a_row != NO_REPLY_FAULT_AFTER) return;
    t.lost = true;
    t.lost_misses = 0;
    t.removed = false;
    t.next_try_ms = now_ms + LOST_PROBE_MS;
    t.window = master::WindowKeeper();
    drop_housekeeping_(t);
    t.queries.reset();
    LOGW("bus master: TAP %04X stopped answering; polling the others and asking it every %u s", t.id,
         static_cast<unsigned>(LOST_PROBE_MS / 1000));
    std::lock_guard<std::mutex> lock(mu_);
    t.st.state = "lost";
    return;
  }
  // Every TAP quiet at once is the bus (cut, or being wired), not the TAPs:
  // each is still asked every LOST_PROBE_MS, so polling is back soon after
  // the bus is. Only one quiet while another answers is a TAP gone.
  bool bus_quiet = std::all_of(taps_.begin(), taps_.end(), [](const std::unique_ptr<Tap> &x) { return x->lost; });
  bool removed = ++t.lost_misses >= LOST_MISSES && !bus_quiet;
  t.next_try_ms = now_ms + (removed ? REMOVED_PROBE_MS : LOST_PROBE_MS);
  if (t.lost_misses == LOST_MISSES && bus_quiet)
    LOGW("bus master: TAP %04X has not answered %u tries, and no TAP is answering: it may be the bus, so it is "
         "still asked every %u s",
         t.id, static_cast<unsigned>(LOST_MISSES), static_cast<unsigned>(LOST_PROBE_MS / 1000));
  bool newly_removed = removed && !t.removed;
  t.removed = removed;
  if (newly_removed)
    LOGW("bus master: TAP %04X has not answered %u tries; asking it every %u s (it keeps its id)", t.id,
         static_cast<unsigned>(t.lost_misses), static_cast<unsigned>(REMOVED_PROBE_MS / 1000));
  std::lock_guard<std::mutex> lock(mu_);
  t.st.state = removed ? "removed" : "lost";
}

void MasterCore::tick_(uint32_t now_ms) {
  if (status().fault == Fault::second_master) {
    if (resort_job_) resort_cut_off_("another master is on the bus");
    listen_stopped_();
    return;
  }
  if (!port_->is_open()) {
    // A port that isn't there yet (a USB adapter still enumerating) or went
    // away: try again every few seconds, as listening does.
    if (!port_->configured() || (tried_open_ && now_ms - last_open_try_ms_ < REOPEN_EVERY_MS)) return;
    tried_open_ = true;
    last_open_try_ms_ = now_ms;
    if (!open()) return;
  }
  if (standing_by_()) {
    stand_by_(now_ms);
    return;
  }
  // After the standby check: transact() sends nothing while standing by, so
  // asked there, the probe was spent with nothing sent.
  // The TAP's version, once per start: on the bench before anything else,
  // running the bus after any bring-up, so it goes to the address the TAP
  // has. The CCA asks it at its own bring-up; it changes nothing.
  if (bench_)
    for (auto &tp : taps_)
      if (!tp->version_asked) {
        ask_version_(*tp);
        return;
      }
  {
    bool herd;
    {
      std::lock_guard<std::mutex> lock(mu_);
      herd = herd_wanted_;
      herd_wanted_ = false;
    }
    if (herd && !herd_ && !table_job_) start_herd_("looking for TAPs, asked from this monitor");
  }
  {
    uint16_t forget;
    {
      std::lock_guard<std::mutex> lock(mu_);
      forget = forget_wanted_;
      forget_wanted_ = 0;
    }
    if (forget && !herd_) {
      forget_tap_(forget);
      known_networks_due_();  // no TAP is told about one that's gone
      sync_key_words_();
    }
  }
  {
    std::vector<std::pair<uint16_t, std::string>> asks;
    {
      std::lock_guard<std::mutex> lock(mu_);
      asks.swap(ask_wanted_);
    }
    for (const auto &a : asks)
      for (auto &t : taps_)
        if (t->queries && t->queries->ask(a.first, a.second, now_ms))
          LOGI("bus master: asking optimizer %04X (TAP %04X) %s", a.first, t->id,
               a.second.substr(0, a.second.size() - 1).c_str());
  }
  if (startup_ && phase_ != Phase::running) {
    run_startup(now_ms);
    return;
  }
  // The re-sort: a pending one taken up, a lost TAP whose turn it is, the
  // owner's stop, and the waiting.
  {
    bool took = false, stop = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (resort_pending_ && !resort_job_ && !table_job_ && !table_job_pending_) {
        resort_job_ = std::move(resort_pending_);
        took = true;
      }
      stop = resort_stop_wanted_;
      resort_stop_wanted_ = false;
    }
    if (took) {
      LOGW("bus master: re-sort: %s%s %zu TAPs", resort_job_->dry_run() ? "a dry run of " : "",
           resort_job_->put_back() ? "putting the old networks back on" : "giving new networks to", resort_job_->taps().size());
      if (resort_job_->put_back() && !resort_job_->dry_run()) {
        // From its first PV off, a restart owes every TAP a PV on.
        std::optional<ResortFile> f;
        {
          std::lock_guard<std::mutex> lock(mu_);
          f = resort_file_;
        }
        if (f) {
          f->phase = "writing";
          if (resort_save_(&*f)) {
            std::lock_guard<std::mutex> lock(mu_);
            resort_file_ = f;
          } else {
            resort_job_->abort("couldn't save the re-sort's state; nothing was changed");
          }
        }
      }
      settle_resort_(now_ms);
    }
    if (resort_job_) {
      bool cancel;
      {
        std::lock_guard<std::mutex> lock(mu_);
        cancel = resort_pv_cancel_;
        resort_pv_cancel_ = false;
      }
      uint16_t turn = resort_job_->turn_tap();
      Tap *tt = turn ? tap_by_id_(turn) : nullptr;
      if (cancel && !resort_job_->began()) {
        resort_job_->abort("PV off was asked for; the re-sort stopped before changing anything");
      } else if (turn && (!tt || tt->lost)) {
        char m[64];
        snprintf(m, sizeof(m), "TAP %04X isn't answering", turn);
        resort_job_->abort(m);
      }
      settle_resort_(now_ms);
    }
    // A restart during the writes: every TAP owed its PV state (on, unless
    // held off), sent and confirmed by its PvSwitch, a lost one's when it
    // answers again. The job didn't finish, so the re-sort is stopped.
    if (resort_resume_writing_) {
      resort_resume_writing_ = false;
      resort_owe_pv_();
      LOGW("bus master: re-sort: TigoMonitor restarted during the network writes; PV on to every TAP");
      std::optional<ResortFile> f;
      {
        std::lock_guard<std::mutex> lock(mu_);
        f = resort_file_;
      }
      if (f) {
        f->phase = "stopped";
        resort_save_(&*f);
        std::lock_guard<std::mutex> lock(mu_);
        resort_file_ = f;
        status_.resort.state = "stopped";
        status_.resort.message = "TigoMonitor restarted during the network writes";
      }
    }
    if (stop) {
      size_t missing;
      {
        std::lock_guard<std::mutex> lock(mu_);
        missing = status_.resort.missing.size();
      }
      bool part_way;
      {
        std::lock_guard<std::mutex> lock(mu_);
        part_way = resort_file_ && resort_file_->phase == "stopped";
      }
      resort_save_(nullptr);
      if (part_way)
        LOGW("bus master: re-sort: stopped; the old networks are discarded, and each TAP keeps the network it has now");
      else
        LOGI("bus master: re-sort: stopped waiting; %zu optimizer%s stay listed on every TAP", missing,
             missing == 1 ? "" : "s");
      resort_two_logged_.clear();
      std::lock_guard<std::mutex> lock(mu_);
      resort_file_.reset();
      status_.resort.state = "idle";
      status_.resort.can_put_back = false;
      status_.resort.message = "stopped waiting";
    }
    resort_wait_(now_ms);
  }
  Tap *tp = next_tap_(now_ms);
  if (!tp) return;
  Tap &t = *tp;
  if (!t.version_asked && !t.lost) {
    // A TAP just re-addressed gets the CCA's bring-up, which asks it anyway.
    if (t.readdressed) {
      t.version_asked = true;
    } else {
      ask_version_(t);
      return;
    }
  }
  { std::lock_guard<std::mutex> lock(mu_); status_.phase = phase_; }
  // A lost TAP is only asked whether it is there: a poll, or its window's
  // status. Nothing else is made for it until it answers.
  if (!t.lost) {
    // After a re-address the CCA's bring-up reads go out whatever the
    // housekeeping setting; the rest only with it.
    {
      bool changed, off, silent;
      {
        std::lock_guard<std::mutex> lock(mu_);
        changed = t.pv_gen != pv_gen_;
        t.pv_gen = pv_gen_;
        off = pv_state_.off;
        silent = pv_silent_;
      }
      // No housekeeping, no hourly 0x0B00 but the switch's own.
      if (!t.pv) {
        t.pv = std::make_unique<master::PvSwitch>(off, !housekeeping_on_, now_ms, silent);
        if (silent) LOGW("PV output: took the bus without hearing what the TAP was told; saying nothing about it until asked");
        else if (off) LOGI("PV output is held off; telling the TAP");
      } else if (changed) {
        t.pv->set_off(off, now_ms);
      }
      // The re-sort's bracket has closed: its PV state again, confirmed.
      if (t.resort_pv_owed) {
        t.resort_pv_owed = false;
        t.pv->set_off(off, now_ms);
      }
    }
    // The radio-settings read rides in a housekeeping opening, with or without
    // the rest of housekeeping.
    if ((housekeeping_on_ || t.readdressed || (reads_config_() && !t.config_read_sent)) && !t.housekeeping) {
      master::HousekeepingPlan plan;
      plan.session = plan.broadcast = plan.walks = housekeeping_on_;
      // No 0x41 with more than one TAP: its content is a CCA's, copied, and
      // names the TAP that CCA chose, so through another TAP it could steer
      // an optimizer onto the wrong one. A node that gets none keeps its TAP
      // (owner's analysis, 2026-11-10). Rebuilt after every herd, so a TAP
      // taken on later turns them off on the first too.
      plan.send_41 = housekeeping_on_ && send_41_ && !several_();
      plan.bring_up_reads = t.readdressed;
      // Marked sent when it goes out, below: an opening dropped before it got
      // there (the TAP lost) plans it again.
      plan.config_read = reads_config_() && !t.config_read_sent;
      plan.session_reset = housekeeping_on_ && t.readdressed && !t.session_reset_sent;
      t.session_reset_sent = t.session_reset_sent || plan.session_reset;
      t.housekeeping = std::make_unique<master::Housekeeping>(learned_, now_ms, plan, now_ms ^ unix_now());
      t.housekeeping->set_pv(t.pv.get());
      t.targets_at_ms = now_ms - TARGETS_EVERY_MS;
      if (housekeeping_on_ && several_())
        LOGI("bus master: TAP %04X housekeeping on, without 0x41s: more than one TAP", t.id);
      else if (housekeeping_on_ && !send_41_)
        LOGI("bus master: housekeeping on, without 0x41s: turned off in the settings");
      else if (housekeeping_on_)
        LOGI("bus master: housekeeping on: %zu learned 0x41s%s", learned_.size(),
             learned_.empty() ? " (none heard: using the built-in content)" : "");
    }
    if (queries_on_ && !t.queries) {
      t.queries = std::make_unique<master::Queries>(now_ms, writes_on_);
      LOGI("bus master: queries to optimizers on%s", writes_on_ ? ", with the CCA's two writes" : "");
    }
    // Asked each minute, or each second while none have been heard yet.
    // With more than one TAP, each TAP's own (tap_targets_); with one, all.
    bool own = tap_targets_ && several_();
    if (t.housekeeping && (own || targets_) && now_ms - t.targets_at_ms >= (t.targets_empty ? 1000 : TARGETS_EVERY_MS)) {
      std::vector<uint16_t> list = own ? tap_targets_(t.id) : targets_();
      t.targets_empty = list.empty();
      t.housekeeping->set_targets(list);
      if (tap_pool_) {
        PgwPick pick = tap_pool_(t.id);
        t.housekeeping->set_pool(pick.pool);
        std::lock_guard<std::mutex> lock(mu_);
        t.st.pgw_why = pick.why;
        t.st.pgw_pool = pick.pool;
      }
      t.targets_at_ms = now_ms;
    }
  }
  std::vector<master::Bytes> stops;  // the owner's "stop waiting"s, taken under mu_
  {
    std::lock_guard<std::mutex> lock(mu_);
    // A removal cut off by a TAP restart, once the TAP has answered steadily.
    Tap *rt = tap_by_id_(resume_tap_);
    if (resume_waiting_ && !table_job_ && !table_job_pending_ && rt == &t) {
      if (t.timeouts_in_a_row || t.refusals_in_a_row || t.lost) resume_at_ms_ = now_ms + RESUME_AFTER_MS;
      if (table_writes_on_ && static_cast<int32_t>(now_ms - resume_at_ms_) >= 0) {
        resume_waiting_ = false;
        LOGW("bus master: table: finishing the removal that was cut off, writing back the %zu optimizers that stay",
             resume_stay_.size());
        start_table_job_(std::make_unique<master::TableRemove>(resume_mac_, false, &resume_stay_), t.id);
      }
    }
    stops.swap(stop_waiting_);
    if (table_job_pending_ && !table_job_) {
      table_job_ = std::move(table_job_pending_);
      table_tap_ = table_tap_pending_;
      for (auto &x : taps_) x->join_watch.pause();  // their reads would straddle the change
      bool add = std::string(table_job_->kind()) == "add";
      std::string kind = table_job_->kind();
      if (kind == "period")
        LOGW("bus master: report period: %ssetting TAP %04X to %s on both", table_job_->dry_run() ? "a dry run of " : "",
             table_tap_,
             master::period_code_text(static_cast<master::PeriodJob *>(table_job_.get())->code()).c_str());
      else if (kind == "provision")
        LOGW("bus master: radio settings: %sgiving TAP %04X %s", table_job_->dry_run() ? "a dry run of " : "",
             table_tap_, static_cast<master::ProvisionJob *>(table_job_.get())->block().describe().c_str());
      else if (kind == "handover")
        LOGI("bus master: table: %shanding optimizers over to TAP %04X", table_job_->dry_run() ? "a dry run of " : "",
             table_tap_);
      else if (several_())
        LOGI("bus master: table: %s%s an optimizer on TAP %04X", table_job_->dry_run() ? "a dry run of " : "",
             add ? "adding" : kind == "replace" ? "replacing" : "removing", table_tap_);
      else
        LOGI("bus master: table: %s%s an optimizer", table_job_->dry_run() ? "a dry run of " : "",
             add ? "adding" : kind == "replace" ? "replacing" : "removing");
    }
    // An add waiting for its optimizer to join holds the window open, as
    // the CCA opened it after its write; the owner's switch still wins.
    // So does a re-sort, whatever automatic discovery wants; a TAP owed a
    // trim waits for it before its window is renewed.
    t.window.set_renewing(discovery_renewing_ && (window_wanted_ || t.join_watch.wants_window() || resort_busy_()) &&
                          !t.resort_trim_owed);
    // More than one TAP, told each other's networks: renewals end 01 01, as
    // the CCA's did from its second TAP on.
    t.window.set_known_networks(known_networks_ && several_());
  }
  if (t.refusals_in_a_row >= NO_REPLY_FAULT_AFTER && now_ms - t.refused_at_ms < REFUSED_RETRY_MS) return;
  if (!stops.empty()) {
    for (const auto &mac : stops)
      for (auto &x : taps_)
        if (x->join_watch.drop(mac)) {
          waiting_since_.erase(master::mac_hex(mac));
          LOGI("bus master: table: no longer waiting for an optimizer to join; it stays listed");
        }
    sync_join_status_(t);
  }
  if (retry_config_read_(t, now_ms)) return;
  if (send_known_networks_(t, now_ms)) return;
  // A change to the table first, else the join watch's reads; only on the
  // TAP the change runs on.
  struct TableSources : master::RequestSource {
    master::TableJob *job;
    master::JoinWatch *watch;
    bool from_watch = false;
    master::Request due(uint32_t now, uint8_t &seq) override {
      if (job) return job->due(now, seq);
      master::Request r = watch->due(now, seq);
      from_watch = r.cmd != 0;
      return r;
    }
  } table_sources;
  bool job_here = table_job_ && tap_by_id_(table_tap_) == &t;
  table_sources.job = job_here ? table_job_.get() : nullptr;
  table_sources.watch = &t.join_watch;
  // The re-sort's: its job's frame when it's this TAP's turn, else, while
  // it waits, this TAP's table read every RESORT_READ_MS; ahead of any table
  // change (none runs beside the job).
  bool resort_waiting;
  {
    std::lock_guard<std::mutex> lock(mu_);
    resort_waiting = resort_file_ && resort_file_->phase == "waiting";
  }
  struct ResortSources : master::RequestSource {
    master::ResortJob *job = nullptr;
    Tap *t = nullptr;
    bool walk = false;
    master::RequestSource *rest = nullptr;
    int from = 0;  // 1: the job, 2: the table read
    master::Request due(uint32_t now, uint8_t &seq) override {
      from = 0;
      if (job) {
        master::Request r = job->due(t->id, now, seq);
        if (r.cmd) from = 1;
        return r;
      }
      if (walk) {
        if (!t->resort_walk.active()) t->resort_walk.start();
        from = 2;
        return t->resort_walk.request(seq);
      }
      return rest ? rest->due(now, seq) : master::Request{};
    }
  } resort_sources;
  resort_sources.job = resort_job_.get();
  resort_sources.t = &t;
  resort_sources.walk = resort_waiting && !resort_job_ && !table_job_ &&
                        (t.resort_walk.active() || !t.resort_read || now_ms - t.resort_read_ms >= RESORT_READ_MS);
  resort_sources.rest = (table_job_ && !job_here) ? nullptr : &table_sources;
  size_t waiting_before = t.join_watch.waiting().size();
  master::Request req =
      t.lost ? master::next_request(t.window, t.cursor, seq_, now_ms, nullptr, nullptr, nullptr, nullptr)
             : master::next_request(t.window, t.cursor, seq_, now_ms, t.housekeeping.get(), t.queries.get(), t.pv.get(),
                                    &resort_sources);
  // The owner's PV off holds inside a re-sort's bracket: its PV on goes out as off.
  if (req.resort && req.cmd == master::CMD_SESSION && req.payload.size() == 1 && req.payload[0] == master::PV_ON) {
    std::lock_guard<std::mutex> lock(mu_);
    if (pv_state_.off) req.payload[0] = master::PV_OFF;
  }
  if (t.join_watch.waiting().size() != waiting_before) sync_join_status_(t);  // given up on in due()
  if (job_here) settle_table_job_(now_ms);  // a dry run ends in due(), and a removal saves there
  // Bring-up frames are allowed only straight after a re-address, but for
  // the one GatewayCfg read the owner asked for (set_read_tap_config).
  bool bring_up =
      req.bring_up && (t.readdressed || (reads_config_() && master::is_config_read(req.cmd, req.payload)));
  if (t.housekeeping) {
    std::lock_guard<std::mutex> lock(mu_);
    t.st.sent_41 = t.sent_41_before + t.housekeeping->sent_41();
    t.st.sent_session = t.sent_session_before + t.housekeeping->sent_session();
    t.st.sent_broadcast = t.sent_broadcast_before + t.housekeeping->sent_broadcast();
    t.st.sent_walk_pages = t.sent_walk_pages_before + t.housekeeping->sent_walk_pages();
    t.st.walks_done = t.walks_done_before + t.housekeeping->walks_done();
    t.st.sends_41 = t.housekeeping->sends_41();
    t.st.holder_01 = t.housekeeping->holder(0x01, now_ms);
    t.st.holder_02 = t.housekeeping->holder(0x02, now_ms);
    t.st.holder_04 = t.housekeeping->holder(0x04, now_ms);
    t.st.turn_left_s = (t.housekeeping->turn_left_ms(now_ms) + 999) / 1000;
  }
  if (t.queries) {
    std::lock_guard<std::mutex> lock(mu_);
    t.st.sent_reads = t.queries->sent_reads();
    t.st.sent_writes = t.queries->sent_writes();
    t.st.answers = t.queries->answers();
  }
  if (!req.cmd) return;  // nothing due: a lost TAP's window keeper waiting out a backoff
  // A provisioning check (dry run) only reads, and never builds the write:
  // its reads go with neither switch on, as other checks do.
  bool provision_ok = req.provision && (bench_ || table_writes_on_ || (job_here && table_job_ && table_job_->dry_run()));
  // A re-sort's PV off is allowed inside its bracket (resort.h), which
  // always ends with PV on to every TAP.
  if (!master::may_send(req.cmd, req.payload, bench_, bring_up, writes_on_, (t.pv && t.pv->off()) || req.resort,
                        req.table_edit && table_writes_on_, provision_ok)) {
    set_fault(Fault::refused, "refused to send command " + std::to_string(req.cmd));
    // A job whose frame the gate refused stops, saying so, rather than
    // waiting for a reply that will never come and blocking every change.
    if (resort_sources.from == 1 && resort_job_) {
      resort_job_->abort("refused to send command " + std::to_string(req.cmd));
      settle_resort_(now_ms);
    } else if (resort_sources.from == 2) {
      t.resort_walk = master::TableWalk();
      t.resort_read_ms = now_ms;
    } else if (job_here && table_job_ && req.table_edit) {
      table_job_->abort("refused to send command " + std::to_string(req.cmd));
    }
    return;
  }
  Frame reply;
  in_flight_ = &t;
  bool ok = transact(t.id, req.cmd, req.payload, reply, req.wait_ms ? req.wait_ms : REPLY_TIMEOUT_MS);
  in_flight_ = nullptr;
  if (master::is_config_read(req.cmd, req.payload)) {
    if (!t.config_read_sent) {  // the opening's: the first of its tries
      t.cfg_tries++;
      t.cfg_try_ms = now_ms;
    }
    t.config_read_sent = true;
  }
  // Any read of its settings, the opening's or a provision's: kept, for the
  // key words and for sharing the key.
  if (ok && master::is_config_read(req.cmd, req.payload)) take_config_reply_(t, reply);
  if (status().fault == Fault::second_master || standing_by_()) {
    if (resort_job_) resort_cut_off_("another master is on the bus");
    return;
  }
  if (t.housekeeping && !req.table_edit) t.housekeeping->on_reply(req, ok, reply);
  if (resort_sources.from == 1 && resort_job_) {
    resort_job_->on_reply(t.id, req, ok, reply, now_ms);
    settle_resort_(now_ms);
  } else if (resort_sources.from == 2) {
    master::TableWalk::Result r = t.resort_walk.on_reply(ok, reply);
    if (r == master::TableWalk::done) {
      t.resort_table = t.resort_walk.records();
      t.resort_read = true;
      t.resort_read_ms = now_ms;
      sync_resort_status_();
    } else if (r == master::TableWalk::failed) {
      t.resort_read_ms = now_ms;  // tried again in RESORT_READ_MS
    }
  } else if (req.table_edit && table_sources.from_watch) {
    t.join_watch.on_reply(ok, reply, now_ms);
    sync_join_status_(t);
  } else if (job_here && table_job_ && req.table_edit) {
    table_job_->on_reply(req, ok, reply, now_ms);
    uint8_t sub = req.payload.size() > 3 ? req.payload[3] : 0;
    int ack = table_job_->ack();
    if (sub == 0x29 && req.payload.size() > 5)
      LOGW("bus master: table: wrote %u record%s to the TAP's table (%s)", static_cast<unsigned>(req.payload[5]),
           req.payload[5] == 1 ? "" : "s", ack < 0 ? "no ack; reading it back" : master::set_table_ack_text(ack));
    if (sub == master::SUB_ERASE_FLASH)
      LOGW("bus master: table: erased the TAP's table (%s)", ack < 0 ? "no ack; reading it back" : ack == 0 ? "done" : "refused");
    settle_table_job_(now_ms);
  }
  if (req.cmd == master::CMD_SESSION && t.pv) {
    t.pv->on_reply(req, ok, reply, now_ms);
    sync_pv_status_(t);
  }
  if (ok && t.queries && req.cmd == master::CMD_POLL) {
    uint32_t before = t.queries->answers();
    t.queries->on_poll_reply(reply.payload, now_ms);
    for (const auto &a : t.queries->take_asked())
      LOGI(a.second.rfind("no answer", 0) == 0 ? "bus master: optimizer %04X: %s" : "bus master: optimizer %04X answered: %s",
           a.first, a.second.c_str());
    if (t.queries->answers() != before) {
      std::lock_guard<std::mutex> lock(mu_);
      for (const auto &kv : t.queries->setups()) {
        auto &s = setups_[kv.first];
        if (!kv.second.info.empty()) s.info = kv.second.info;
        if (!kv.second.mppt.empty()) s.mppt = kv.second.mppt;
      }
    }
  }
  if (ok && req.cmd == 0x000A) {
    std::string v(reply.payload.begin(), reply.payload.end());
    for (char &c : v)
      if (c == '\r' || c == '\n') c = ' ';
    while (!v.empty() && v.back() == ' ') v.pop_back();
    LOGI("bus master: TAP version: %s", v.c_str());
    std::lock_guard<std::mutex> lock(mu_);
    t.st.version = v;
  }

  bool window_request = req.cmd == master::CMD_TUNNEL && req.payload.size() > 3 &&
                        (req.payload[3] == master::SUB_DISCOVERY_STATUS ||
                         req.payload[3] == master::SUB_DISCOVERY_START);
  if (ok) {
    tap_answered_(t);
    master::DiscoveryWindow window;
    master::TunnelReply tunnel;
    bool discovery_read = window_request && master::parse_tunnel_reply(reply.payload, tunnel) &&
                      master::parse_discovery_window(tunnel, window);
    if (req.cmd == master::CMD_POLL) {
      uint32_t before = t.cursor.resyncs();
      t.cursor.on_reply(reply);
      if (t.cursor.resyncs() != before) LOGW("bus master: took the TAP's poll cursor, %04X", t.cursor.next());
      if (t.replies_logged < LOG_FIRST_REPLIES) {
        t.replies_logged++;
        std::string h;
        char b[4];
        for (size_t i = 0; i < reply.payload.size() && i < 12; i++) {
          snprintf(b, sizeof(b), "%02X ", reply.payload[i]);
          h += b;
        }
        LOGI("bus master: poll %u reply header %s(%zu bytes); cursor now %04X", static_cast<unsigned>(t.replies_logged), h.c_str(),
             reply.payload.size(), t.cursor.next());
      }
    }
    if (discovery_read) {
      // The CCA lets it run down to 0 and renews then, so that's routine;
      // finding it gone when the TAP comes back is not.
      bool out = !window.running || window.remaining_s == 0;
      // No optimizers listed (a TAP just added): no renewal carries a count
      // of 0, so it stays closed, read every 20 s. Said once, not each read.
      bool empty = window.node_count == 0;
      const char *then = t.window.renewing() ? "renewing" : "left closed, as asked";
      bool was_out = t.window.ever_read() && (!t.window.last().running || t.window.last().remaining_s == 0);
      bool was_empty = t.window.ever_read() && t.window.last().node_count == 0;
      if (out && empty) {
        if (!was_out || !was_empty)
          LOGI("bus master: TAP %04X has no optimizers listed, so its discovery window stays closed until one is added",
               t.id);
      } else if (out && t.lost_since_window_read)
        LOGW("bus master: the TAP came back with its discovery window closed (%u nodes); %s",
             static_cast<unsigned>(window.node_count), then);
      else if (out && (t.window.renewing() || !was_out))
        LOGI("bus master: the TAP's discovery window has closed (%u nodes); %s",
             static_cast<unsigned>(window.node_count), then);
      t.window.on_window(window, now_ms);
    }
    update_discovery_fault_();
    std::lock_guard<std::mutex> lock(mu_);
    if (req.cmd == master::CMD_POLL) t.st.polls++;
    t.st.resyncs = t.cursor.resyncs();
    if (discovery_read) {
      if (t.lost_since_window_read && (!window.running || window.remaining_s == 0)) t.st.back_without_discovery++;
      t.lost_since_window_read = false;
      t.st.discovery = window;
      t.st.discovery_read = true;
    }
    t.st.renewals = t.window.renewals();
    t.st.last_reply_unix = unix_now();
    if (status_.fault == Fault::no_reply) {
      status_.fault = Fault::none;
      status_.detail.clear();
      LOGI("bus master: the TAP is answering again");
    }
    return;
  }
  // A query the TAP couldn't deliver (0x0146): it answered, so the TAP is
  // there. Queries asks again on its usual schedule.
  if (not_taken_) {
    t.timeouts_in_a_row = t.refusals_in_a_row = 0;
    uint16_t node = req.payload.size() > 6 ? static_cast<uint16_t>(req.payload[5] << 8 | req.payload[6]) : 0;
    if (not_taken_logged_.insert(node).second)
      LOGI("bus master: optimizer %04X: the TAP couldn't deliver a query (0x0146); it may be off or rejoining", node);
    std::lock_guard<std::mutex> lock(mu_);
    t.st.not_taken++;
    t.st.last_reply_unix = unix_now();
    return;
  }
  if (req.cmd == master::CMD_POLL) t.cursor.on_timeout();
  if (window_request) t.window.on_no_reply(now_ms);
  // A refusal is the TAP answering: no optimizers behind it, or it isn't
  // running. Say so once, ask once a second, and don't go looking for it.
  if (refused_) {
    t.timeouts_in_a_row = 0;
    t.refused_at_ms = now_ms;
    if (++t.refusals_in_a_row == NO_REPLY_FAULT_AFTER)
      set_fault(Fault::no_reply, req.cmd == master::CMD_POLL || refusals_logged_.count(master::CMD_POLL)
                                     ? "the TAP refuses polls (0x0006): no optimizers behind it, or it isn't running"
                                     : "the TAP refuses every request (0x0006)");
    update_discovery_fault_();
    return;
  }
  t.refusals_in_a_row = 0;
  t.timeouts_in_a_row++;
  {
    std::lock_guard<std::mutex> lock(mu_);
    t.st.timeouts++;
    t.st.renewals = t.window.renewals();
    if (t.timeouts_in_a_row == NO_REPLY_FAULT_AFTER) {
      t.st.tap_lost++;
      t.lost_since_window_read = true;
      auto &recent = t.st.recent_lost_unix;
      if (recent.size() == MasterStatus::RECENT_LOST) recent.erase(recent.begin());
      recent.push_back(unix_now());
    }
  }
  update_discovery_fault_();
  if (several_()) {
    bool was_lost = t.lost;
    tap_unanswered_(t, now_ms);
    // A lost TAP may be back at the factory address (a power cut, and no
    // stored id): in startup mode, its try asks there too, once. One there
    // means a herd, the way to re-address one of several (above).
    Frame f;
    if (startup_ && was_lost && phase_ == Phase::running && !table_job_ &&
        transact(FACTORY_ID, master::CMD_GET_MAC_NOW, {}, f) && f.payload.size() >= 8) {
      Bytes mac(f.payload.begin(), f.payload.begin() + 8);
      // This one, or adopting: a herd. A stranger, not adopting: said once.
      if (adopt_taps_ || (!t.mac.empty() && mac == t.mac)) {
        start_herd_("a TAP is at the factory address");
        return;
      }
      note_stranger_(mac);
    }
    bool all_lost = true;
    for (const auto &x : taps_) all_lost = all_lost && x->lost;
    if (all_lost && !was_lost) {
      // None answers: as with one TAP, it may be the bus, or all of them came
      // back at the factory address.
      set_fault(Fault::no_reply, "no TAP has answered " + std::to_string(NO_REPLY_FAULT_AFTER) + " requests in a row");
      if (startup_) {
        LOGW("bus master: no TAP is answering; looking for them again");
        phase_ = Phase::probing;
        probe_index_ = 0;
        restart_taps_();
        for (auto &x : taps_) x->lost = false;
      }
    }
    return;
  }
  // A TAP that went quiet may have lost power and come back at the factory
  // address: in startup mode, go and look for it again.
  if (startup_ && t.timeouts_in_a_row == NO_REPLY_FAULT_AFTER) {
    LOGW("bus master: the TAP stopped answering at %04X; looking for it again", t.id);
    phase_ = Phase::probing;
    probe_index_ = 0;
    t.window = master::WindowKeeper();
    drop_housekeeping_(t);
  }
  if (t.timeouts_in_a_row == NO_REPLY_FAULT_AFTER)
    set_fault(Fault::no_reply,
              "the TAP has not answered " + std::to_string(NO_REPLY_FAULT_AFTER) + " requests in a row");
}

// Startup: bring the TAPs up before steady polling. Each is probed with a
// read at its id first, so the re-addressing writes only fire when a TAP
// really is at the factory address. One that already answers at its id (a
// warm reset, or the CCA removed without a power cut) needs no discovery;
// once all have been looked for, the scan for one left at the factory address.
void MasterCore::run_startup(uint32_t now_ms) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    status_.phase = phase_;
  }
  // The quiet after the bring-up's RESET: nothing at all goes out.
  if (holding_ && static_cast<int32_t>(now_ms - hold_until_ms_) < 0) return;
  holding_ = false;
  Frame r;
  if (phase_ == Phase::probing) {
    if (probe_index_ >= taps_.size()) probe_index_ = 0;
    Tap &t = *taps_[probe_index_];
    bool last = probe_index_ + 1 == taps_.size();
    t.readdressed = false;
    if (transact(t.id, master::CMD_GET_MAC_NOW, {}, r)) {  // already addressed
      probe_misses_ = 0;
      {
        std::lock_guard<std::mutex> lock(mu_);  // taps() reads the MAC
        if (r.payload.size() >= 8) {
          t.mac.assign(r.payload.begin(), r.payload.begin() + 8);
          t.st.mac = master::mac_hex(t.mac);
        }
        // Found where it should be: running, whatever it was before the
        // look (after every TAP went quiet, one found here was left "lost").
        t.lost = false;
        t.st.state = "running";
      }
      if (several_()) LOGI("bus master: TAP %04X is already there; polling", t.id);
      else LOGI("bus master: the TAP is already at %04X; polling", t.id);
      t.timeouts_in_a_row = 0;
      if (!last) {
        probe_index_++;
        return;
      }
      discovery_ = std::make_unique<master::Discovery>(FACTORY_ID, first_().id, /*scan_only=*/true);
      discovery_tap_ = first_().id;
      phase_ = Phase::discovering;  // just the scan for another TAP, then running
    } else if (++probe_misses_ < PROBE_MISSES_BEFORE_FACTORY) {
      return;  // ask at its own address again before looking anywhere else
    } else if (transact(FACTORY_ID, master::CMD_GET_MAC_NOW, {}, r)) {  // at the factory address
      // Only the TAP this monitor was polling is ever given its id: another
      // one there (a second TAP on the bus) is not, unless adopting. With
      // one TAP, one of no known MAC is taken as it, as before.
      bool same = r.payload.size() >= 8 && !t.mac.empty() && std::equal(t.mac.begin(), t.mac.end(), r.payload.begin());
      bool ours = same || (t.mac.empty() && !several_());
      // With more than one TAP, re-addressing one is a herd: Discovery's
      // SET_485_ID moves them all to the factory address, where its GET_MAC
      // hears them all and could give the first to answer this one's id, and
      // one with no stored id would stay there after its RESET.
      if (adopt_taps_ || (several_() && ours)) {
        probe_misses_ = 0;
        start_herd_("a TAP is at the factory address");
        return;
      }
      if (!ours && several_()) {
        // A stranger, not adopting: said, and this TAP looked for no more
        // here; the others are brought up, and it is asked as a TAP lost.
        if (r.payload.size() >= 8) note_stranger_(Bytes(r.payload.begin(), r.payload.begin() + 8));
        probe_misses_ = 0;
        t.timeouts_in_a_row = NO_REPLY_FAULT_AFTER;
        if (!t.lost) tap_unanswered_(t, now_ms);
        if (!last) {
          probe_index_++;
          return;
        }
        discovery_ = std::make_unique<master::Discovery>(FACTORY_ID, first_().id, /*scan_only=*/true);
        discovery_tap_ = first_().id;
        phase_ = Phase::discovering;
        return;
      }
      if (!ours) {
        set_fault(Fault::extra_tap, "a different TAP answers at the factory address; not re-addressing it");
        return;
      }
      probe_misses_ = 0;
      LOGW("bus master: the TAP is at the factory address %04X; re-addressing it to %04X", FACTORY_ID, t.id);
      discovery_ = std::make_unique<master::Discovery>(FACTORY_ID, t.id);
      discovery_tap_ = t.id;
      t.readdressed = true;
      phase_ = Phase::discovering;
      t.timeouts_in_a_row = 0;
    } else if (several_()) {
      // Not at its id nor the factory address: the others are brought up,
      // and this one is asked again now and then, as a TAP lost.
      probe_misses_ = 0;
      t.timeouts_in_a_row = NO_REPLY_FAULT_AFTER;
      if (!t.lost) tap_unanswered_(t, now_ms);
      if (!last) {
        probe_index_++;
        return;
      }
      discovery_ = std::make_unique<master::Discovery>(FACTORY_ID, first_().id, /*scan_only=*/true);
      discovery_tap_ = first_().id;
      phase_ = Phase::discovering;
    } else {
      t.timeouts_in_a_row++;
      if (t.timeouts_in_a_row == NO_REPLY_FAULT_AFTER)
        set_fault(Fault::no_reply, "no TAP answered at the configured id or the factory address");
    }
    return;
  }
  // phase_ == discovering
  if (herd_) {
    master::DiscoveryRequest h = herd_->next();
    if (h.done) return herd_done_();
    if (h.failed) {
      set_fault(Fault::discovery_failed, std::string("herding the TAPs failed: no answer to ") + herd_->unanswered() +
                                             "; looking again in " + std::to_string(RETRY_AFTER_FAIL_MS / 1000) + " s");
      herd_.reset();
      phase_ = Phase::probing;
      probe_index_ = 0;
      holding_ = true;
      hold_until_ms_ = now_ms + RETRY_AFTER_FAIL_MS;
      return;
    }
    if (h.idle_ms) {
      holding_ = true;
      hold_until_ms_ = now_ms + h.idle_ms;
      return;
    }
    if (!master::may_send(h.cmd, h.payload, bench_, /*startup=*/true)) {
      set_fault(Fault::refused, "refused a herd command " + std::to_string(h.cmd));
      return;
    }
    // After a reset the CCA numbers its tunnel requests from 01 again; the
    // TAP only echoes it (bench, 2026-11-11): this follows the CCA's way.
    if (h.cmd == master::CMD_RESET) seq_ = 0;
    std::vector<Frame> all;
    if (h.collect) {
      transact(h.dest, h.cmd, h.payload, r, h.wait_ms ? h.wait_ms : REPLY_TIMEOUT_MS, &all);
      for (const auto &f : all) herd_->on_reply(f);
    } else if (transact(h.dest, h.cmd, h.payload, r, h.wait_ms ? h.wait_ms : REPLY_TIMEOUT_MS)) {
      herd_->on_reply(r);
    }
    return;
  }
  master::DiscoveryRequest d = discovery_->next();
  if (d.done) {
    if (discovery_->dropped_back())
      LOGW("bus master: the TAP was back at the factory address after the RESET (it keeps no id); re-addressed it "
           "again without one, %s",
           discovery_->stored() ? "and stored the id so it keeps it" : "but it did not ack storing the id");
    Tap *dt = tap_by_id_(discovery_tap_);
    if (dt && !discovery_->mac().empty()) {
      std::lock_guard<std::mutex> lock(mu_);  // taps() reads the MAC
      dt->mac = discovery_->mac();
      dt->st.mac = master::mac_hex(dt->mac);
    }
    // A re-address of one TAP among several: look for the rest next.
    if (dt && dt->readdressed && probe_index_ + 1 < taps_.size()) {
      LOGI("bus master: TAP %04X is now at its id; looking for the next", dt->id);
      probe_index_++;
      phase_ = Phase::probing;
      return;
    }
    // A TAP the scan heard at the factory address, adopting: the herd.
    if (adopt_taps_ && !discovery_->extra_taps().empty()) {
      start_herd_("another TAP answered at the factory address");
      return;
    }
    if (several_()) LOGI("bus master: %zu TAPs; polling", taps_.size());
    else LOGI("bus master: the TAP is now at %04X; polling", first_().id);
    phase_ = Phase::running;
    probe_index_ = 0;
    if (status().fault == Fault::discovery_failed) set_fault(Fault::none, "");
    report_extra_taps_(discovery_->extra_taps());
    return;
  }
  if (d.failed) {
    // Say so, wait, and look again: a TAP slow back from the RESET is found
    // at its id then; one still at the factory address is re-addressed.
    std::string step = discovery_->unanswered();
    set_fault(Fault::discovery_failed, step.empty() ? "re-addressing the TAP failed; it did not answer at the id it was given"
                                                    : "re-addressing the TAP failed: no answer to " + step +
                                                          "; looking again in " +
                                                          std::to_string(RETRY_AFTER_FAIL_MS / 1000) + " s");
    discovery_.reset();
    phase_ = Phase::probing;
    probe_index_ = 0;
    holding_ = true;
    hold_until_ms_ = now_ms + RETRY_AFTER_FAIL_MS;
    return;
  }
  if (d.idle_ms) {
    holding_ = true;
    hold_until_ms_ = now_ms + d.idle_ms;
    return;
  }
  if (!master::may_send(d.cmd, d.payload, bench_, /*startup=*/true)) {
    set_fault(Fault::refused, "refused a discovery command " + std::to_string(d.cmd));
    return;
  }
  if (d.cmd == master::CMD_RESET) seq_ = 0;  // the tunnel count starts at 01 again, as the CCA's
  if (transact(d.dest, d.cmd, d.payload, r, d.wait_ms ? d.wait_ms : REPLY_TIMEOUT_MS)) discovery_->on_reply(r);
}

// Stopped by another master: nothing more goes out, but the bus is still
// read, so the panels the other one polls reach the Monitor (and Home
// Assistant) instead of going stale there. It never starts sending again
// by itself; a restart or a settings save does that.
void MasterCore::listen_stopped_() {
  if (!port_->is_open()) return;
  char buf[512];
  int n = port_->read(buf, sizeof(buf), REPLY_TIMEOUT_MS);
  const Sink &sink = on_listen_ ? on_listen_ : on_rx_;
  if (n > 0 && sink) sink(buf, static_cast<size_t>(n));
}

// Standing by: read what is on the bus, for the Monitor, and note each
// request another master sends. None for takeover_s, then a random moment
// more, and this one takes the bus over.
void MasterCore::stand_by_(uint32_t) {
  char buf[512];
  int n = port_->read(buf, sizeof(buf), REPLY_TIMEOUT_MS);
  if (n < 0) {
    set_fault(Fault::no_port, "read failed: the port closed or failed");
    port_->close();  // reopened by tick()
    return;
  }
  if (n > 0) {
    const Sink &sink = on_listen_ ? on_listen_ : on_rx_;
    if (sink) sink(buf, static_cast<size_t>(n));
    bool heard = false;
    int pv_heard = -1;  // the byte of a 0x0B00 another master sent: what the TAP was last told
    for (int i = 0; i < n; i++) {
      splitter_.feed(buf[i], [&](const std::string &body) {
        Frame f;
        if (!master::decode_frame(body, f) || !is_request_(f)) return;
        heard = true;
        if (f.cmd == master::CMD_SESSION && f.payload.size() == 1) pv_heard = f.payload[0];
        if (learn_taps_ && f.cmd == master::CMD_POLL) learn_tap_(f.dest);
      });
    }
    if (pv_heard >= 0) hear_pv_(pv_heard == master::PV_OFF);
    if (heard) {
      heard_at_ms_ = steady_ms();
      if (standby_ == Standby::taking_over) {
        LOGI("standby: a master is running the bus again; standing by");
        set_standby_(Standby::standing_by);
      }
    }
  }
  int64_t quiet = steady_ms() - heard_at_ms_;
  if (standby_ == Standby::standing_by && quiet >= takeover_ms_) {
    static std::minstd_rand rng(static_cast<uint32_t>(steady_ms()));
    jitter_ms_ = rng() % (TAKEOVER_JITTER_MS + 1);
    set_standby_(Standby::taking_over);
  }
  if (standby_ == Standby::taking_over && quiet >= static_cast<int64_t>(takeover_ms_) + jitter_ms_) take_over_(quiet);
}

// A TAP the master this one stands by for polls: run it too on taking over.
// In the tick, where taps_ is walked without the lock, so changed under it.
void MasterCore::learn_tap_(uint16_t id) {
  // A bring-up's polls to the factory addresses, or a broadcast, aren't a TAP.
  if (id == 0 || id > 0x7FFE || id == 0x1234 || id == FACTORY_ID) return;
  bool known = false;
  for (const auto &t : taps_) known = known || t->id == id;
  // A known one confirms a guess, which is then kept like the rest.
  if (known && !learn_guessed_) return;
  std::vector<TapEntry> saved;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!known) {
      if (learn_guessed_) taps_.clear();
      auto t = std::make_unique<Tap>();
      t->id = id;
      t->st.id = id;
      taps_.push_back(std::move(t));
      turn_ = 0;
    }
    learn_guessed_ = false;
    for (const auto &x : taps_) saved.push_back({x->id, x->mac});
  }
  LOGI("standby: the master polls TAP %04X; %zu TAP%s to run on taking over", id, saved.size(),
       saved.size() == 1 ? "" : "s");
  if (taps_learned_) taps_learned_(saved);
}

// As at start, minus the open: the discovery window is read before anything renews
// it, the poll cursor is taken from the TAP's reply, and a TAP that lost
// power is brought up only if startup is on.
void MasterCore::take_over_(int64_t quiet_ms) {
  LOGW("standby: no master heard for %.1f s; taking over the bus", quiet_ms / 1000.0);
  restart_taps_();
  for (auto &t : taps_) t->lost = false;
  holding_ = false;
  if (learned_cb_) learned_ = learned_cb_();
  phase_ = startup_ ? Phase::probing : Phase::running;
  probe_index_ = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    pv_silent_ = !pv_heard_since_standby_;
  }
  set_standby_(Standby::active);
  std::lock_guard<std::mutex> lock(mu_);
  for (auto &t : taps_) t->st.state = "running";
  status_.takeovers++;
  status_.last_takeover_unix = unix_now();
  status_.learned_41 = static_cast<uint32_t>(learned_.size());
  if (status_.fault != Fault::extra_tap) {
    status_.fault = Fault::none;
    status_.detail.clear();
  }
}

// Another master's request, heard while running the bus: it wins, whoever
// it is. The other side, if it is a standby too, yields as well; their
// takeover times differ, so they don't tie again.
void MasterCore::yield_() {
  if (resort_job_) resort_cut_off_("another master is on the bus");
  LOGW("standby: another master is on the bus; standing by");
  heard_at_ms_ = steady_ms();
  set_standby_(Standby::standing_by);
  std::lock_guard<std::mutex> lock(mu_);
  status_.yields++;
}

void MasterCore::set_standby_(Standby s) {
  standby_ = s;
  std::lock_guard<std::mutex> lock(mu_);
  status_.standby = s;
}

// Counts carry over; the next one starts again when polling does.
void MasterCore::drop_housekeeping_(Tap &t) {
  t.pv.reset();  // made again with the next start, sending the held state first
  if (!t.housekeeping) return;
  t.sent_41_before += t.housekeeping->sent_41();
  t.sent_session_before += t.housekeeping->sent_session();
  t.sent_broadcast_before += t.housekeeping->sent_broadcast();
  t.sent_walk_pages_before += t.housekeeping->sent_walk_pages();
  t.walks_done_before += t.housekeeping->walks_done();
  t.housekeeping.reset();
}

// A TAP heard at the factory address that isn't one of ours, not adopting:
// reported as the scan's are, but only the first time.
void MasterCore::note_stranger_(const Bytes &mac) {
  std::string h = master::mac_hex(mac);
  for (auto &c : h) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  for (const auto &e : status().extra_taps)
    if (e == h) return;
  std::vector<Bytes> all;
  for (const auto &e : status().extra_taps) {
    Bytes b;
    for (size_t i = 0; i + 1 < e.size(); i += 2) b.push_back(static_cast<uint8_t>(std::stoi(e.substr(i, 2), nullptr, 16)));
    all.push_back(b);
  }
  all.push_back(mac);
  report_extra_taps_(all);
}

// A TAP the scan heard at the factory address that isn't one of ours was
// left there: not re-addressed, not polled. Say so, naming it, until a
// bring-up finds none.
void MasterCore::report_extra_taps_(const std::vector<master::Bytes> &macs) {
  std::vector<std::string> hexes;
  for (const auto &mac : macs) {
    std::string h;
    char b[3];
    for (uint8_t c : mac) {
      snprintf(b, sizeof(b), "%02x", c);
      h += b;
    }
    LOGW("bus master: another TAP answered at the factory address (MAC %s); adopting TAPs is off "
         "(master_adopt_taps), so it was left there and is not polled", h.c_str());
    hexes.push_back(h);
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    status_.extra_taps = hexes;
  }
  if (!hexes.empty()) {
    std::string list;
    for (const auto &h : hexes) list += (list.empty() ? "" : ", ") + h;
    set_fault(Fault::extra_tap, "another TAP is on the bus at the factory address (MAC " + list +
                                    "); adopting TAPs is off (master_adopt_taps), so its panels are not monitored");
  } else if (status().fault == Fault::extra_tap) {
    set_fault(Fault::none, "");
  }
}

// A TAP not taking renewals: say so, and clear it once a reading shows
// time on every discovery window. (A closed window doesn't stop
// production, measured 2026-11-03.)
void MasterCore::update_discovery_fault_() {
  Fault f = status().fault;
  bool stuck = false;
  for (const auto &t : taps_) stuck = stuck || t->window.stuck();
  if (stuck && f != Fault::discovery_refused && f != Fault::second_master)
    set_fault(Fault::discovery_refused, "the TAP did not take " + std::to_string(master::WindowKeeper::RENEW_LIMIT) +
                                        " discovery renewals in a row; trying again every " +
                                        std::to_string(master::WindowKeeper::RENEW_BACKOFF_MS / 1000) + " s");
  else if (!stuck && f == Fault::discovery_refused)
    set_fault(Fault::none, "");
}

}  // namespace tigo_master
}  // namespace esphome
