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

#include <arpa/inet.h>

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

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

// Behind a reverse proxy, every visitor arrives from the proxy's address, so
// one person's wrong guesses would lock everyone out. The proxy says who it
// is passing along in X-Forwarded-For; that is believed only from an address
// in trusted_proxies (each an address, or a network as address/bits), since
// anyone else can write the header. The proxy appends the address it saw,
// so the list is read from the end, past any further trusted proxies; what
// comes before that is the visitor's own to write, and isn't used.
namespace web_proxy_detail {
struct Net {
  int family = 0;  // AF_INET or AF_INET6
  unsigned char bytes[16] = {};
  int bits = 0;
};
// An address, written IPv4-as-IPv6 (::ffff:a.b.c.d) or not, as IPv4 or IPv6.
inline bool parse_address(const std::string &text, Net &out) {
  std::string t = text;
  if (t.rfind("::ffff:", 0) == 0 && t.find('.') != std::string::npos) t = t.substr(7);
  if (inet_pton(AF_INET, t.c_str(), out.bytes) == 1) {
    out.family = AF_INET;
    out.bits = 32;
    return true;
  }
  if (inet_pton(AF_INET6, t.c_str(), out.bytes) == 1) {
    out.family = AF_INET6;
    out.bits = 128;
    return true;
  }
  return false;
}
inline bool parse_entry(const std::string &entry, Net &out) {
  size_t slash = entry.find('/');
  if (!parse_address(entry.substr(0, slash), out)) return false;
  if (slash == std::string::npos) return true;
  std::string b = entry.substr(slash + 1);
  if (b.empty() || b.size() > 3 || b.find_first_not_of("0123456789") != std::string::npos) return false;
  int bits = std::stoi(b);
  if (bits > out.bits) return false;
  out.bits = bits;
  return true;
}
inline bool in(const Net &addr, const Net &net) {
  if (addr.family != net.family) return false;
  int full = net.bits / 8, rest = net.bits % 8;
  if (std::memcmp(addr.bytes, net.bytes, full) != 0) return false;
  if (!rest) return true;
  unsigned char mask = static_cast<unsigned char>(0xff << (8 - rest));
  return (addr.bytes[full] & mask) == (net.bytes[full] & mask);
}
inline std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
}  // namespace web_proxy_detail

inline bool proxy_entry_ok(const std::string &entry) {
  web_proxy_detail::Net n;
  return web_proxy_detail::parse_entry(entry, n);
}

inline bool from_trusted_proxy(const std::string &address, const std::vector<std::string> &trusted) {
  web_proxy_detail::Net a, n;
  if (!web_proxy_detail::parse_address(address, a)) return false;
  for (const std::string &e : trusted)
    if (web_proxy_detail::parse_entry(e, n) && web_proxy_detail::in(a, n)) return true;
  return false;
}

// Who to count wrong passwords against: the connection's address, or, from
// a trusted proxy, the address it forwarded for.
inline std::string client_address(const std::string &remote, const std::string &forwarded_for,
                                  const std::vector<std::string> &trusted) {
  std::string who = remote;
  if (!from_trusted_proxy(who, trusted)) return who;
  size_t end = forwarded_for.size();
  while (end > 0) {
    size_t comma = forwarded_for.rfind(',', end - 1);
    size_t start = comma == std::string::npos ? 0 : comma + 1;
    std::string hop = web_proxy_detail::trim(forwarded_for.substr(start, end - start));
    web_proxy_detail::Net a;
    if (!web_proxy_detail::parse_address(hop, a)) return who;  // garbled: stop at the last one we trust
    who = hop;
    if (!from_trusted_proxy(hop, trusted)) return who;
    if (comma == std::string::npos) break;
    end = comma;
  }
  return remote;  // every hop is a trusted proxy: count against the connection
}
