#pragma once

// The site's layout as the ESP32 gets it from YAML and NVS: which inverters
// exist and which MPPT labels each owns (the `inverters:` block), plus the
// names and panel ratings set on the Topology page. Kept in
// <state_dir>/topology.json, apart from settings.json, so a rename never
// reconnects MQTT. Strings themselves come from the node table.

#include <map>
#include <mutex>
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

 private:
  bool save_locked_() const;
  std::string path_;
  mutable std::mutex mu_;
  std::vector<InverterConfig> inverters_;
  std::map<std::string, std::string> pending_names_;  // inverter name -> display name
  std::map<std::string, StringOverride> strings_;
};

}  // namespace tigod
