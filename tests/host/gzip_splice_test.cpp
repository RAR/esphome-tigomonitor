// Host test for components/tigo_server/gzip_splice.h: the ESP32 keeps the web
// UI gzipped in flash and splices the API token in at request time. The page
// it sends must inflate to app.html with the token in place of the
// placeholder, byte for byte.
//
// Build: needs the generated header (web_assets_gen.py --out DIR/web_assets.h)
// on the include path, APP_HTML_PATH pointing at the source page, and -lz.
#include "gzip_splice.h"
#include "web_assets.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                   \
    }                                                               \
  } while (0)

using namespace tigo_gzip;

static uint32_t crc_of(const std::string &s) {
  return crc32_update(0, reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

static void test_crc_check_value() {
  CHECK(crc_of("123456789") == 0xCBF43926u);
  CHECK(crc_of("") == 0u);
  // Same as zlib.
  const std::string s = "TigoMonitor";
  CHECK(crc_of(s) == crc32(0, reinterpret_cast<const Bytef *>(s.data()), s.size()));
}

static void test_combine_matches_concatenation() {
  std::string all;
  for (int i = 0; i < 5000; i++) all += static_cast<char>((i * 131 + 7) & 0xff);
  for (size_t cut : {size_t(0), size_t(1), size_t(4), size_t(999), size_t(4999), size_t(5000)}) {
    std::string a = all.substr(0, cut), b = all.substr(cut);
    CHECK(crc32_combine(crc_of(a), crc_of(b), b.size()) == crc_of(all));
  }
}

static std::string inflate_gzip(const std::string &gz, int *rc_out) {
  z_stream z{};
  inflateInit2(&z, 16 + MAX_WBITS);
  z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(gz.data()));
  z.avail_in = gz.size();
  std::string out;
  char buf[65536];
  int rc;
  do {
    z.next_out = reinterpret_cast<Bytef *>(buf);
    z.avail_out = sizeof buf;
    rc = inflate(&z, Z_NO_FLUSH);
    out.append(buf, sizeof buf - z.avail_out);
  } while (rc == Z_OK);
  *rc_out = (rc == Z_STREAM_END && z.avail_in == 0) ? 0 : rc;
  inflateEnd(&z);
  return out;
}

static std::string send_page(const std::string &token) {
  std::string wire;
  bool ok = send_spliced(esphome::tigo_server::APP_HTML_GZ, token.data(), token.size(),
                         [&](const void *p, size_t n) {
                           wire.append(static_cast<const char *>(p), n);
                           return true;
                         });
  CHECK(ok);
  return wire;
}

static void test_page_inflates_with_token(const std::string &source, const std::string &token) {
  std::string expect = source;
  const std::string ph = "__TIGO_API_TOKEN__";
  size_t at = expect.find(ph);
  CHECK(at != std::string::npos);
  expect.replace(at, ph.size(), token);

  std::string wire = send_page(token);
  int rc = -1;
  std::string got = inflate_gzip(wire, &rc);
  CHECK(rc == 0);  // a clean end: CRC and length in the trailer both checked by zlib
  CHECK(got == expect);
  // Smaller than a third of the page: the point of the exercise.
  CHECK(wire.size() * 3 < source.size());
}

static void test_send_failure_stops() {
  int calls = 0;
  bool ok = send_spliced(esphome::tigo_server::APP_HTML_GZ, "t", 1, [&](const void *, size_t) {
    calls++;
    return false;
  });
  CHECK(!ok);
  CHECK(calls == 1);
}

int main() {
  std::ifstream in(APP_HTML_PATH, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string source = ss.str();
  CHECK(!source.empty());

  test_crc_check_value();
  test_combine_matches_concatenation();
  test_page_inflates_with_token(source, "0123456789abcdef0123456789abcdef");
  test_page_inflates_with_token(source, "");
  test_send_failure_stops();

  if (failures) {
    std::printf("gzip_splice_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("gzip_splice_test: all passed\n");
  return 0;
}
