// The web UI's sign-in: a session per browser, after the username and
// password, in place of the browser's own login popup (HTTP Basic).
//
// A session is 32 random bytes, sent to the browser as an HttpOnly,
// SameSite=Strict cookie and held here in memory only: a restart signs
// everyone out, and no token is ever written to disk. A new password or
// username ends every session. Wrong passwords are counted per client
// address, the sign-in form's and HTTP Basic's alike: TRIES within WINDOW_S
// lock that address out for LOCK_S. The same wrong one again counts once,
// so a browser that saved a login before the password changed, and sends
// it with every request, doesn't lock its owner out by itself.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>

class WebSessions {
 public:
  static const int64_t LIFETIME_S = 30LL * 24 * 3600;
  static const size_t MAX = 32;  // the oldest goes when a new one would pass this
  static const int TRIES = 5;
  static const int64_t WINDOW_S = 15 * 60;
  static const int64_t LOCK_S = 15 * 60;

  std::string create(int64_t now, const std::function<uint8_t()> &random_byte) {
    std::string t;
    char hex[3];
    for (int i = 0; i < 32; i++) {
      std::snprintf(hex, sizeof(hex), "%02x", random_byte());
      t += hex;
    }
    std::lock_guard<std::mutex> lock(mu_);
    while (sessions_.size() >= MAX) {
      auto oldest = sessions_.begin();
      for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
        if (it->second < oldest->second) oldest = it;
      sessions_.erase(oldest);
    }
    sessions_[t] = now + LIFETIME_S;
    return t;
  }
  bool valid(const std::string &token, int64_t now) {
    if (token.size() != 64) return false;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) return false;
    if (now >= it->second) {
      sessions_.erase(it);
      return false;
    }
    return true;
  }
  void drop(const std::string &token) {
    std::lock_guard<std::mutex> lock(mu_);
    sessions_.erase(token);
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mu_);
    sessions_.clear();
  }
  size_t size() {
    std::lock_guard<std::mutex> lock(mu_);
    return sessions_.size();
  }

  bool locked(const std::string &client, int64_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = tries_.find(client);
    return it != tries_.end() && now < it->second.locked_until;
  }
  void failed(const std::string &client, int64_t now, const std::string &attempt) {
    size_t h = std::hash<std::string>()(attempt);  // not the attempt itself: it may be the old password
    std::lock_guard<std::mutex> lock(mu_);
    if (tries_.size() > 256) tries_.clear();  // a flood of addresses: start over rather than grow
    Tries &t = tries_[client];
    if (t.count && h == t.last && now - t.first < WINDOW_S) return;
    t.last = h;
    if (now - t.first >= WINDOW_S) t = {0, now, t.locked_until, h};
    if (++t.count >= TRIES) {
      t.locked_until = now + LOCK_S;
      t.count = 0;
      t.first = now;
    }
  }
  void succeeded(const std::string &client) {
    std::lock_guard<std::mutex> lock(mu_);
    tries_.erase(client);
  }

 private:
  struct Tries {
    int count = 0;
    int64_t first = 0;
    int64_t locked_until = 0;
    size_t last = 0;  // the last wrong attempt's hash
  };
  std::mutex mu_;
  std::map<std::string, int64_t> sessions_;  // token -> expiry, unix seconds
  std::map<std::string, Tries> tries_;
};

// One cookie's value from a Cookie header, or "".
inline std::string cookie_value(const std::string &header, const std::string &name) {
  size_t i = 0;
  while (i < header.size()) {
    while (i < header.size() && (header[i] == ' ' || header[i] == ';')) i++;
    size_t end = header.find(';', i);
    if (end == std::string::npos) end = header.size();
    std::string part = header.substr(i, end - i);
    size_t eq = part.find('=');
    if (eq != std::string::npos && part.substr(0, eq) == name) return part.substr(eq + 1);
    i = end + 1;
  }
  return "";
}
