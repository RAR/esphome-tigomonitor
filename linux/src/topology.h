#pragma once

// The site's layout as the ESP32 gets it from YAML and NVS: which inverters
// exist and which MPPT labels each owns (the `inverters:` block), plus the
// names and panel ratings set on the Topology page. Kept in
// <state_dir>/topology.json, apart from settings.json, so a rename never
// reconnects MQTT. Strings themselves come from the node table, except that
// a layout saved from the Topology editor also keeps its strings here, so an
// MPPT or string with no panels yet survives.

#include "monitor.h"
#include "tigo_layout.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace tigod {

struct InverterConfig {
  std::string name;          // identity; nodes reach it through its MPPT labels
  std::string display_name;  // empty: show name
  std::vector<std::string> mppts;
};

struct StringOverride {
  std::string display_label;  // empty: show the CCA label
  int panel_rating_w = 0;     // 0: not set
};

// An incoming layout held for the user, because they saved their own. Kept
// in memory only: the next sync produces it again.
struct PendingLayout {
  std::string source;  // "cca", "cloud", "import"
  std::vector<LayoutPanel> panels;  // a CCA's or the cloud's, as sent; empty for an import
  std::vector<esphome::tigo_monitor::layout::Assignment> assignments;  // as they land on the nodes
  std::vector<InverterConfig> inverters;  // empty: the source has none
  uint32_t fp = 0;
};

class Topology {
 public:
  explicit Topology(std::string path);
  void load();

  std::vector<InverterConfig> inverters() const;
  std::map<std::string, StringOverride> string_overrides() const;

  // Replaces the inverter list; display names carry over by name.
  bool set_inverters(std::vector<InverterConfig> inverters);
  // False if no inverter has that name.
  bool set_inverter_display_name(const std::string &name, const std::string &display);
  // For a backup's names: one for an inverter not configured yet is kept and
  // applied when it is added, since on the Pi the backup often comes first.
  void remember_inverter_display_name(const std::string &name, const std::string &display);
  bool set_string_display_label(const std::string &label, const std::string &display);
  bool set_string_rating(const std::string &label, int rating_w);

  // The Topology editor's layout. manual: the user saved one, so what
  // arrives later is held for them (tigo_layout.h decide()).
  bool manual() const;
  bool set_manual(bool manual);
  // The layout last turned down from each source (tigo_layout.h Declined).
  uint32_t declined_fp(const std::string &source) const;
  bool set_declined_fp(const std::string &source, uint32_t fp);
  // Inverters -> MPPTs -> strings with their names and ratings, no panels.
  esphome::tigo_monitor::layout::Layout skeleton() const;
  // Replaces the inverters and each MPPT's strings; sets the strings' names
  // and ratings to the ones given.
  bool set_skeleton(const esphome::tigo_monitor::layout::Layout &l);
  // Forgets the editor's strings (back to the node table's alone).
  bool clear_skeleton_strings();
  // MPPT label -> the name the editor gave it, for the MPPTs it renamed.
  std::map<std::string, std::string> mppt_names() const;
  void set_pending(PendingLayout p);
  std::optional<PendingLayout> pending() const;
  void clear_pending();

 private:
  bool save_locked_() const;
  std::string path_;
  mutable std::mutex mu_;
  std::vector<InverterConfig> inverters_;
  std::map<std::string, std::string> pending_names_;  // inverter name -> display name
  std::map<std::string, StringOverride> strings_;
  std::map<std::string, std::vector<std::string>> mppt_strings_;  // MPPT label -> string labels, in order
  std::map<std::string, std::string> mppt_names_;  // MPPT label -> display name, unique within its inverter
  bool manual_ = false;
  esphome::tigo_monitor::layout::Declined declined_;
  std::optional<PendingLayout> pending_;
};

}  // namespace tigod
