// Host test for linux/src/web_session.h: the web UI's sign-in sessions and
// the lockout after repeated wrong passwords.
#include "web_session.h"

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

// Not random, but no two tokens alike (a byte counter repeats every 8).
static uint8_t counter_byte() {
  static uint32_t n = 12345;
  n = n * 1103515245u + 12345u;
  return static_cast<uint8_t>(n >> 16);
}

static void test_a_session() {
  WebSessions s;
  std::string t = s.create(1000, counter_byte);
  CHECK(t.size() == 64);
  CHECK(t.find_first_not_of("0123456789abcdef") == std::string::npos);
  CHECK(s.valid(t, 1000));
  CHECK(s.valid(t, 1000 + WebSessions::LIFETIME_S - 1));
  CHECK(!s.valid(t, 1000 + WebSessions::LIFETIME_S));  // expired
  CHECK(!s.valid("", 1000));
  CHECK(!s.valid(t.substr(0, 63) + "x", 1000));
  std::string u = s.create(1000, counter_byte);
  CHECK(u != t);
  s.drop(u);
  CHECK(!s.valid(u, 1000));
  std::string v = s.create(2000, counter_byte);
  s.clear();  // a new password: every session ends
  CHECK(!s.valid(v, 2000));
}

static void test_the_oldest_goes_first() {
  WebSessions s;
  std::string first = s.create(1, counter_byte);
  for (size_t i = 1; i < WebSessions::MAX; i++) s.create(1 + static_cast<int64_t>(i), counter_byte);
  CHECK(s.valid(first, 100));
  s.create(100, counter_byte);
  CHECK(!s.valid(first, 100));
  CHECK(s.size() == WebSessions::MAX);
}

static void test_the_lockout() {
  WebSessions s;
  for (int i = 0; i < WebSessions::TRIES - 1; i++) s.failed("10.0.0.5", 100 + i, std::to_string(i));
  CHECK(!s.locked("10.0.0.5", 110));
  s.failed("10.0.0.5", 110, "last");
  CHECK(s.locked("10.0.0.5", 111));
  CHECK(s.locked("10.0.0.5", 110 + WebSessions::LOCK_S - 1));
  CHECK(!s.locked("10.0.0.5", 110 + WebSessions::LOCK_S));
  CHECK(!s.locked("10.0.0.6", 111));  // per address
  // Tries spread over more than the window don't add up.
  WebSessions w;
  for (int i = 0; i < WebSessions::TRIES; i++) w.failed("a", i * (WebSessions::WINDOW_S / 2), std::to_string(i));
  CHECK(!w.locked("a", WebSessions::TRIES * (WebSessions::WINDOW_S / 2)));
  // The same wrong attempt again counts once: a browser repeating a login
  // it saved before the password changed, once per request.
  WebSessions same;
  for (int i = 0; i < 20; i++) same.failed("c", 10, "admin:old");
  CHECK(!same.locked("c", 11));
  for (int i = 0; i < WebSessions::TRIES - 1; i++) same.failed("c", 12, "admin:guess" + std::to_string(i));
  CHECK(same.locked("c", 13));  // the stale one and four guesses
  // A right password clears the count.
  WebSessions r;
  for (int i = 0; i < WebSessions::TRIES - 1; i++) r.failed("b", 10, std::to_string(i));
  r.succeeded("b");
  r.failed("b", 11, "x");
  CHECK(!r.locked("b", 12));
}

static void test_the_cookie() {
  CHECK(cookie_value("tm_session=abc", "tm_session") == "abc");
  CHECK(cookie_value("theme=dark; tm_session=abc; x=1", "tm_session") == "abc");
  CHECK(cookie_value("xtm_session=abc", "tm_session").empty());
  CHECK(cookie_value("", "tm_session").empty());
  CHECK(cookie_value("tm_session=", "tm_session").empty());
}

static void test_behind_a_proxy() {
  std::vector<std::string> none;
  std::vector<std::string> proxy = {"10.0.0.1"};
  // No trusted proxy: the header is anyone's to write, so it is ignored.
  CHECK(client_address("10.0.0.1", "203.0.113.7", none) == "10.0.0.1");
  // From the proxy: the address it saw, the last in the list.
  CHECK(client_address("10.0.0.1", "203.0.113.7", proxy) == "203.0.113.7");
  CHECK(client_address("10.0.0.1", "198.51.100.9, 203.0.113.7", proxy) == "203.0.113.7");  // the first is the visitor's to write
  CHECK(client_address("10.0.0.1", " 203.0.113.7 ", proxy) == "203.0.113.7");
  CHECK(client_address("10.0.0.1", "", proxy) == "10.0.0.1");
  CHECK(client_address("10.0.0.1", "not an address", proxy) == "10.0.0.1");
  // Someone else sending the header: ignored.
  CHECK(client_address("10.0.0.2", "203.0.113.7", proxy) == "10.0.0.2");
  // Proxies in a chain, all trusted: the first address none of them is.
  std::vector<std::string> two = {"10.0.0.1", "172.16.0.0/12"};
  CHECK(client_address("10.0.0.1", "203.0.113.7, 172.18.0.3", two) == "203.0.113.7");
  CHECK(client_address("172.18.0.3", "203.0.113.7", two) == "203.0.113.7");  // a Docker network
  CHECK(client_address("172.32.0.1", "203.0.113.7", two) == "172.32.0.1");  // outside the /12
  CHECK(client_address("10.0.0.1", "172.18.0.3", two) == "10.0.0.1");      // every hop trusted
  // IPv6, and IPv4 written as IPv6.
  std::vector<std::string> six = {"fd00::/8"};
  CHECK(client_address("fd12::1", "2001:db8::5", six) == "2001:db8::5");
  CHECK(client_address("fe80::1", "2001:db8::5", six) == "fe80::1");
  CHECK(client_address("::ffff:10.0.0.1", "203.0.113.7", proxy) == "203.0.113.7");
  CHECK(from_trusted_proxy("10.0.0.1", proxy));
  CHECK(!from_trusted_proxy("10.0.0.1", none));
}

static void test_proxy_entries() {
  for (const char *ok : {"10.0.0.1", "10.0.0.0/8", "0.0.0.0/0", "192.168.1.0/24", "::1", "fd00::/8", "2001:db8::/32"})
    CHECK(proxy_entry_ok(ok));
  for (const char *bad : {"", "10.0.0", "10.0.0.1/33", "10.0.0.1/", "10.0.0.1/x", "fd00::/129", "proxy.lan", " 10.0.0.1"})
    CHECK(!proxy_entry_ok(bad));
}

int main() {
  test_a_session();
  test_the_oldest_goes_first();
  test_the_lockout();
  test_the_cookie();
  test_behind_a_proxy();
  test_proxy_entries();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
