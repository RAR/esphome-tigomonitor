// Persisted user configuration — lets a handful of runtime knobs be edited in the web UI
// and stored on-device (NVS) instead of living only in the YAML. The YAML values remain the
// defaults: at boot the codegen setters populate the member vars with the YAML values, then
// tigo_config_load() captures those as the defaults and overlays any stored overrides. Each
// field can be reverted to its YAML default, which clears the override so future YAML edits
// take effect again.
//
// NVS is plaintext at rest; nothing sensitive is stored here (no auth/creds — see tigo_cloud).

#include "tigo_monitor.h"

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace esphome {
namespace tigo_monitor {

static const char *const CFG_TAG = "tigo_monitor.config";

// Override bitmask — one bit per user-settable field.
enum : uint32_t {
  CFG_PCAL = 1u << 0,
  CFG_NIGHT = 1u << 1,
  CFG_RESETMID = 1u << 2,
  CFG_SYNCCCA = 1u << 3,
  CFG_CCAIP = 1u << 4,
};

// Persisted blob. `magic` guards against reading uninitialised/foreign NVS.
struct TigoConfig {
  uint32_t magic;
  uint32_t overrides;     // which fields the user has overridden
  float power_calibration;
  uint32_t night_mode_min;  // minutes (UI unit); member var is ms
  uint8_t reset_at_midnight;
  uint8_t sync_cca_on_startup;
  uint8_t _pad[2];
  char cca_ip[64];
};

static constexpr uint32_t CFG_MAGIC = 0x71C0C0F9;  // "tigo cfg"
static uint32_t tigo_config_hash() { return fnv1_hash("tigo_user_config_v1"); }

// ---------------------------------------------------------------------------
void TigoMonitorComponent::tigo_config_load() {
  // Snapshot the YAML-provided values as the revert-to-default baseline. setup() runs after
  // the codegen set_* calls, so the members already hold the YAML values here.
  cfg_def_power_calibration_ = power_calibration_;
  cfg_def_night_mode_timeout_ = night_mode_timeout_;
  cfg_def_reset_at_midnight_ = reset_at_midnight_;
  cfg_def_sync_cca_on_startup_ = sync_cca_on_startup_;
  cfg_def_cca_ip_ = cca_ip_;

  TigoConfig c{};
  auto pref = this->cached_pref_<TigoConfig>(tigo_config_hash());
  if (!pref.load(&c) || c.magic != CFG_MAGIC) {
    cfg_overrides_ = 0;
    return;
  }
  cfg_overrides_ = c.overrides;

  if (c.overrides & CFG_PCAL) power_calibration_ = c.power_calibration;
  if (c.overrides & CFG_NIGHT) night_mode_timeout_ = (unsigned long) c.night_mode_min * 60000UL;
  if (c.overrides & CFG_RESETMID) reset_at_midnight_ = c.reset_at_midnight != 0;
  if (c.overrides & CFG_SYNCCCA) sync_cca_on_startup_ = c.sync_cca_on_startup != 0;
  if (c.overrides & CFG_CCAIP) {
    c.cca_ip[sizeof(c.cca_ip) - 1] = '\0';
    cca_ip_ = c.cca_ip;
  }
  ESP_LOGI(CFG_TAG, "Restored user config (overrides 0x%02X)", (unsigned) cfg_overrides_);
}

void TigoMonitorComponent::tigo_config_save_() {
  TigoConfig c{};
  c.magic = CFG_MAGIC;
  c.overrides = cfg_overrides_;
  c.power_calibration = power_calibration_;
  c.night_mode_min = (uint32_t) (night_mode_timeout_ / 60000UL);
  c.reset_at_midnight = reset_at_midnight_ ? 1 : 0;
  c.sync_cca_on_startup = sync_cca_on_startup_ ? 1 : 0;
  strncpy(c.cca_ip, cca_ip_.c_str(), sizeof(c.cca_ip) - 1);
  auto pref = this->cached_pref_<TigoConfig>(tigo_config_hash());
  pref.save(&c);
  ESP_LOGD(CFG_TAG, "User config persisted (overrides 0x%02X)", (unsigned) cfg_overrides_);
}

// JSON describing every field: current value, YAML default, and whether it's overridden.
// Consumed by the Tools > Device Configuration card.
std::string TigoMonitorComponent::tigo_config_json() {
  // "overridden" reflects whether the live value actually differs from the YAML/boot default
  // — not merely that an override was once saved. A stored value equal to the default reads as
  // not-overridden (and Revert stays disabled), so a field shows the tag only when it really
  // diverges from the config.
  float pcal_d = power_calibration_ - cfg_def_power_calibration_;
  bool pcal_ov = pcal_d < -1e-6f || pcal_d > 1e-6f;
  bool night_ov = night_mode_timeout_ != cfg_def_night_mode_timeout_;
  bool reset_ov = reset_at_midnight_ != cfg_def_reset_at_midnight_;
  bool sync_ov = sync_cca_on_startup_ != cfg_def_sync_cca_on_startup_;
  bool ip_ov = cca_ip_ != cfg_def_cca_ip_;
  char buf[640];
  snprintf(buf, sizeof(buf),
           "{"
           "\"power_calibration\":{\"value\":%.3f,\"default\":%.3f,\"overridden\":%s},"
           "\"night_mode_timeout\":{\"value\":%lu,\"default\":%lu,\"overridden\":%s},"
           "\"reset_at_midnight\":{\"value\":%s,\"default\":%s,\"overridden\":%s},"
           "\"sync_cca_on_startup\":{\"value\":%s,\"default\":%s,\"overridden\":%s},"
           "\"cca_ip\":{\"value\":\"%s\",\"default\":\"%s\",\"overridden\":%s}"
           "}",
           power_calibration_, cfg_def_power_calibration_, pcal_ov ? "true" : "false",
           night_mode_timeout_ / 60000UL, cfg_def_night_mode_timeout_ / 60000UL,
           night_ov ? "true" : "false",
           reset_at_midnight_ ? "true" : "false", cfg_def_reset_at_midnight_ ? "true" : "false",
           reset_ov ? "true" : "false",
           sync_cca_on_startup_ ? "true" : "false", cfg_def_sync_cca_on_startup_ ? "true" : "false",
           sync_ov ? "true" : "false",
           cca_ip_.c_str(), cfg_def_cca_ip_.c_str(), ip_ov ? "true" : "false");
  return std::string(buf);
}

static bool parse_bool_(const std::string &v) {
  return v == "1" || v == "true" || v == "on" || v == "yes";
}

// Apply a single field from the web UI: validate, set the live member, mark it overridden,
// persist. power_calibration takes effect immediately (every power calc reads the member).
bool TigoMonitorComponent::tigo_config_apply(const std::string &key, const std::string &value) {
  if (key == "power_calibration") {
    float f = strtof(value.c_str(), nullptr);
    if (f < 0.5f || f > 2.0f) return false;
    power_calibration_ = f;
    cfg_overrides_ |= CFG_PCAL;
  } else if (key == "night_mode_timeout") {
    long m = strtol(value.c_str(), nullptr, 10);
    if (m < 1 || m > 1440) return false;
    night_mode_timeout_ = (unsigned long) m * 60000UL;
    cfg_overrides_ |= CFG_NIGHT;
  } else if (key == "reset_at_midnight") {
    reset_at_midnight_ = parse_bool_(value);
    cfg_overrides_ |= CFG_RESETMID;
  } else if (key == "sync_cca_on_startup") {
    sync_cca_on_startup_ = parse_bool_(value);
    cfg_overrides_ |= CFG_SYNCCCA;
  } else if (key == "cca_ip") {
    if (value.size() >= 64) return false;
    cca_ip_ = value;
    cfg_overrides_ |= CFG_CCAIP;
  } else {
    return false;
  }
  tigo_config_save_();
  ESP_LOGI(CFG_TAG, "Set %s = %s", key.c_str(), value.c_str());
  return true;
}

// Revert a field to its YAML default and clear the override (so future YAML edits apply).
bool TigoMonitorComponent::tigo_config_reset(const std::string &key) {
  if (key == "power_calibration") {
    power_calibration_ = cfg_def_power_calibration_;
    cfg_overrides_ &= ~CFG_PCAL;
  } else if (key == "night_mode_timeout") {
    night_mode_timeout_ = cfg_def_night_mode_timeout_;
    cfg_overrides_ &= ~CFG_NIGHT;
  } else if (key == "reset_at_midnight") {
    reset_at_midnight_ = cfg_def_reset_at_midnight_;
    cfg_overrides_ &= ~CFG_RESETMID;
  } else if (key == "sync_cca_on_startup") {
    sync_cca_on_startup_ = cfg_def_sync_cca_on_startup_;
    cfg_overrides_ &= ~CFG_SYNCCCA;
  } else if (key == "cca_ip") {
    cca_ip_ = cfg_def_cca_ip_;
    cfg_overrides_ &= ~CFG_CCAIP;
  } else {
    return false;
  }
  tigo_config_save_();
  ESP_LOGI(CFG_TAG, "Reverted %s to default", key.c_str());
  return true;
}

}  // namespace tigo_monitor
}  // namespace esphome
