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

void MasterCore::set_window_wanted(bool wanted, int missing) {
  bool changed;
  {
    std::lock_guard<std::mutex> lock(mu_);
    changed = wanted != window_wanted_;
    window_wanted_ = wanted;
    status_.window_wanted = wanted;
    status_.missing_expected = missing;
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
  // job asks it first and stops unless it's this one.
  if (!tap && taps_.size() == 1) tap = taps_.front()->id;
  if (!bench_)
    why = "giving a TAP its radio settings is for the bench only (master_bench)";
  else if (tap_mac.size() != 8 || !tap)
    why = "no TAP on this bus answered with that MAC";  // with several, it must have said its MAC already
  else if (!master::is_radio_block_ok(block.encode()))
    why = "the channel must be 11 to 26";
  else if (table_refusal_(true, tap, why))
    return false;
  if (!why.empty()) return false;
  start_table_job_(std::make_unique<master::ProvisionJob>(tap_mac, block, reset, dry_run), tap);
  return true;
}

bool MasterCore::request_table_remove(const master::Bytes &mac, bool dry_run, std::string &why, uint16_t tap) {
  std::lock_guard<std::mutex> lock(mu_);
  if (table_refusal_(dry_run, tap, why)) return false;
  if (mac.size() != 8) {
    why = "a MAC is 8 bytes";
    return false;
  }
  if (!dry_run && !table_saved_) {
    why = "nowhere to save the optimizers that stay, so nothing is erased";
    return false;
  }
  start_table_job_(std::make_unique<master::TableRemove>(mac, dry_run), tap);
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
  else
    why.clear();
  if (!why.empty()) return false;
  herd_wanted_ = true;
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
  }
  for (const auto &l : lines) log_(l.find("wasn't heard") != std::string::npos ? 4 : 6, "%s", l.c_str());
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
  if (std::string(table_job_->kind()) == "provision") {
    if (table_job_->ok())
      LOGI("bus master: radio settings: %s", table_job_->message().c_str());
    else
      LOGW("bus master: radio settings: stopped: %s", table_job_->message().c_str());
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
  if (std::string(table_job_->kind()) == "remove" && table_job_->ok() && !table_job_->dry_run() && on_removed_ &&
      (table_job_->id() || static_cast<master::TableRemove *>(table_job_.get())->resuming()))
    on_removed_(master::mac_hex(table_job_->mac()));
  if (!add && table_job_->ok() && !table_job_->dry_run()) {
    for (auto &t : taps_) t->join_watch.drop(table_job_->mac());
    waiting_since_.erase(master::mac_hex(table_job_->mac()));
    // A replace's new optimizer is waited for like an add's.
    auto *r = static_cast<master::TableRemove *>(table_job_.get());
    if (r->awaiting_join()) {
      tap->join_watch.add(r->added_mac(), r->id(), now_ms);
      waiting_since_[master::mac_hex(r->added_mac())] = unix_now();
      replaced_from_[master::mac_hex(r->added_mac())] = master::mac_hex(r->mac());
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
  status_ = MasterStatus{};
  status_.table_edit = table_edit;
  status_.table_waiting = waiting;
  status_.table_joins = joins;
  status_.table_tap = table_tap;
  for (auto &t : taps_) {
    TapStatus fresh;
    fresh.id = t->id;
    fresh.mac = t->st.mac;
    fresh.version = t->st.version;
    t->st = fresh;
    t->lost = false;
    t->lost_misses = 0;
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
  t.lost_misses = 0;
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
  bool removed = ++t.lost_misses >= LOST_MISSES;
  t.next_try_ms = now_ms + (removed ? REMOVED_PROBE_MS : LOST_PROBE_MS);
  if (t.lost_misses == LOST_MISSES)
    LOGW("bus master: TAP %04X has not answered %u tries; asking it every %u s (it keeps its id)", t.id,
         static_cast<unsigned>(LOST_MISSES), static_cast<unsigned>(REMOVED_PROBE_MS / 1000));
  std::lock_guard<std::mutex> lock(mu_);
  t.st.state = removed ? "removed" : "lost";
}

void MasterCore::tick_(uint32_t now_ms) {
  if (status().fault == Fault::second_master) {
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
    if (forget && !herd_) forget_tap_(forget);
  }
  if (startup_ && phase_ != Phase::running) {
    run_startup(now_ms);
    return;
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
    }
    // The radio-settings read rides in a housekeeping opening, with or without
    // the rest of housekeeping.
    if ((housekeeping_on_ || t.readdressed || (read_tap_config_ && !t.config_read_sent)) && !t.housekeeping) {
      master::HousekeepingPlan plan;
      plan.session = plan.broadcast = plan.walks = housekeeping_on_;
      // No 0x41 with more than one TAP: its content is a CCA's, copied, and
      // names the TAP that CCA chose, so through another TAP it could steer
      // an optimizer onto the wrong one. A node that gets none keeps its TAP
      // (owner's analysis, 2026-11-10). Rebuilt after every herd, so a TAP
      // taken on later turns them off on the first too.
      plan.send_41 = housekeeping_on_ && !several_();
      plan.bring_up_reads = t.readdressed;
      // Marked sent when it goes out, below: an opening dropped before it got
      // there (the TAP lost) plans it again.
      plan.config_read = read_tap_config_ && !t.config_read_sent;
      plan.session_reset = housekeeping_on_ && t.readdressed && !t.session_reset_sent;
      t.session_reset_sent = t.session_reset_sent || plan.session_reset;
      t.housekeeping = std::make_unique<master::Housekeeping>(learned_, now_ms, plan, now_ms ^ unix_now());
      t.housekeeping->set_pv(t.pv.get());
      t.targets_at_ms = now_ms - TARGETS_EVERY_MS;
      if (housekeeping_on_ && several_())
        LOGI("bus master: TAP %04X housekeeping on, without 0x41s: more than one TAP", t.id);
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
      if (kind == "provision")
        LOGW("bus master: radio settings: %sgiving TAP %04X %s", table_job_->dry_run() ? "a dry run of " : "",
             table_tap_, static_cast<master::ProvisionJob *>(table_job_.get())->block().describe().c_str());
      else if (several_())
        LOGI("bus master: table: %s%s an optimizer on TAP %04X", table_job_->dry_run() ? "a dry run of " : "",
             add ? "adding" : kind == "replace" ? "replacing" : "removing", table_tap_);
      else
        LOGI("bus master: table: %s%s an optimizer", table_job_->dry_run() ? "a dry run of " : "",
             add ? "adding" : kind == "replace" ? "replacing" : "removing");
    }
    // An add waiting for its optimizer to join holds the window open, as
    // the CCA opened it after its write; the owner's switch still wins.
    t.window.set_renewing(discovery_renewing_ && (window_wanted_ || t.join_watch.wants_window()));
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
  size_t waiting_before = t.join_watch.waiting().size();
  master::Request req =
      t.lost ? master::next_request(t.window, t.cursor, seq_, now_ms, nullptr, nullptr, nullptr, nullptr)
             : master::next_request(t.window, t.cursor, seq_, now_ms, t.housekeeping.get(), t.queries.get(), t.pv.get(),
                                    (table_job_ && !job_here) ? nullptr : &table_sources);
  if (t.join_watch.waiting().size() != waiting_before) sync_join_status_(t);  // given up on in due()
  if (job_here) settle_table_job_(now_ms);  // a dry run ends in due(), and a removal saves there
  // Bring-up frames are allowed only straight after a re-address, but for
  // the one GatewayCfg read the owner asked for (set_read_tap_config).
  bool bring_up =
      req.bring_up && (t.readdressed || (read_tap_config_ && master::is_config_read(req.cmd, req.payload)));
  if (t.housekeeping) {
    std::lock_guard<std::mutex> lock(mu_);
    t.st.sent_41 = t.sent_41_before + t.housekeeping->sent_41();
    t.st.sent_session = t.sent_session_before + t.housekeeping->sent_session();
    t.st.sent_broadcast = t.sent_broadcast_before + t.housekeeping->sent_broadcast();
    t.st.sent_walk_pages = t.sent_walk_pages_before + t.housekeeping->sent_walk_pages();
    t.st.walks_done = t.walks_done_before + t.housekeeping->walks_done();
  }
  if (t.queries) {
    std::lock_guard<std::mutex> lock(mu_);
    t.st.sent_reads = t.queries->sent_reads();
    t.st.sent_writes = t.queries->sent_writes();
    t.st.answers = t.queries->answers();
  }
  if (!req.cmd) return;  // nothing due: a lost TAP's window keeper waiting out a backoff
  if (!master::may_send(req.cmd, req.payload, bench_, bring_up, writes_on_, t.pv && t.pv->off(),
                        req.table_edit && table_writes_on_, req.provision && bench_)) {
    set_fault(Fault::refused, "refused to send command " + std::to_string(req.cmd));
    return;
  }
  Frame reply;
  in_flight_ = &t;
  bool ok = transact(t.id, req.cmd, req.payload, reply, req.wait_ms ? req.wait_ms : REPLY_TIMEOUT_MS);
  in_flight_ = nullptr;
  if (master::is_config_read(req.cmd, req.payload)) t.config_read_sent = true;
  if (status().fault == Fault::second_master || standing_by_()) return;
  if (t.housekeeping && !req.table_edit) t.housekeeping->on_reply(req, ok, reply);
  if (req.table_edit && table_sources.from_watch) {
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
      const char *then = t.window.renewing() ? "renewing" : "left closed, as asked";
      bool was_out = t.window.ever_read() && (!t.window.last().running || t.window.last().remaining_s == 0);
      if (out && t.lost_since_window_read)
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
      if (r.payload.size() >= 8) {
        std::lock_guard<std::mutex> lock(mu_);  // taps() reads the MAC
        t.mac.assign(r.payload.begin(), r.payload.begin() + 8);
        t.st.mac = master::mac_hex(t.mac);
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
