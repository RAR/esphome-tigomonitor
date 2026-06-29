// Tigo cloud import — recovers the system layout (panel names + string/MPPT/inverter
// structure + optimizer serials) from Tigo's cloud API when the CCA's local HTTP API is
// locked down (firmware 4.0.4+). BLE gives us CCA info/network but not the panel layout;
// this fills that gap and applies it to the node table exactly like the old local-HTTP
// "Import from CCA" did (match cloud serial -> UART barcode, then set node metadata).
//
// API (same one the Tigo mobile app uses, verified against Bobsilvio/tigosolar-online):
//   login   : POST https://mapi.tigoenergy.com/api/v3/user/login?type=8
//             body {username,password} -> user.auth (bearer), user.refresh_token, user.expires
//   systems : GET  /api/v3/systems/query?limit=100&page=1&sort=-id -> systems[].system_id
//   layout  : GET  /api/v3/systems/layout?id=<id>
//             -> system.inverters[].mppts[].strings[].panels[]{serial,label,object_id,type}
//
// Credentials are entered in the web UI; only the bearer token (months-lived, revocable)
// is persisted to NVS — never the password.

#include "tigo_monitor.h"

#ifdef USE_TIGO_CLOUD

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/components/network/util.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <cctype>
#include <cstring>
#include <string>

namespace esphome {
namespace tigo_monitor {

static const char *const CLOUD_TAG = "tigo_monitor.cloud";
static const char *const CLOUD_API_HOST = "https://mapi.tigoenergy.com";

// Persisted credential blob (NVS, plaintext at rest). Only the token is stored — never the
// password. Sizes are generous; Tigo's bearer token can be long.
struct CloudCreds {
  char email[128];
  char token[1536];
  char refresh[512];
  char expires[40];
  int32_t system_id;
};

static uint32_t cloud_creds_hash() { return fnv1_hash("tigo_cloud_creds_v1"); }

// ---------------------------------------------------------------------------
// HTTPS JSON request (TLS verified against the ESP-IDF cert bundle)
// ---------------------------------------------------------------------------
bool TigoMonitorComponent::cloud_http_json_(const char *method, const std::string &url,
                                            const std::string &body, const std::string &bearer,
                                            std::string &out_body, int &out_status) {
  out_body.clear();
  out_status = 0;
  if (!network::is_connected()) {
    ESP_LOGW(CLOUD_TAG, "Network not connected");
    return false;
  }

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = (strcmp(method, "POST") == 0) ? HTTP_METHOD_POST : HTTP_METHOD_GET;
  config.timeout_ms = 15000;
  config.buffer_size = 2048;
  config.buffer_size_tx = 1024;
  config.crt_bundle_attach = esp_crt_bundle_attach;  // verify TLS against the cert bundle
  config.keep_alive_enable = false;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(CLOUD_TAG, "HTTP client init failed");
    return false;
  }
  esp_http_client_set_header(client, "Accept", "application/json");
  std::string auth;
  if (!bearer.empty()) {
    auth = "Bearer " + bearer;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
  }
  if (!body.empty())
    esp_http_client_set_header(client, "Content-Type", "application/json");

  esp_err_t err = esp_http_client_open(client, body.size());
  if (err != ESP_OK) {
    ESP_LOGE(CLOUD_TAG, "open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return false;
  }
  if (!body.empty()) {
    int written = esp_http_client_write(client, body.c_str(), body.size());
    if (written < 0) {
      ESP_LOGE(CLOUD_TAG, "request body write failed");
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return false;
    }
  }

  esp_http_client_fetch_headers(client);
  out_status = esp_http_client_get_status_code(client);

  const size_t buf_size = 2048;
  char *buf = static_cast<char *>(heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM));
  if (!buf)
    buf = static_cast<char *>(malloc(buf_size));
  if (!buf) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  constexpr size_t MAX_RESP = 96 * 1024;
  int read_len;
  bool truncated = false;
  while ((read_len = esp_http_client_read(client, buf, buf_size)) > 0) {
    if (out_body.size() + read_len > MAX_RESP) {
      truncated = true;
      break;
    }
    out_body.append(buf, read_len);
  }
  heap_caps_free(buf);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (truncated) {
    ESP_LOGE(CLOUD_TAG, "response exceeded %zu byte cap", MAX_RESP);
    return false;
  }
  return read_len >= 0;
}

// ---------------------------------------------------------------------------
// Login + system discovery
// ---------------------------------------------------------------------------
bool TigoMonitorComponent::tigo_cloud_login(const std::string &email, const std::string &password) {
  // Build the body with cJSON so the email/password are escaped correctly.
  cJSON *req = cJSON_CreateObject();
  cJSON_AddStringToObject(req, "username", email.c_str());
  cJSON_AddStringToObject(req, "password", password.c_str());
  char *body_c = cJSON_PrintUnformatted(req);
  std::string body = body_c ? body_c : "";
  cJSON_free(body_c);
  cJSON_Delete(req);

  std::string url = std::string(CLOUD_API_HOST) + "/api/v3/user/login?type=8";
  std::string resp;
  int status = 0;
  if (!cloud_http_json_("POST", url, body, "", resp, status)) {
    ESP_LOGW(CLOUD_TAG, "login request failed");
    return false;
  }
  if (status == 401 || status == 403) {
    ESP_LOGW(CLOUD_TAG, "login rejected (%d) — bad credentials", status);
    return false;
  }
  if (status != 200) {
    ESP_LOGW(CLOUD_TAG, "login HTTP %d", status);
    return false;
  }

  cJSON *root = cJSON_Parse(resp.c_str());
  if (!root) {
    ESP_LOGW(CLOUD_TAG, "login: unparseable response");
    return false;
  }
  cJSON *user = cJSON_GetObjectItem(root, "user");
  cJSON *tok = user ? cJSON_GetObjectItem(user, "auth") : nullptr;
  if (!tok || !cJSON_IsString(tok)) {
    ESP_LOGW(CLOUD_TAG, "login: no token in response");
    cJSON_Delete(root);
    return false;
  }
  cloud_email_ = email;
  cloud_token_ = tok->valuestring;
  if (cloud_token_.size() >= sizeof(((CloudCreds *) nullptr)->token))
    ESP_LOGW(CLOUD_TAG, "token longer than storage buffer — will truncate on persist");
  cJSON *rt = cJSON_GetObjectItem(user, "refresh_token");
  cloud_refresh_token_ = (rt && cJSON_IsString(rt)) ? rt->valuestring : "";
  cJSON *exp = cJSON_GetObjectItem(user, "expires");
  cloud_expires_iso_ = (exp && cJSON_IsString(exp)) ? exp->valuestring : "";
  cJSON_Delete(root);
  ESP_LOGI(CLOUD_TAG, "Logged into Tigo cloud as %s (token expires %s)", email.c_str(),
           cloud_expires_iso_.c_str());

  // Discover the system id (newest system on the account).
  cloud_system_id_ = 0;
  std::string sys_url =
      std::string(CLOUD_API_HOST) + "/api/v3/systems/query?limit=100&page=1&sort=-id";
  std::string sresp;
  int sstatus = 0;
  if (cloud_http_json_("GET", sys_url, "", cloud_token_, sresp, sstatus) && sstatus == 200) {
    cJSON *sroot = cJSON_Parse(sresp.c_str());
    if (sroot) {
      cJSON *systems = cJSON_GetObjectItem(sroot, "systems");
      if (!systems || !cJSON_IsArray(systems))
        systems = cJSON_GetObjectItem(sroot, "data");
      if (systems && cJSON_IsArray(systems) && cJSON_GetArraySize(systems) > 0) {
        cJSON *s0 = cJSON_GetArrayItem(systems, 0);
        cJSON *sid = cJSON_GetObjectItem(s0, "system_id");
        if (!sid || !cJSON_IsNumber(sid))
          sid = cJSON_GetObjectItem(s0, "id");
        if (sid && cJSON_IsNumber(sid))
          cloud_system_id_ = sid->valueint;
      }
      cJSON_Delete(sroot);
    }
  }
  ESP_LOGI(CLOUD_TAG, "Tigo cloud system_id = %d", cloud_system_id_);

  cloud_save_creds_();
  return true;
}

// ---------------------------------------------------------------------------
// Import the layout and apply it to the node table
// ---------------------------------------------------------------------------
bool TigoMonitorComponent::tigo_cloud_import() {
  if (cloud_token_.empty()) {
    ESP_LOGW(CLOUD_TAG, "import: no token — connect first");
    return false;
  }
  if (cloud_system_id_ == 0) {
    ESP_LOGW(CLOUD_TAG, "import: no system id — reconnect");
    return false;
  }

  std::string url =
      std::string(CLOUD_API_HOST) + "/api/v3/systems/layout?id=" + std::to_string(cloud_system_id_);
  std::string resp;
  int status = 0;
  if (!cloud_http_json_("GET", url, "", cloud_token_, resp, status)) {
    ESP_LOGW(CLOUD_TAG, "layout request failed");
    return false;
  }
  if (status == 401) {
    ESP_LOGW(CLOUD_TAG, "layout 401 — token expired; clearing, reconnect required");
    cloud_token_.clear();
    cloud_save_creds_();
    return false;
  }
  if (status != 200) {
    ESP_LOGW(CLOUD_TAG, "layout HTTP %d", status);
    return false;
  }
  match_cloud_layout_to_uart_(resp);
  return true;
}

// ---------------------------------------------------------------------------
// Cloud health rollup (Energy-Intelligence equipment-status/summary)
// ---------------------------------------------------------------------------
bool TigoMonitorComponent::tigo_cloud_health(std::string &out_json) {
  // The summary endpoint returns Tigo's own warning/error counts per equipment type, e.g.
  // [{"equipmentType":"unit","warning":1,"error":0,"unknown":0},{"equipmentType":"panel",…}].
  // It's already clean JSON, so we pass it straight through to the web UI.
  out_json.clear();
  if (cloud_token_.empty() || cloud_system_id_ == 0) {
    ESP_LOGW(CLOUD_TAG, "health: not connected");
    return false;
  }
  std::string url = "https://ei.tigoenergy.com/api/v4/equipment-status/summary?systemId=" +
                    std::to_string(cloud_system_id_);
  std::string resp;
  int status = 0;
  if (!cloud_http_json_("GET", url, "", cloud_token_, resp, status)) {
    ESP_LOGW(CLOUD_TAG, "health request failed");
    return false;
  }
  if (status == 401) {
    ESP_LOGW(CLOUD_TAG, "health 401 — token expired; clearing");
    cloud_token_.clear();
    cloud_save_creds_();
    return false;
  }
  if (status != 200 || resp.empty() || resp[0] != '[') {
    ESP_LOGW(CLOUD_TAG, "health HTTP %d", status);
    return false;
  }
  out_json = resp;
  return true;
}

bool TigoMonitorComponent::tigo_cloud_equipment(const std::string &view, std::string &out_json) {
  // Proxy the Energy-Intelligence per-equipment status feeds straight through to the UI.
  //   view "latest"  -> equipment-status/latest  (current status per equipment)
  //   view "history" -> equipment-status/history (recent chronological status events)
  // statusCode classification (confirmed): 0=ok, 1=warning, 2=error.
  out_json.clear();
  if (cloud_token_.empty() || cloud_system_id_ == 0) {
    ESP_LOGW(CLOUD_TAG, "equipment: not connected");
    return false;
  }
  std::string path = (view == "history")
                         ? "/api/v4/equipment-status/history?limit=50&offset=0&systemId="
                         : "/api/v4/equipment-status/latest?systemId=";
  std::string url = "https://ei.tigoenergy.com" + path + std::to_string(cloud_system_id_);
  std::string resp;
  int status = 0;
  if (!cloud_http_json_("GET", url, "", cloud_token_, resp, status)) {
    ESP_LOGW(CLOUD_TAG, "equipment request failed");
    return false;
  }
  if (status == 401) {
    cloud_token_.clear();
    cloud_save_creds_();
    return false;
  }
  if (status != 200 || resp.empty() || resp[0] != '[') {
    ESP_LOGW(CLOUD_TAG, "equipment HTTP %d", status);
    return false;
  }
  out_json = resp;
  return true;
}

// ---------------------------------------------------------------------------
// Layout -> node table (mirror of match_cca_to_uart)
// ---------------------------------------------------------------------------
void TigoMonitorComponent::match_cloud_layout_to_uart_(const std::string &layout_json) {
  StateLock lock(state_mutex_);

  // PSRAM-backed cJSON parse — the layout can be sizeable on large systems.
  cJSON_Hooks hooks;
  hooks.malloc_fn = [](size_t s) -> void * {
    void *p = heap_caps_malloc(s, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(s);
  };
  hooks.free_fn = [](void *p) { heap_caps_free(p); };
  cJSON_InitHooks(&hooks);

  cJSON *root = cJSON_Parse(layout_json.c_str());
  if (!root) {
    ESP_LOGE(CLOUD_TAG, "layout parse failed");
    cJSON_InitHooks(NULL);
    return;
  }
  cJSON *system = cJSON_GetObjectItem(root, "system");
  cJSON *inverters = system ? cJSON_GetObjectItem(system, "inverters") : nullptr;
  if (!inverters || !cJSON_IsArray(inverters)) {
    ESP_LOGW(CLOUD_TAG, "layout: no system.inverters array");
    cJSON_Delete(root);
    cJSON_InitHooks(NULL);
    return;
  }

  // Layout skeleton at DEBUG — handy if a future layout doesn't import cleanly.
  ESP_LOGD(CLOUD_TAG, "layout skeleton: %d inverter(s)", cJSON_GetArraySize(inverters));
  {
    cJSON *di;
    cJSON_ArrayForEach(di, inverters) {
      cJSON *dil = cJSON_GetObjectItem(di, "label");
      cJSON *dm = cJSON_GetObjectItem(di, "mppts");
      ESP_LOGD(CLOUD_TAG, "  inv '%s': %d mppt(s)",
               (dil && cJSON_IsString(dil)) ? dil->valuestring : "?",
               cJSON_IsArray(dm) ? cJSON_GetArraySize(dm) : -1);
      cJSON *dmm;
      cJSON_ArrayForEach(dmm, dm) {
        cJSON *dml = cJSON_GetObjectItem(dmm, "label");
        cJSON *ds = cJSON_GetObjectItem(dmm, "strings");
        ESP_LOGD(CLOUD_TAG, "    mppt '%s': %d string(s)",
                 (dml && cJSON_IsString(dml)) ? dml->valuestring : "?",
                 cJSON_IsArray(ds) ? cJSON_GetArraySize(ds) : -1);
        cJSON *dss;
        cJSON_ArrayForEach(dss, ds) {
          cJSON *dsl = cJSON_GetObjectItem(dss, "label");
          cJSON *dp = cJSON_GetObjectItem(dss, "panels");
          ESP_LOGD(CLOUD_TAG, "      string '%s': %d panel(s)",
                   (dsl && cJSON_IsString(dsl)) ? dsl->valuestring : "?",
                   cJSON_IsArray(dp) ? cJSON_GetArraySize(dp) : -1);
        }
      }
    }
  }

  int panel_count = 0, matched = 0, global_mppt = 0;
  cJSON *inv;
  cJSON_ArrayForEach(inv, inverters) {
    cJSON *mppts = cJSON_GetObjectItem(inv, "mppts");
    if (!cJSON_IsArray(mppts)) continue;
    cJSON *mppt;
    cJSON_ArrayForEach(mppt, mppts) {
      // The cloud numbers MPPTs per-inverter (each inverter has its own "MPPT 1"/"MPPT 2"),
      // so the labels collide across inverters and every string collapses onto the first
      // YAML inverter. Renumber globally in layout order (MPPT 1,2,3,4,…) so they line up
      // with the one-global-MPPT-per-physical-MPPT convention the YAML `inverters: mppts:`
      // list uses (inverter 1 -> MPPT 1,2; inverter 2 -> MPPT 3,4; …).
      global_mppt++;
      std::string mppt_label = "MPPT " + std::to_string(global_mppt);
      cJSON *strings = cJSON_GetObjectItem(mppt, "strings");
      if (!cJSON_IsArray(strings)) continue;
      cJSON *str;
      cJSON_ArrayForEach(str, strings) {
        cJSON *sl = cJSON_GetObjectItem(str, "label");
        std::string string_label = (sl && cJSON_IsString(sl)) ? sl->valuestring : "";
        cJSON *panels = cJSON_GetObjectItem(str, "panels");
        if (!cJSON_IsArray(panels)) continue;
        cJSON *panel;
        cJSON_ArrayForEach(panel, panels) {
          panel_count++;
          cJSON *ser = cJSON_GetObjectItem(panel, "serial");
          if (!ser || !cJSON_IsString(ser)) continue;
          cJSON *pl = cJSON_GetObjectItem(panel, "label");
          std::string panel_label = (pl && cJSON_IsString(pl)) ? pl->valuestring : "";
          cJSON *oid = cJSON_GetObjectItem(panel, "object_id");
          std::string obj_id;
          if (oid && cJSON_IsString(oid))
            obj_id = oid->valuestring;
          else if (oid && cJSON_IsNumber(oid))
            obj_id = std::to_string(oid->valueint);

          // The cloud serial embeds the radio MAC's last 6 hex: MAC 04C05B4000BBCC02 ->
          // serial "4-BBCC02K" (6-hex core + a check char). The UART side only has the MAC
          // (long_address), so match a node when its MAC's last 6 chars appear anywhere in
          // the serial (case-insensitive) — the printed serial and the MAC are otherwise
          // disjoint identifier spaces.
          std::string serial_up = ser->valuestring;
          for (char &c : serial_up) c = (char) toupper((unsigned char) c);
          bool found = false;
          for (auto &node : node_table_) {
            std::string bc = get_barcode_for_node(node);
            if (bc.size() < 6) continue;
            std::string last6 = bc.substr(bc.size() - 6);
            for (char &c : last6) c = (char) toupper((unsigned char) c);
            if (serial_up.find(last6) == std::string::npos) continue;
            node.cca_label = panel_label;
            node.cca_string_label = string_label;
            node.cca_inverter_label = mppt_label;
            node.cca_object_id = obj_id;
            node.cca_validated = true;
            matched++;
            found = true;
            ESP_LOGD(CLOUD_TAG, "Matched %s (...%s) -> '%s' [%s / %s]", node.addr.c_str(),
                     last6.c_str(), panel_label.c_str(), mppt_label.c_str(), string_label.c_str());
            break;
          }
          if (!found)
            ESP_LOGD(CLOUD_TAG, "No UART node for cloud panel '%s' (serial %s)",
                     panel_label.c_str(), serial_up.c_str());
        }
      }
    }
  }
  cJSON_Delete(root);
  cJSON_InitHooks(NULL);

  ESP_LOGI(CLOUD_TAG, "Cloud layout import: %d panels, %d matched to UART devices", panel_count,
           matched);
  if (matched > 0) {
    save_node_table();
    last_cca_sync_time_ = millis();
    rebuild_string_groups();
  }
}

// ---------------------------------------------------------------------------
// Credential persistence (token only — never the password)
// ---------------------------------------------------------------------------
void TigoMonitorComponent::cloud_save_creds_() {
  CloudCreds c{};
  strncpy(c.email, cloud_email_.c_str(), sizeof(c.email) - 1);
  strncpy(c.token, cloud_token_.c_str(), sizeof(c.token) - 1);
  strncpy(c.refresh, cloud_refresh_token_.c_str(), sizeof(c.refresh) - 1);
  strncpy(c.expires, cloud_expires_iso_.c_str(), sizeof(c.expires) - 1);
  c.system_id = cloud_system_id_;
  auto pref = this->cached_pref_<CloudCreds>(cloud_creds_hash());
  pref.save(&c);
  ESP_LOGD(CLOUD_TAG, "Cloud creds persisted (token %d bytes)", (int) cloud_token_.size());
}

void TigoMonitorComponent::tigo_cloud_load_creds() {
  CloudCreds c{};
  auto pref = this->cached_pref_<CloudCreds>(cloud_creds_hash());
  if (!pref.load(&c))
    return;
  c.email[sizeof(c.email) - 1] = '\0';
  c.token[sizeof(c.token) - 1] = '\0';
  c.refresh[sizeof(c.refresh) - 1] = '\0';
  c.expires[sizeof(c.expires) - 1] = '\0';
  cloud_email_ = c.email;
  cloud_token_ = c.token;
  cloud_refresh_token_ = c.refresh;
  cloud_expires_iso_ = c.expires;
  cloud_system_id_ = c.system_id;
  if (!cloud_token_.empty())
    ESP_LOGI(CLOUD_TAG, "Restored Tigo cloud token for %s (system %d, expires %s)",
             cloud_email_.c_str(), cloud_system_id_, cloud_expires_iso_.c_str());
}

}  // namespace tigo_monitor
}  // namespace esphome

#endif  // USE_TIGO_CLOUD
