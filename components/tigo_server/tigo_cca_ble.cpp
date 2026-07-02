// CCA-over-BLE client, folded into tigo_server.
//
// Compiled only when cca_source: ble/auto is configured (USE_TIGO_CCA_BLE). In that
// case TigoWebServer is itself a ble_client::BLEClientNode and talks the Tigo CCA's
// mobile_api over GATT, so the CCA Info page works when the CCA's local HTTP API is
// locked down (firmware 4.0.4+). Protocol verified against the Tigo app 5.4.6:
//   request frame  = chr(reqNum) + "cmd=X[ sid=Y]" + '\0'  (single-byte ASCII,
//                    space-separated params, one write-with-response / long write)
//   session key    = SHA512((uts + 159260).toString())  — uts is a nonce from DEVICE_INFO
//   response       = byte0 reqNum, body from byte3, accumulate until trailing '\0'
//
// Vendored from https://github.com/RAR/esphome-tigo-ble (component tigo_ble); the HA
// entity publishing was dropped — the CCA Info page reads the cached DEVICE_INFO.

#include "tigo_web_server.h"

#ifdef USE_TIGO_CCA_BLE

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
// ESP-IDF 6.0 ships mbedtls 4.0, which removed the legacy mbedtls_sha512_* API and
// its public "mbedtls/sha512.h" header — hashing moved to the PSA Crypto API. Use PSA
// on 6.0+, the legacy mbedtls one-shot on 5.x. (Mirrors esphome's own sha256 component.)
#include <esp_idf_version.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#include <psa/crypto.h>
#else
#include "mbedtls/sha512.h"
#endif
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace esphome {
namespace tigo_server {

static const char *const BLE_TAG = "tigo_server.ble";
static const uint32_t CCA_SESSION_SEED = 159260;  // app constant (eZ) for the session key

// Tigo service UUID d3dadcba-e4fa-4016-ba33-8bcc671999a7 (little-endian bytes)
static const uint8_t CCA_SERVICE_UUID_BYTES[16] = {
    0xa7, 0x99, 0x19, 0x67, 0xcc, 0x8b, 0x33, 0xba,
    0x16, 0x40, 0xfa, 0xe4, 0xba, 0xdc, 0xda, 0xd3};

// ---------------------------------------------------------------------------
// Lifecycle (called from TigoWebServer::setup()/loop())
// ---------------------------------------------------------------------------

void TigoWebServer::ble_setup_() {
  ESP_LOGCONFIG(BLE_TAG, "CCA-over-BLE client enabled (cca_source: ble/auto)");
  // Capture the compile-time MAC, then overlay any user-selected MAC from NVS.
  this->ble_load_mac_();
  // The advertisement listener is registered at codegen time (esp32_ble_tracker
  // .register_ble_device in __init__.py) so parse_device() is actually dispatched —
  // a runtime register_listener() is compiled out unless the LISTENER_COUNT macro is set.
}

void TigoWebServer::ble_loop_() {
  // Web-server task asked for a refresh — run it here so the connect()/queue calls
  // happen on the main loop, not the httpd task.
  if (this->ble_refresh_requested_.exchange(false)) {
    this->cca_ble_refresh_once();
  }
  if (this->ble_discovery_start_requested_.exchange(false)) {
    this->cca_start_discovery();
  }
  if (this->ble_discovery_poll_requested_.exchange(false)) {
    this->cca_poll_discovery_status();
  }
  // Drain one queued network command per loop, and only when idle, so requests don't
  // stack overlapping connects (each is a one-shot connect → … → auto-disconnect).
  if (!this->ble_connected_) {
    std::string net_cmd, net_params;
    {
      std::lock_guard<std::mutex> lock(this->ble_net_req_mutex_);
      if (!this->ble_net_requests_.empty()) {
        net_cmd = this->ble_net_requests_.front().first;
        net_params = this->ble_net_requests_.front().second;
        this->ble_net_requests_.erase(this->ble_net_requests_.begin());
      }
    }
    if (!net_cmd.empty())
      this->cca_run_network_command_(net_cmd, net_params);
  }

  // Drain queued commands once the link is ready.
  if (this->ble_connected_ && this->ble_ready_ && !this->ble_command_queue_.empty()) {
    std::string cmd = this->ble_command_queue_.front();
    this->ble_command_queue_.erase(this->ble_command_queue_.begin());
    this->ble_write_command_(cmd);
  }

  // Fire a deferred (post-auth) command when its delay elapses.
  if (this->ble_deferred_time_ > 0 && millis() >= this->ble_deferred_time_) {
    this->ble_write_command_(this->ble_deferred_command_, this->ble_deferred_params_);
    this->ble_deferred_time_ = 0;
    this->ble_deferred_command_.clear();
    this->ble_deferred_params_.clear();
  }

  if (this->ble_awaiting_response_ && (millis() - this->ble_last_command_time_ > 60000)) {
    ESP_LOGW(BLE_TAG, "Response timeout — no reply for request %d", this->ble_last_command_);
    this->ble_awaiting_response_ = false;
  }

  // Deferred auto-disconnect after a one-shot refresh (kept out of the BLE callback).
  if (this->ble_disconnect_at_ > 0 && millis() >= this->ble_disconnect_at_) {
    this->ble_disconnect_at_ = 0;
    ESP_LOGD(BLE_TAG, "One-shot refresh complete — disconnecting");
    this->cca_ble_disconnect();
  }
}

// ---------------------------------------------------------------------------
// GATT events
// ---------------------------------------------------------------------------

void TigoWebServer::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      this->ble_handle_connect_(param);
      break;
    case ESP_GATTC_DISCONNECT_EVT:
      this->ble_handle_disconnect_();
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
      this->ble_handle_service_discovery_();
      break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      if (param->reg_for_notify.status == ESP_GATT_OK) {
        ESP_LOGI(BLE_TAG, "Notifications enabled for handle 0x%04x", param->reg_for_notify.handle);
      } else {
        ESP_LOGW(BLE_TAG, "Notification registration failed: %d", param->reg_for_notify.status);
      }
      break;
    case ESP_GATTC_WRITE_DESCR_EVT:
      if (param->write.status == ESP_GATT_OK) {
        ESP_LOGI(BLE_TAG, "CCCD write success — ready for commands");
        this->ble_ready_ = true;
      } else {
        ESP_LOGW(BLE_TAG, "CCCD write failed: %d", param->write.status);
      }
      break;
    case ESP_GATTC_WRITE_CHAR_EVT:
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(BLE_TAG, "Write failed: %d", param->write.status);
      }
      break;
    case ESP_GATTC_NOTIFY_EVT:
      this->ble_handle_notification_(param);
      break;
    case ESP_GATTC_READ_CHAR_EVT:
      if (param->read.status == ESP_GATT_OK) {
        std::string data((char *) param->read.value, param->read.value_len);
        this->ble_process_response_(data);
      }
      break;
    default:
      break;
  }
}

void TigoWebServer::ble_handle_connect_(esp_ble_gattc_cb_param_t *param) {
  if (param->open.status == ESP_GATT_OK) {
    ESP_LOGI(BLE_TAG, "Connected to Tigo CCA");
    this->ble_connected_ = true;
  } else {
    ESP_LOGW(BLE_TAG, "Connection failed: %d", param->open.status);
  }
}

void TigoWebServer::ble_handle_disconnect_() {
  ESP_LOGI(BLE_TAG, "Disconnected from Tigo CCA");
  this->ble_connected_ = false;
  this->ble_ready_ = false;
  this->ble_response_buffer_.clear();
}

// ---------------------------------------------------------------------------
// Service / characteristic discovery
// ---------------------------------------------------------------------------

void TigoWebServer::ble_handle_service_discovery_() {
  uint16_t count = 0;
  auto status = esp_ble_gattc_get_attr_count(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                             ESP_GATT_DB_PRIMARY_SERVICE, 0, 0xFFFF,
                                             ESP_GATT_ILLEGAL_HANDLE, &count);
  if (status != ESP_GATT_OK || count == 0) {
    ESP_LOGW(BLE_TAG, "No services found, using fallback handles");
    this->ble_setup_notifications_fallback_();
    return;
  }

  esp_gattc_service_elem_t services[count];
  status = esp_ble_gattc_get_service(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                     nullptr, services, &count, 0);
  if (status != ESP_GATT_OK) {
    ESP_LOGW(BLE_TAG, "Failed to enumerate services");
    this->ble_setup_notifications_fallback_();
    return;
  }

  uint16_t svc_start = 0, svc_end = 0;
  for (uint16_t i = 0; i < count; i++) {
    if (services[i].uuid.len == ESP_UUID_LEN_128 &&
        memcmp(services[i].uuid.uuid.uuid128, CCA_SERVICE_UUID_BYTES, 16) == 0) {
      svc_start = services[i].start_handle;
      svc_end = services[i].end_handle;
      break;
    }
  }
  if (svc_start == 0) {
    ESP_LOGW(BLE_TAG, "Tigo service not found, using fallback handles");
    this->ble_setup_notifications_fallback_();
    return;
  }
  this->ble_discover_characteristics_(svc_start, svc_end);
}

void TigoWebServer::ble_discover_characteristics_(uint16_t svc_start, uint16_t svc_end) {
  uint16_t count = 0;
  auto status = esp_ble_gattc_get_attr_count(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                             ESP_GATT_DB_CHARACTERISTIC, svc_start, svc_end,
                                             ESP_GATT_ILLEGAL_HANDLE, &count);
  if (status != ESP_GATT_OK || count == 0) {
    ESP_LOGW(BLE_TAG, "No characteristics found");
    this->ble_setup_notifications_fallback_();
    return;
  }

  esp_gattc_char_elem_t chars[count];
  status = esp_ble_gattc_get_all_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                      svc_start, svc_end, chars, &count, 0);
  if (status != ESP_GATT_OK) {
    ESP_LOGW(BLE_TAG, "Failed to enumerate characteristics");
    this->ble_setup_notifications_fallback_();
    return;
  }

  for (uint16_t i = 0; i < count; i++) {
    if (chars[i].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY)
      this->ble_char_notify_ = chars[i].char_handle;
    if (chars[i].properties & ESP_GATT_CHAR_PROP_BIT_WRITE)
      this->ble_char_write_ = chars[i].char_handle;
  }
  if (this->ble_char_notify_ == 0 || this->ble_char_write_ == 0) {
    ESP_LOGW(BLE_TAG, "Missing characteristics, using fallback handles");
    this->ble_setup_notifications_fallback_();
    return;
  }
  this->ble_setup_notifications_(svc_start, svc_end);
}

void TigoWebServer::ble_setup_notifications_(uint16_t svc_start, uint16_t svc_end) {
  esp_ble_gattc_register_for_notify(this->parent()->get_gattc_if(), this->parent()->get_remote_bda(),
                                    this->ble_char_notify_);

  uint16_t cccd_handle = 0, count = 0;
  esp_bt_uuid_t svc_uuid;
  svc_uuid.len = ESP_UUID_LEN_128;
  memcpy(svc_uuid.uuid.uuid128, CCA_SERVICE_UUID_BYTES, 16);

  auto status = esp_ble_gattc_get_attr_count(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                             ESP_GATT_DB_DESCRIPTOR, svc_start, svc_end,
                                             this->ble_char_notify_, &count);
  if (status == ESP_GATT_OK && count > 0) {
    esp_gattc_descr_elem_t descs[count];
    status = esp_ble_gattc_get_descr_by_char_handle(this->parent()->get_gattc_if(),
                                                    this->parent()->get_conn_id(), this->ble_char_notify_,
                                                    svc_uuid, descs, &count);
    if (status == ESP_GATT_OK) {
      for (uint16_t i = 0; i < count; i++) {
        if (descs[i].uuid.len == ESP_UUID_LEN_16 && descs[i].uuid.uuid.uuid16 == 0x2902) {
          cccd_handle = descs[i].handle;
          break;
        }
      }
    }
  }
  if (cccd_handle == 0)
    cccd_handle = this->ble_char_notify_ + 1;  // CCCD typically follows the characteristic
  this->ble_write_cccd_(cccd_handle);
}

void TigoWebServer::ble_setup_notifications_fallback_() {
  this->ble_char_notify_ = 0x1A42;
  this->ble_char_write_ = 0x1A44;
  esp_ble_gattc_register_for_notify(this->parent()->get_gattc_if(), this->parent()->get_remote_bda(),
                                    this->ble_char_notify_);
  this->ble_write_cccd_(0x1A43);
}

void TigoWebServer::ble_write_cccd_(uint16_t cccd_handle) {
  uint8_t enable[] = {0x01, 0x00};
  esp_ble_gattc_write_char_descr(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), cccd_handle,
                                 sizeof(enable), enable, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
}

// ---------------------------------------------------------------------------
// Notification handling
// ---------------------------------------------------------------------------

void TigoWebServer::ble_handle_notification_(esp_ble_gattc_cb_param_t *param) {
  // Response: byte0 reqNum, bytes1-2 overhead, body from byte3, ending with '\0'.
  // Full dumps at VERBOSE only — logging a ~1.7 KB response at INFO stalled the loop.
  ESP_LOGV(BLE_TAG, "<<< Notify: handle=0x%04x, len=%d", param->notify.handle, param->notify.value_len);
  if (param->notify.value_len > 0)
    ESP_LOG_BUFFER_HEX_LEVEL(BLE_TAG, param->notify.value, param->notify.value_len, ESP_LOG_VERBOSE);

  if (param->notify.value_len < 4) {
    ESP_LOGW(BLE_TAG, "Short notification (%d bytes)", param->notify.value_len);
    return;
  }

  const uint8_t *body = &param->notify.value[3];
  size_t body_len = param->notify.value_len - 3;
  this->ble_response_buffer_.insert(this->ble_response_buffer_.end(), body, body + body_len);

  if (!this->ble_response_buffer_.empty() && this->ble_response_buffer_.back() == '\0') {
    this->ble_awaiting_response_ = false;
    this->ble_response_buffer_.pop_back();
    std::string json(this->ble_response_buffer_.begin(), this->ble_response_buffer_.end());
    ESP_LOGD(BLE_TAG, "Response to '%s' (%d bytes)", this->ble_last_command_.c_str(), json.length());
    const size_t chunk = 200;
    for (size_t i = 0; i < json.length(); i += chunk)
      ESP_LOGV(BLE_TAG, "%s", json.substr(i, chunk).c_str());
    this->ble_process_response_(json);
    this->ble_response_buffer_.clear();
  }
}

// ---------------------------------------------------------------------------
// Public command API
// ---------------------------------------------------------------------------

void TigoWebServer::cca_ble_connect() {
  // set_enabled() only flips a gate — connect() is what actually opens the GATT
  // link (the ble_client.connect action does the same). Must run on the main loop.
  // Make sure the link targets the user-selected MAC (defensive against setup ordering).
  if (this->ble_saved_mac_ != 0 && this->parent()->get_address() != this->ble_saved_mac_)
    this->parent()->set_address(this->ble_saved_mac_);
  ESP_LOGI(BLE_TAG, "Connecting to CCA over BLE (%s)...", this->parent()->address_str());
  this->parent()->set_enabled(true);
  this->parent()->connect();
}

void TigoWebServer::cca_ble_disconnect() {
  ESP_LOGI(BLE_TAG, "Disconnecting from CCA...");
  this->parent()->set_enabled(false);  // flips enabled true→false, which disconnects
  this->ble_connected_ = false;
  this->ble_ready_ = false;
  this->ble_command_queue_.clear();
}

void TigoWebServer::ble_queue_command_(const std::string &cmd) { this->ble_command_queue_.push_back(cmd); }

void TigoWebServer::cca_device_ping() { this->ble_queue_command_("DEVICE_PING"); }
void TigoWebServer::cca_device_info() { this->ble_queue_command_("DEVICE_INFO"); }

void TigoWebServer::cca_ble_refresh_once() {
  // Self-contained refresh for the dashboard button: connect, pull DEVICE_INFO
  // (cached for the page + yields the session key) then NETWORK_INFO (protected),
  // then drop the link so the CCA is free for the phone app. send_protected queues
  // DEVICE_INFO first, so this gets both in one connection; the auto-disconnect is
  // armed once the last response lands (see ble_process_response_).
  ESP_LOGI(BLE_TAG, "One-shot CCA refresh over BLE (DEVICE_INFO + NETWORK_INFO)...");
  this->ble_auto_disconnect_ = true;
  this->cca_ble_connect();
  this->cca_send_protected("NETWORK_INFO");
}

void TigoWebServer::cca_start_discovery() {
  // Config action: kick a topology rescan on the CCA. One-shot like the refresh —
  // connect, DEVICE_INFO for a fresh sid, fire START_DISCOVERY, drop the link so the
  // scan runs on the CCA on its own. Progress is read separately via DISCOVERY_STATUS.
  ESP_LOGI(BLE_TAG, "Triggering CCA topology discovery over BLE (START_DISCOVERY)...");
  this->ble_auto_disconnect_ = true;
  this->cca_ble_connect();
  this->cca_send_protected("START_DISCOVERY");
}

void TigoWebServer::cca_poll_discovery_status() {
  // Read scan progress: connect, fresh sid, DISCOVERY_STATUS, cache the payload, drop
  // the link. Called repeatedly by the UI while a discovery is running.
  ESP_LOGI(BLE_TAG, "Polling CCA discovery status over BLE (DISCOVERY_STATUS)...");
  this->ble_auto_disconnect_ = true;
  this->cca_ble_connect();
  this->cca_send_protected("DISCOVERY_STATUS");
}

void TigoWebServer::cca_send_protected(const std::string &command, const std::string &extra_params) {
  // Proven auth pattern: fetch a fresh DEVICE_INFO (gets the uts nonce), derive the
  // sid, then send `command` with that sid (plus any extra, already-encoded params)
  // while it's fresh.
  ESP_LOGI(BLE_TAG, "Protected command '%s': fetching DEVICE_INFO for sid...", command.c_str());
  this->ble_pending_protected_cmd_ = command;
  this->ble_pending_protected_params_ = extra_params;
  this->cca_device_info();
}

void TigoWebServer::cca_run_network_command_(const std::string &command, const std::string &params) {
  // One-shot like the refresh/discovery paths: connect, fresh sid, fire the command,
  // cache the reply by name, drop the link. Runs on the main loop (BLE stack calls).
  ESP_LOGI(BLE_TAG, "Network command over BLE: %s", command.c_str());
  this->ble_auto_disconnect_ = true;
  this->cca_ble_connect();
  this->cca_send_protected(command, params);
}

void TigoWebServer::ble_enqueue_net_request_(const std::string &command, const std::string &params) {
  std::lock_guard<std::mutex> lock(this->ble_net_req_mutex_);
  this->ble_net_requests_.emplace_back(command, params);
}

// ---------------------------------------------------------------------------
// Command writing
// ---------------------------------------------------------------------------

void TigoWebServer::ble_write_command_(const std::string &cmd, const std::string &params) {
  if (!this->ble_connected_) {
    ESP_LOGW(BLE_TAG, "Not connected");
    return;
  }

  // Remember which command this response belongs to (commands are serialized).
  this->ble_last_command_ = cmd;

  this->ble_request_num_++;
  if (this->ble_request_num_ == 0)
    this->ble_request_num_ = 1;

  std::string text = "cmd=" + cmd;
  if (!params.empty())
    text += " " + params;

  // Plain single-byte ASCII: [reqNum] + "cmd=... [params]" + 0x00 (verified app 5.4.6).
  std::vector<uint8_t> payload;
  payload.reserve(1 + text.size() + 1);
  payload.push_back(this->ble_request_num_);
  for (char c : text)
    payload.push_back(static_cast<uint8_t>(c));
  payload.push_back(0x00);

  ESP_LOGD(BLE_TAG, ">>> Send [%d]: %s (%d bytes)", this->ble_request_num_, text.c_str(), payload.size());
  ESP_LOG_BUFFER_HEX_LEVEL(BLE_TAG, payload.data(), std::min(payload.size(), (size_t) 64), ESP_LOG_VERBOSE);

  this->ble_awaiting_response_ = true;
  this->ble_last_command_time_ = millis();
  this->ble_send_payload_(payload);
}

void TigoWebServer::ble_send_payload_(const std::vector<uint8_t> &payload) {
  if (this->ble_char_write_ == 0) {
    ESP_LOGW(BLE_TAG, "Write handle not set");
    return;
  }
  // One write-with-response (long write if > MTU-3); the CCA reassembles into one value.
  auto status = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                         this->ble_char_write_, payload.size(),
                                         const_cast<uint8_t *>(payload.data()), ESP_GATT_WRITE_TYPE_RSP,
                                         ESP_GATT_AUTH_REQ_NONE);
  if (status != ESP_GATT_OK)
    ESP_LOGW(BLE_TAG, "Write failed (%d bytes): %d", payload.size(), status);
}

// ---------------------------------------------------------------------------
// Session key + response parsing
// ---------------------------------------------------------------------------

void TigoWebServer::ble_generate_session_key_(uint32_t uts) {
  // sid = SHA512((uts + 159260).toString()) -> 128-char lowercase hex.
  uint64_t sum = static_cast<uint64_t>(uts) + CCA_SESSION_SEED;
  std::string sum_str = std::to_string(sum);

  unsigned char hash[64];
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
  // PSA one-shot SHA-512 (mbedtls 4.0 / IDF 6.0+). PSA is auto-initialized by ESP-IDF at startup.
  size_t hash_len = 0;
  psa_hash_compute(PSA_ALG_SHA_512, reinterpret_cast<const unsigned char *>(sum_str.c_str()),
                   sum_str.length(), hash, sizeof(hash), &hash_len);
#else
  mbedtls_sha512(reinterpret_cast<const unsigned char *>(sum_str.c_str()), sum_str.length(), hash, 0);
#endif
  char hex[129];
  for (int i = 0; i < 64; i++)
    sprintf(hex + i * 2, "%02x", hash[i]);
  hex[128] = '\0';
  this->ble_session_key_ = std::string(hex);
  ESP_LOGD(BLE_TAG, "Session key generated from uts=%u", uts);
}

void TigoWebServer::ble_process_response_(const std::string &data) {
  // The CCA responses are small, controlled JSON. We don't parse them server-side —
  // the cache holds the raw string and the CCA Info page parses it in the browser.
  // We only need two things here: spot the auth-failure reply, and lift `uts`.

  // A bad/expired sid comes back as {"msg":"Unauthorized request"}. Match that string
  // specifically — action commands (WIFI_CONNECT/CLEAR, RUN_TEST) legitimately report
  // success/failure in a "msg" field, so the old "any msg = rejected" test would have
  // wrongly dropped them.
  if (data.find("Unauthorized request") != std::string::npos) {
    ESP_LOGW(BLE_TAG, "CCA rejected '%s' (bad/expired sid): %s", this->ble_last_command_.c_str(),
             data.c_str());
    return;
  }

  if (this->ble_last_command_ == "NETWORK_INFO") {
    this->ble_store_network_(data);
    ESP_LOGI(BLE_TAG, "CCA NETWORK_INFO cached (%d bytes)", (int) data.length());
    this->ble_arm_auto_disconnect_();  // last command of the refresh — drop the link
    return;
  }

  if (this->ble_last_command_ == "DISCOVERY_STATUS") {
    this->ble_store_discovery_(data);
    ESP_LOGI(BLE_TAG, "CCA DISCOVERY_STATUS cached (%d bytes)", (int) data.length());
    this->ble_arm_auto_disconnect_();
    return;
  }

  if (this->ble_last_command_ == "START_DISCOVERY") {
    // Ack only — progress is read separately via DISCOVERY_STATUS. Cache it too so the
    // UI can confirm the kickoff, then drop the link and let the scan run on the CCA.
    this->ble_store_discovery_(data);
    ESP_LOGI(BLE_TAG, "CCA discovery started: %s", data.c_str());
    this->ble_arm_auto_disconnect_();
    return;
  }

  if (this->ble_last_command_ != "DEVICE_INFO") {
    // Any other protected command (network reads/writes: WIFI_STATUS/SCAN, CELLULAR_INFO,
    // RUN_TEST, WIFI_CONNECT/CLEAR) — cache the reply by command name for the Network
    // panel, then drop the link.
    this->ble_store_net_result_(this->ble_last_command_, data);
    ESP_LOGI(BLE_TAG, "CCA '%s' result cached (%d bytes)", this->ble_last_command_.c_str(),
             (int) data.length());
    this->ble_arm_auto_disconnect_();
    return;
  }

  // Cache the raw DEVICE_INFO for the CCA Info page (same shape the page renders).
  this->ble_store_cca_info_(data);
  ESP_LOGI(BLE_TAG, "CCA DEVICE_INFO cached (%d bytes) — CCA Info page updated", (int) data.length());

  // Auth: extract the uts nonce, derive the sid, and fire any queued protected command.
  size_t uts_pos = data.find("\"uts\":");
  if (uts_pos != std::string::npos) {
    size_t val_start = data.find_first_of("0123456789", uts_pos + 6);
    if (val_start != std::string::npos) {
      size_t val_end = data.find_first_not_of("0123456789", val_start);
      uint32_t uts =
          static_cast<uint32_t>(strtoul(data.substr(val_start, val_end - val_start).c_str(), nullptr, 10));
      this->ble_generate_session_key_(uts);
      if (!this->ble_pending_protected_cmd_.empty()) {
        std::string cmd = this->ble_pending_protected_cmd_;
        std::string extra = this->ble_pending_protected_params_;
        this->ble_pending_protected_cmd_.clear();
        this->ble_pending_protected_params_.clear();
        ESP_LOGI(BLE_TAG, "Auth ready; scheduling %s in 100ms", cmd.c_str());
        this->ble_deferred_command_ = cmd;
        // Extra params (already URI-encoded) come before sid, matching the app's order.
        this->ble_deferred_params_ =
            extra.empty() ? ("sid=" + this->ble_session_key_) : (extra + " sid=" + this->ble_session_key_);
        this->ble_deferred_time_ = millis() + 100;
      }
    }
  }

  // Arm the deferred disconnect only if nothing further is queued (a protected
  // command like NETWORK_INFO follows DEVICE_INFO; let that one disconnect instead).
  if (this->ble_pending_protected_cmd_.empty() && this->ble_deferred_command_.empty())
    this->ble_arm_auto_disconnect_();
}

void TigoWebServer::ble_arm_auto_disconnect_() {
  // Deferred (run from ble_loop_, not the BLE callback) so the I/O settles first.
  if (this->ble_auto_disconnect_) {
    this->ble_auto_disconnect_ = false;
    this->ble_disconnect_at_ = millis() + 500;
  }
}

// ---------------------------------------------------------------------------
// Cached CCA info (read by build_cca_info_json)
// ---------------------------------------------------------------------------

void TigoWebServer::ble_store_cca_info_(const std::string &raw_device_info) {
  std::lock_guard<std::mutex> lock(this->cca_info_mutex_);
  this->cca_info_json_ = raw_device_info;
  this->cca_info_time_ = millis();
}

void TigoWebServer::ble_store_network_(const std::string &raw_network_info) {
  std::lock_guard<std::mutex> lock(this->cca_info_mutex_);
  this->cca_network_json_ = raw_network_info;
}

std::string TigoWebServer::ble_get_network_json_() {
  std::lock_guard<std::mutex> lock(this->cca_info_mutex_);
  return this->cca_network_json_;
}

void TigoWebServer::ble_store_discovery_(const std::string &raw_discovery_status) {
  std::lock_guard<std::mutex> lock(this->cca_info_mutex_);
  this->cca_discovery_json_ = raw_discovery_status;
  this->cca_discovery_time_ = millis();
}

std::string TigoWebServer::ble_get_discovery_json_() {
  std::lock_guard<std::mutex> lock(this->cca_info_mutex_);
  return this->cca_discovery_json_;
}

uint32_t TigoWebServer::ble_get_discovery_seconds_ago_() {
  std::lock_guard<std::mutex> lock(this->cca_info_mutex_);
  if (this->cca_discovery_time_ == 0)
    return 0;
  return (millis() - this->cca_discovery_time_) / 1000;
}

void TigoWebServer::ble_store_net_result_(const std::string &command, const std::string &json) {
  std::lock_guard<std::mutex> lock(this->cca_info_mutex_);
  this->cca_net_results_[command] = json;
  this->cca_net_times_[command] = millis();
}

std::string TigoWebServer::ble_get_net_result_(const std::string &command, uint32_t &age_s) {
  std::lock_guard<std::mutex> lock(this->cca_info_mutex_);
  auto it = this->cca_net_results_.find(command);
  if (it == this->cca_net_results_.end()) {
    age_s = 0;
    return "";
  }
  auto t = this->cca_net_times_.find(command);
  age_s = (t != this->cca_net_times_.end() && t->second != 0) ? (millis() - t->second) / 1000 : 0;
  return it->second;
}

std::string TigoWebServer::ble_uri_encode_(const std::string &value) {
  // Match the app's CK(): encodeURIComponent then escape !'()*. Net effect is RFC 3986
  // unreserved kept (A-Za-z0-9-_.~), everything else percent-encoded (uppercase hex).
  // Required because params are space-separated on the wire, so a space or special char
  // in an SSID/password would otherwise split or corrupt the frame.
  static const char *const HEX = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() * 3);
  for (unsigned char c : value) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(HEX[c >> 4]);
      out.push_back(HEX[c & 0x0F]);
    }
  }
  return out;
}

bool TigoWebServer::ble_has_cca_info_() {
  std::lock_guard<std::mutex> lock(this->cca_info_mutex_);
  return this->cca_info_time_ != 0 && !this->cca_info_json_.empty();
}

std::string TigoWebServer::ble_get_cca_info_json_() {
  std::lock_guard<std::mutex> lock(this->cca_info_mutex_);
  return this->cca_info_json_;
}

uint32_t TigoWebServer::ble_get_cca_info_seconds_ago_() {
  std::lock_guard<std::mutex> lock(this->cca_info_mutex_);
  if (this->cca_info_time_ == 0)
    return 0;
  return (millis() - this->cca_info_time_) / 1000;
}

std::string TigoWebServer::ble_address_() { return this->parent()->address_str(); }

// ---------------------------------------------------------------------------
// BLE device search + on-device MAC persistence
// ---------------------------------------------------------------------------

// Tigo Energy's BLE MAC OUI — only the CCA advertises on it, so it's a reliable filter.
static constexpr uint64_t TIGO_OUI = 0x04C05Bull;
static const uint32_t CCA_MAC_MAGIC = 0x71C0AC11;  // "tigo cca mac"

struct CcaBleMacPref {
  uint32_t magic;
  uint64_t mac;  // 0 = no override (use the YAML MAC)
};

static uint32_t cca_ble_mac_hash() { return fnv1_hash("tigo_cca_ble_mac_v1"); }

// "AA:BB:CC:DD:EE:FF" -> uint64 (0 on malformed input).
static uint64_t parse_mac_(const std::string &s) {
  unsigned b[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
    return 0;
  uint64_t mac = 0;
  for (int i = 0; i < 6; i++) {
    if (b[i] > 0xFF) return 0;
    mac = (mac << 8) | b[i];
  }
  return mac;
}

static std::string mac_to_str_(uint64_t mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           (unsigned) ((mac >> 40) & 0xFF), (unsigned) ((mac >> 32) & 0xFF),
           (unsigned) ((mac >> 24) & 0xFF), (unsigned) ((mac >> 16) & 0xFF),
           (unsigned) ((mac >> 8) & 0xFF), (unsigned) (mac & 0xFF));
  return std::string(buf);
}

// Tracker callback (main loop). Keep it cheap and never log — logging every advertisement
// starves the loop and drops the web/API (confirmed during BLE bring-up).
bool TigoWebServer::parse_device(const esp32_ble_tracker::ESPBTDevice &device) {
  uint64_t mac = device.address_uint64();
  if ((mac >> 24) != TIGO_OUI)  // top 3 bytes == 04:C0:5B
    return false;
  std::lock_guard<std::mutex> lock(this->ble_scan_mutex_);
  for (auto &h : this->ble_scan_hits_) {
    if (h.mac == mac) {  // refresh existing hit
      h.rssi = device.get_rssi();
      h.seen_ms = millis();
      if (h.name.empty() && !device.get_name().empty()) h.name = device.get_name();
      return false;
    }
  }
  if (this->ble_scan_hits_.size() < 16) {
    this->ble_scan_hits_.push_back({mac, device.get_rssi(), device.get_name(), millis()});
    // Safe to log: only the CCA advertises on this OUI, so this fires a handful of times.
    ESP_LOGI(BLE_TAG, "Found Tigo CCA over BLE: %s RSSI=%d %s", device.address_str(),
             device.get_rssi(), device.get_name().c_str());
  }
  return false;  // not consumed — let other listeners see it too
}

void TigoWebServer::ble_scan_clear() {
  std::lock_guard<std::mutex> lock(this->ble_scan_mutex_);
  this->ble_scan_hits_.clear();
}

std::string TigoWebServer::ble_scan_json() {
  uint64_t active = this->parent()->get_address();
  std::string out = "{\"active_mac\":\"" + mac_to_str_(active) + "\",\"yaml_mac\":\"" +
                    mac_to_str_(this->ble_yaml_mac_) + "\",\"saved\":" +
                    (this->ble_saved_mac_ != 0 ? "true" : "false") + ",\"devices\":[";
  std::lock_guard<std::mutex> lock(this->ble_scan_mutex_);
  bool first = true;
  for (auto &h : this->ble_scan_hits_) {
    std::string name = h.name;
    for (auto &c : name)
      if (c == '"' || c == '\\') c = '\'';  // keep the JSON well-formed
    out += first ? "" : ",";
    first = false;
    out += "{\"mac\":\"" + mac_to_str_(h.mac) + "\",\"rssi\":" + std::to_string(h.rssi) +
           ",\"name\":\"" + name + "\",\"age_s\":" + std::to_string((millis() - h.seen_ms) / 1000) + "}";
  }
  out += "]}";
  return out;
}

void TigoWebServer::ble_load_mac_() {
  this->ble_yaml_mac_ = this->parent()->get_address();  // compile-time MAC = revert target
  CcaBleMacPref pref{};
  auto p = global_preferences->make_preference<CcaBleMacPref>(cca_ble_mac_hash());
  if (p.load(&pref) && pref.magic == CCA_MAC_MAGIC && pref.mac != 0) {
    this->ble_saved_mac_ = pref.mac;
    this->parent()->set_address(pref.mac);
    ESP_LOGI(BLE_TAG, "Using saved CCA BLE MAC %s (YAML default %s)",
             mac_to_str_(pref.mac).c_str(), mac_to_str_(this->ble_yaml_mac_).c_str());
  }
}

bool TigoWebServer::ble_set_saved_mac(const std::string &mac_str) {
  uint64_t mac = parse_mac_(mac_str);
  if (mac == 0)
    return false;
  if (this->ble_connected_)  // don't retarget a live link
    this->cca_ble_disconnect();
  this->ble_saved_mac_ = mac;
  this->parent()->set_address(mac);
  CcaBleMacPref pref{CCA_MAC_MAGIC, mac};
  auto p = global_preferences->make_preference<CcaBleMacPref>(cca_ble_mac_hash());
  p.save(&pref);
  ESP_LOGI(BLE_TAG, "Saved CCA BLE MAC %s", mac_to_str_(mac).c_str());
  return true;
}

void TigoWebServer::ble_reset_mac() {
  if (this->ble_connected_)
    this->cca_ble_disconnect();
  this->ble_saved_mac_ = 0;
  this->parent()->set_address(this->ble_yaml_mac_);
  CcaBleMacPref pref{CCA_MAC_MAGIC, 0};
  auto p = global_preferences->make_preference<CcaBleMacPref>(cca_ble_mac_hash());
  p.save(&pref);
  ESP_LOGI(BLE_TAG, "Reverted CCA BLE MAC to YAML default %s", mac_to_str_(this->ble_yaml_mac_).c_str());
}

}  // namespace tigo_server
}  // namespace esphome

#endif  // USE_TIGO_CCA_BLE
