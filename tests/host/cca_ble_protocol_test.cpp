// Host test for components/tigo_monitor/tigo_cca_ble_protocol.h.
//
//   g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor
//       tests/host/cca_ble_protocol_test.cpp -o /tmp/cca_ble_protocol_test && /tmp/cca_ble_protocol_test
//
// The SSIDs, passwords and nonces here are made up.

#include "tigo_cca_ble_protocol.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace esphome::tigo_monitor::cca_ble;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static void test_uri_encode() {
  std::puts("uri_encode keeps unreserved characters and encodes the rest as the app does");
  CHECK(uri_encode("Az09-_.~") == "Az09-_.~");
  CHECK(uri_encode("My Net") == "My%20Net");
  CHECK(uri_encode("a!'()*b") == "a%21%27%28%29%2Ab");
  CHECK(uri_encode("p=w&d") == "p%3Dw%26d");
  CHECK(uri_encode("\xC3\xA9") == "%C3%A9");  // UTF-8 bytes, uppercase hex
}

static void test_command_text() {
  std::puts("command text puts params before the sid");
  CHECK(command_text("DEVICE_INFO", "", "") == "cmd=DEVICE_INFO");
  CHECK(command_text("NETWORK_INFO", "", "abc") == "cmd=NETWORK_INFO sid=abc");
  CHECK(command_text("NETWORK_WIFI_CONNECT", "nid=My%20Net pwd=x", "abc") ==
        "cmd=NETWORK_WIFI_CONNECT nid=My%20Net pwd=x sid=abc");
}

static void test_request_frame() {
  std::puts("a request is the number, the text and a terminating zero");
  auto f = request_frame<std::vector<uint8_t>>(7, "cmd=X");
  CHECK((f == std::vector<uint8_t>{7, 'c', 'm', 'd', '=', 'X', 0}));
  CHECK(next_request_number(1) == 2);
  CHECK(next_request_number(255) == 1);
}

static void test_reply_assembly() {
  std::puts("a reply is the bodies of its notifications, ending at a zero");
  std::string reply;
  const uint8_t a[] = {7, 0, 0, '{', '"', 'a'};
  const uint8_t b[] = {7, 0, 0, '"', '}', 0};
  const uint8_t short_one[] = {7, 0, 0};
  CHECK(!append_reply_piece(reply, short_one, sizeof(short_one)));
  CHECK(append_reply_piece(reply, a, sizeof(a)));
  CHECK(!reply_complete(reply));
  CHECK(append_reply_piece(reply, b, sizeof(b)));
  CHECK(reply_complete(reply));
  CHECK(reply == std::string("{\"a\"}\0", 6));
}

static void test_session() {
  std::puts("the session key input is uts plus the app's seed, in decimal");
  CHECK(session_key_input(1700000123) == "1700159383");
  CHECK(session_key_input(4294967295ULL) == "4295126555");  // no 32-bit wrap
  CHECK(is_unauthorized(std::string("{\"msg\":\"Unauthorized request\"}")));
  CHECK(!is_unauthorized(std::string("{\"msg\":\"ok\"}")));
}

int main() {
  test_uri_encode();
  test_command_text();
  test_request_frame();
  test_reply_assembly();
  test_session();
  std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
