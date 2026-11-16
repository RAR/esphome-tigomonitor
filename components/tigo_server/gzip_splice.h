// Serve a gzipped page with a value spliced into it, without inflating it.
//
// web_assets_gen.py stores the page as one gzip stream cut at the API-token
// placeholder: PRE (gzip header + deflate, sync-flushed so it ends on a byte
// boundary and is not final) and POST (the final deflate blocks). Between
// them we send a stored (uncompressed) deflate block holding the token, and
// after them the gzip trailer: CRC-32 and length of the whole inflated page.
// The CRC is CRC(PRE) extended over the token, then combined with CRC(POST)
// (zlib's crc32_combine), so the device never needs POST's text.
//
// Plain C++, no ESP-IDF: tests/host/gzip_splice_test.cpp inflates the result
// with zlib and compares it with app.html.
#pragma once

#include <cstddef>
#include <cstdint>

namespace tigo_gzip {

struct SplicedPage {
  const uint8_t *pre;
  size_t pre_size;
  uint32_t pre_crc;  // CRC-32 of PRE's inflated text
  uint32_t pre_len;  // its length
  const uint8_t *post;
  size_t post_size;
  uint32_t post_crc;
  uint32_t post_len;
};

constexpr uint32_t kPoly = 0xEDB88320u;

// zlib-compatible: crc32_update(0, ...) == zlib crc32(0, ...).
inline uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (kPoly & (0u - (crc & 1u)));
  }
  return ~crc;
}

// a * b modulo the CRC polynomial (reflected), as in zlib's multmodp.
inline uint32_t crc32_multmodp(uint32_t a, uint32_t b) {
  uint32_t m = 1u << 31, p = 0;
  for (;;) {
    if (a & m) {
      p ^= b;
      if ((a & (m - 1)) == 0) break;
    }
    m >>= 1;
    b = (b & 1) ? (b >> 1) ^ kPoly : b >> 1;
  }
  return p;
}

// CRC of A followed by B, from CRC(A), CRC(B) and B's length.
inline uint32_t crc32_combine(uint32_t crc1, uint32_t crc2, uint64_t len2) {
  uint32_t q = 1u << 30;  // x^1
  for (int i = 0; i < 3; i++) q = crc32_multmodp(q, q);  // x^8: one byte
  uint32_t p = 1u << 31;  // x^0
  while (len2) {
    if (len2 & 1) p = crc32_multmodp(q, p);
    len2 >>= 1;
    q = crc32_multmodp(q, q);
  }
  return crc32_multmodp(p, crc1) ^ crc2;
}

// Sends the page with `mid` (at most 65535 bytes) in place of the
// placeholder. `send(const void*, size_t) -> bool` writes one piece; a false
// stops the response and is returned.
template<typename Send>
bool send_spliced(const SplicedPage &pg, const char *mid, size_t mid_len, Send &&send) {
  if (mid_len > 0xFFFF) return false;
  if (!send(pg.pre, pg.pre_size)) return false;

  // Stored block, not final: BFINAL=0 BTYPE=00 in a byte (PRE ended
  // byte-aligned), then LEN and its complement, little-endian.
  const uint16_t len = static_cast<uint16_t>(mid_len);
  const uint8_t hdr[5] = {0x00, static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8),
                          static_cast<uint8_t>(~len), static_cast<uint8_t>(~len >> 8)};
  if (!send(hdr, sizeof hdr)) return false;
  if (mid_len && !send(mid, mid_len)) return false;
  if (!send(pg.post, pg.post_size)) return false;

  uint32_t crc = crc32_update(pg.pre_crc, reinterpret_cast<const uint8_t *>(mid), mid_len);
  crc = crc32_combine(crc, pg.post_crc, pg.post_len);
  const uint32_t isize = pg.pre_len + static_cast<uint32_t>(mid_len) + pg.post_len;
  const uint8_t trailer[8] = {
      static_cast<uint8_t>(crc),   static_cast<uint8_t>(crc >> 8),   static_cast<uint8_t>(crc >> 16),
      static_cast<uint8_t>(crc >> 24), static_cast<uint8_t>(isize), static_cast<uint8_t>(isize >> 8),
      static_cast<uint8_t>(isize >> 16), static_cast<uint8_t>(isize >> 24)};
  return send(trailer, sizeof trailer);
}

}  // namespace tigo_gzip
