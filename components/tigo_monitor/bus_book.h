#pragma once

// What the bus says about each TAP and optimizer, kept by address: firmware
// versions, settings (^00Smrt), MPPT parameters (^00Mppt), ratings (^00Cal),
// the TAPs' radio settings and MACs, and the last changes among them. Filled
// from BusWatch (bus_watch.h) after each frame; the Devices page reads it as
// /api/versions.
//
// Pure and header-only, like bus_watch.h, so both builds keep it the same
// way: the Linux daemon's BusLog wraps it with its JSON files, the ESP32's
// tigo_monitor with a file on its history's flash. Neither the alarm nor the
// exchanges live here: those are BusLog's and BusWatch's.

#include "bus_watch.h"
#include "mesh_history.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace tigo_monitor {
namespace master {

class BusBook {
 public:
  static const size_t KEEP_CHANGES = 50;
  struct Change {
    int64_t unix_ms;
    uint16_t addr;
    Str what, before, after;
  };
  // level: 'I' or 'W'.
  using Log = std::function<void(char level, const char *msg)>;

  explicit BusBook(Log log = nullptr) : log_(std::move(log)) {}

  // The barcode at an address in the node table, or "": an answer from an
  // address another optimizer has taken since (a gateway renumbering) then
  // starts that address's record afresh instead of logging a change.
  void set_barcode_of(std::function<std::string(uint16_t)> f) { barcode_of_ = std::move(f); }

  // After each frame BusWatch was given. runs_bus: this monitor runs the bus
  // now; while another master runs it, what an optimizer reports changed is
  // that master's doing, kept but not listed as a change.
  void absorb(BusWatch &w, int64_t now, bool runs_bus) {
    for (auto r : w.take_radios()) {
      auto it = radios.find(r.tap);
      if (!r.profile && it != radios.end()) r.profile = it->second.first.profile;  // a stored read: keep the live one
      bool changed = it == radios.end() || it->second.first.channel != r.channel || it->second.first.slt != r.slt ||
                     it->second.first.slm != r.slm;
      if (changed) log_f_('I', "bus: TAP %04X radio: channel %d, SLT %d, SLm %d", r.tap, r.channel, r.slt, r.slm);
      radios[r.tap] = {r, now};
      dirty = true;
    }
    for (const auto &t : w.take_tap_ids()) {
      auto it = tap_ids.find(t.tap);
      if (it == tap_ids.end() || it->second.first != t.mac) log_f_('I', "bus: TAP %04X is %s", t.tap, t.mac.c_str());
      tap_ids[t.tap] = {t.mac, now};
      dirty = true;
    }
    for (const auto &m : w.take_mppts()) {
      check_owner_(m.addr);
      auto it = mppts.find(m.addr);
      if (it == mppts.end() || it->second.first.params != m.params)
        log_f_('I', "bus: optimizer %04X MPPT parameters: %s", m.addr, m.params.c_str());
      if (it != mppts.end() && it->second.first.params != m.params)
        change_(now, runs_bus, m.addr, "MPPT", it->second.first.params, m.params);
      mppts[m.addr] = {m, now};
      dirty = true;
    }
    for (const auto &m : w.take_smarts()) {
      check_owner_(m.addr);
      auto it = smarts.find(m.addr);
      if (it == smarts.end() || it->second.first.settings != m.settings)
        log_f_('I', "bus: optimizer %04X settings: %s (%s)", m.addr, m.settings.c_str(),
               m.stored ? "stored" : "not stored");
      if (it != smarts.end() && mesh_history::settings_changed(to_std(it->second.first.settings), to_std(m.settings)))
        change_(now, runs_bus, m.addr, "settings", it->second.first.settings, m.settings);
      smarts[m.addr] = {m, now};
      dirty = true;
    }
    for (const auto &c : w.take_cals()) {
      check_owner_(c.addr);
      auto it = cals.find(c.addr);
      if (it == cals.end() || it->second.first.cal != c.cal)
        log_f_('I', "bus: optimizer %04X is rated %d V, %d W (%s)", c.addr, c.volts, c.watts, c.cal.c_str());
      cals[c.addr] = {c, now};
      dirty = true;
    }
    for (const auto &v : w.take_versions()) {
      if (!v.tap) check_owner_(v.addr);
      VersionBook::Result r = versions.record(v, now);
      dirty = true;
      if (r == VersionBook::changed) {
        if (v.tap || runs_bus)
          log_f_('W', "bus: %s %04X's firmware changed: %s, was %s", v.tap ? "TAP" : "optimizer", v.addr,
                 v.version.c_str(), versions.find(v.tap, v.addr)->previous.c_str());
        if (!v.tap) change_(now, runs_bus, v.addr, "firmware", versions.find(v.tap, v.addr)->previous, v.version);
      } else if (r == VersionBook::first) {
        log_f_('I', "bus: %s %04X runs %s %s", v.tap ? "TAP" : "optimizer", v.addr, v.model.c_str(),
               v.version.c_str());
      }
    }
  }

  // Restoring what was saved: the changes, oldest first.
  void restore_change(const Change &c) {
    changes.push_back(c);
    while (changes.size() > KEEP_CHANGES) changes.pop_front();
  }

  // A saved copy, one record a line, fields apart by tabs (a field's own tab
  // or newline becomes a space), PANs and owners included: the ESP32's file
  // on its history's flash, read back a line at a time by restore_line.
  //   V tap addr model version previous since last
  //   R tap channel slt slm profile pan heard      I tap mac heard
  //   S addr settings stored heard                 M addr params answer heard
  //   C addr volts watts cal heard                 O addr barcode
  //   X unix_ms addr what before after             (changes, oldest first)
  template<class Out> void write_text(Out &out) const {
    char b[48];
    auto f = [&](const Str &s) {
      Str c = s;
      for (char &ch : c)
        if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
      out.append("\t");
      out.append(c.c_str());
    };
    auto n = [&](long long v) {
      std::snprintf(b, sizeof(b), "\t%lld", v);
      out.append(b);
    };
    auto h = [&](unsigned v) {
      std::snprintf(b, sizeof(b), "\t%04X", v & 0xFFFF);
      out.append(b);
    };
    for (const auto &kv : versions.entries()) {
      const auto &e = kv.second;
      out.append("V"), n(e.tap), h(e.addr), f(e.model), f(e.version), f(e.previous), n(e.since_ms), n(e.last_ms);
      out.append("\n");
    }
    for (const auto &kv : radios) {
      const auto &r = kv.second.first;
      out.append("R"), h(kv.first), n(r.channel), n(r.slt), n(r.slm), n(r.profile), h(r.pan), n(kv.second.second);
      out.append("\n");
    }
    for (const auto &kv : tap_ids) out.append("I"), h(kv.first), f(kv.second.first), n(kv.second.second), out.append("\n");
    for (const auto &kv : smarts)
      out.append("S"), h(kv.first), f(kv.second.first.settings), n(kv.second.first.stored), n(kv.second.second),
          out.append("\n");
    for (const auto &kv : mppts)
      out.append("M"), h(kv.first), f(kv.second.first.params), f(kv.second.first.answer), n(kv.second.second),
          out.append("\n");
    for (const auto &kv : cals)
      out.append("C"), h(kv.first), n(kv.second.first.volts), n(kv.second.first.watts), f(kv.second.first.cal),
          n(kv.second.second), out.append("\n");
    for (const auto &kv : owners.owners()) out.append("O"), h(kv.first), f(kv.second), out.append("\n");
    for (const auto &c : changes)
      out.append("X"), n(c.unix_ms), h(c.addr), f(c.what), f(c.before), f(c.after), out.append("\n");
  }

  // One line of write_text's (its newline optional). False for one that
  // isn't a record: the rest still load.
  bool restore_line(const char *line) { return restore_line(Str(line)); }
  template <class S> bool restore_line(const S &line) {
    Vec<Str> t;
    Str cur;
    for (char c : line) {
      if (c == '\n' || c == '\r') break;
      if (c == '\t') {
        t.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    t.push_back(cur);
    auto x = [&](size_t i) { return static_cast<uint16_t>(std::strtoul(t[i].c_str(), nullptr, 16)); };
    auto i64 = [&](size_t i) { return static_cast<int64_t>(std::strtoll(t[i].c_str(), nullptr, 10)); };
    auto i32 = [&](size_t i) { return static_cast<int>(std::strtol(t[i].c_str(), nullptr, 10)); };
    const Str &k = t[0];
    if (k == "V" && t.size() == 8) {
      VersionBook::Entry e;
      e.tap = i32(1) != 0;
      e.addr = x(2);
      e.model = t[3];
      e.version = t[4];
      e.previous = t[5];
      e.since_ms = i64(6);
      e.last_ms = i64(7);
      if (e.version.empty()) return false;
      versions.restore(e);
    } else if (k == "R" && t.size() == 8) {
      TapRadio r;
      r.tap = x(1);
      r.channel = i32(2);
      r.slt = i32(3);
      r.slm = i32(4);
      r.profile = i32(5);
      r.pan = x(6);
      if (!r.tap) return false;
      radios[r.tap] = {r, i64(7)};
    } else if (k == "I" && t.size() == 4) {
      if (!x(1) || t[2].size() != 16) return false;
      tap_ids[x(1)] = {t[2], i64(3)};
    } else if (k == "S" && t.size() == 5) {
      SmartSeen s;
      s.addr = x(1);
      s.settings = t[2];
      s.stored = i32(3) != 0;
      if (s.settings.empty()) return false;
      smarts[s.addr] = {s, i64(4)};
    } else if (k == "M" && t.size() == 5) {
      MpptSeen m;
      m.addr = x(1);
      m.params = t[2];
      m.answer = t[3];
      if (m.params.empty()) return false;
      mppts[m.addr] = {m, i64(4)};
    } else if (k == "C" && t.size() == 6) {
      CalSeen c;
      c.addr = x(1);
      c.volts = i32(2);
      c.watts = i32(3);
      c.cal = t[4];
      if (c.cal.empty()) return false;
      cals[c.addr] = {c, i64(5)};
    } else if (k == "O" && t.size() == 3) {
      if (t[2].empty()) return false;
      owners.restore(x(1), t[2]);
    } else if (k == "X" && t.size() == 6) {
      restore_change({i64(1), x(2), t[3], t[4], t[5]});
    } else {
      return false;
    }
    return true;
  }

  // /api/versions, written into out (anything with append(const char *)):
  // {taps, optimizers, radios, ids, settings, mppt, ratings, changes}, each
  // by address, changes newest first. with_pans adds each TAP's PAN and the
  // address owners, for a saved copy only: the API never carries the PAN.
  template<class Out> void write_json(Out &out, bool with_pans) const {
    char b[64];
    auto str = [&](const Str &s) {
      out.append("\"");
      Str e;
      for (char c : s) {
        if (c == '"' || c == '\\') {
          e += '\\';
          e += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
          std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned char>(c));
          e += b;
        } else {
          e += c;
        }
      }
      out.append(e.c_str());
      out.append("\"");
    };
    auto key = [&](const char *k) {
      out.append("\"");
      out.append(k);
      out.append("\":");
    };
    auto hex = [&](unsigned v) {
      std::snprintf(b, sizeof(b), "\"%04X\"", v & 0xFFFF);
      out.append(b);
    };
    auto num = [&](long long v) {
      std::snprintf(b, sizeof(b), "%lld", v);
      out.append(b);
    };
    auto entry = [&](const VersionBook::Entry &e) {
      out.append("{");
      key("addr"), hex(e.addr), out.append(",");
      key("model"), str(e.model), out.append(",");
      key("version"), str(e.version), out.append(",");
      key("previous"), str(e.previous), out.append(",");
      key("since_unix_ms"), num(e.since_ms), out.append(",");
      key("last_unix_ms"), num(e.last_ms);
      out.append("}");
    };
    out.append("{");
    for (int tap = 1; tap >= 0; tap--) {
      key(tap ? "taps" : "optimizers");
      out.append("[");
      bool first = true;
      for (const auto &kv : versions.entries()) {
        if (kv.second.tap != (tap == 1)) continue;
        if (!first) out.append(",");
        first = false;
        entry(kv.second);
      }
      out.append("],");
    }
    key("radios");
    out.append("[");
    bool first = true;
    for (const auto &kv : radios) {
      if (!first) out.append(",");
      first = false;
      out.append("{");
      key("tap"), hex(kv.first), out.append(",");
      key("channel"), num(kv.second.first.channel), out.append(",");
      key("slt"), num(kv.second.first.slt), out.append(",");
      key("slm"), num(kv.second.first.slm), out.append(",");
      if (kv.second.first.profile) key("profile"), num(kv.second.first.profile), out.append(",");
      if (with_pans && kv.second.first.pan) key("pan"), hex(kv.second.first.pan), out.append(",");
      key("heard_unix_ms"), num(kv.second.second);
      out.append("}");
    }
    out.append("],");
    key("ids");
    out.append("[");
    first = true;
    for (const auto &kv : tap_ids) {
      if (!first) out.append(",");
      first = false;
      out.append("{");
      key("tap"), hex(kv.first), out.append(",");
      key("mac"), str(kv.second.first), out.append(",");
      key("heard_unix_ms"), num(kv.second.second);
      out.append("}");
    }
    out.append("],");
    key("settings");
    out.append("[");
    first = true;
    for (const auto &kv : smarts) {
      if (!first) out.append(",");
      first = false;
      out.append("{");
      key("addr"), hex(kv.first), out.append(",");
      key("settings"), str(kv.second.first.settings), out.append(",");
      key("stored"), out.append(kv.second.first.stored ? "true" : "false"), out.append(",");
      key("heard_unix_ms"), num(kv.second.second);
      out.append("}");
    }
    out.append("],");
    key("mppt");
    out.append("[");
    first = true;
    for (const auto &kv : mppts) {
      if (!first) out.append(",");
      first = false;
      out.append("{");
      key("addr"), hex(kv.first), out.append(",");
      key("params"), str(kv.second.first.params), out.append(",");
      key("answer"), str(kv.second.first.answer), out.append(",");
      key("heard_unix_ms"), num(kv.second.second);
      out.append("}");
    }
    out.append("],");
    key("ratings");
    out.append("[");
    first = true;
    for (const auto &kv : cals) {
      if (!first) out.append(",");
      first = false;
      out.append("{");
      key("addr"), hex(kv.first), out.append(",");
      key("volts"), num(kv.second.first.volts), out.append(",");
      key("watts"), num(kv.second.first.watts), out.append(",");
      key("cal"), str(kv.second.first.cal), out.append(",");
      key("heard_unix_ms"), num(kv.second.second);
      out.append("}");
    }
    out.append("],");
    key("changes");
    out.append("[");
    first = true;
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
      if (!first) out.append(",");
      first = false;
      out.append("{");
      key("unix_ms"), num(it->unix_ms), out.append(",");
      key("addr"), hex(it->addr), out.append(",");
      key("what"), str(it->what), out.append(",");
      key("before"), str(it->before), out.append(",");
      key("after"), str(it->after);
      out.append("}");
    }
    out.append("]");
    if (with_pans) {
      out.append(",");
      key("owners");
      out.append("[");
      first = true;
      for (const auto &kv : owners.owners()) {
        if (!first) out.append(",");
        first = false;
        out.append("{");
        key("addr"), hex(kv.first), out.append(",");
        key("barcode"), str(kv.second);
        out.append("}");
      }
      out.append("]");
    }
    out.append("}");
  }

  VersionBook versions;
  // Each TAP's radio settings, as last read (by anyone), and when.
  Map<uint16_t, std::pair<TapRadio, int64_t>> radios;
  // Each TAP's MAC, its ID, from its last answer to GET_MAC, and when.
  Map<uint16_t, std::pair<Str, int64_t>> tap_ids;
  // Each optimizer's MPPT parameters, from its answer to ^00Mppt, and when.
  Map<uint16_t, std::pair<MpptSeen, int64_t>> mppts;
  // Each optimizer's answer to ^00Smrt, as last heard, and when.
  Map<uint16_t, std::pair<SmartSeen, int64_t>> smarts;
  // Each optimizer's rating, from its answer to ^00Cal, and when.
  Map<uint16_t, std::pair<CalSeen, int64_t>> cals;
  // Which barcode each address last answered for.
  AddressOwners owners;
  Deque<Change> changes;  // newest last
  bool dirty = false;          // changed since the owner last saved it

 private:
  void log_f_(char level, const char *fmt, ...) {
    if (!log_) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_(level, buf);
  }
  // An optimizer whose settings, MPPT parameters or firmware changed: a CCA
  // never drifts them, so each is news. The last KEEP_CHANGES are kept. Only
  // while this monitor runs the bus: under another master, the change is its.
  void change_(int64_t now, bool runs_bus, uint16_t addr, const char *what, const Str &before,
               const Str &after) {
    if (!runs_bus) {
      log_f_('I', "bus: optimizer %04X's %s is now %s, was %s (another master runs the bus)", addr, what,
             after.c_str(), before.c_str());
      return;
    }
    log_f_('W', "bus: optimizer %04X's %s changed: %s, was %s", addr, what, after.c_str(), before.c_str());
    changes.push_back({now, addr, what, before, after});
    while (changes.size() > KEEP_CHANGES) changes.pop_front();
    dirty = true;
  }
  // Before an optimizer's answer is compared with what its address said
  // before: if another optimizer holds that address now, forget the old one's.
  void check_owner_(uint16_t addr) {
    if (!barcode_of_) return;
    Str now = to_str(barcode_of_(addr));
    auto was = owners.owners().find(addr);
    Str before = was == owners.owners().end() ? "" : was->second;
    if (!owners.moved(addr, now)) {
      if (before.empty() && !now.empty()) dirty = true;  // first owner, kept
      return;
    }
    log_f_('I', "bus: address %04X is barcode %s now, was %s; its answers start afresh", addr, now.c_str(),
           before.c_str());
    smarts.erase(addr);
    mppts.erase(addr);
    cals.erase(addr);
    versions.forget(false, addr);
    dirty = true;
  }

  Log log_;
  std::function<std::string(uint16_t)> barcode_of_;
};

}  // namespace master
}  // namespace tigo_monitor
}  // namespace esphome
