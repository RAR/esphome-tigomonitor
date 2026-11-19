#pragma once

// The array layout: where each panel and each TAP physically sits, as
// Topology > Array places them on a grid of panel-sized cells. One cell
// per panel, so panels in touching cells are neighbours; a gap, such as
// between roof faces or ground-mount rows, is empty cells. Portrait or
// landscape only changes how a panel is drawn. A TAP can share a panel's
// square, as when it's mounted under that panel.
//
// Kept apart from the wiring layout (inverter > MPPT > string > panel): that
// says how panels are connected, this says where they are.
//
// Pure and header-only, so both builds keep it the same way: the Linux
// daemon (linux/src/array_layout.h wraps it for nlohmann::json) and the
// ESP32's web server. It reads /api/array's JSON itself rather than through
// a JSON library: on the ESP32 a parsed tree of a large layout would land on
// the small internal heap, and this writes nothing but the layout (into the
// PSRAM types of bus_watch.h's TIGO_BUS_ALLOCATOR).

#include "bus_watch.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace esphome {
namespace tigo_monitor {
namespace array_grid {

using master::Map;
using master::Str;
using master::Vec;

struct ArrayPanel {
  Str addr;          // the optimizer's short address, 4 hex digits, upper case
  int x = 0, y = 0;  // the cell, columns and rows from the top left
  bool portrait = true;
  Str barcode;  // its barcode, upper case; empty in a layout saved before
                // barcodes were kept. The address can change under a
                // panel (a renumbering); the barcode can't.
};

struct ArrayTap {
  Str id;  // the TAP's RS485 id, 4 hex digits, upper case
  int x = 0, y = 0;
};

struct ArrayLayout {
  Vec<ArrayPanel> panels;
  Vec<ArrayTap> taps;
};

static const int ARRAY_CELLS = 200;    // columns and rows, each 0..199
static const size_t ARRAY_MAX = 1024;  // panels, as a TAP's table holds
static const int JSON_DEPTH = 64;      // nesting followed in a key that is skipped

// ---- reading JSON ------------------------------------------------------------

// One JSON value as the layout reads it: a string or a number kept, anything
// else known only by its kind.
struct JsonVal {
  enum Kind { NONE, STR, INT, NUM, BOOL, NUL, OBJ, ARR } kind = NONE;
  Str s;
  long long i = 0;
  bool b = false;
  const char *at = nullptr;  // where an OBJ or ARR starts, to read it after
};

// Strict JSON (RFC 8259), as nlohmann::json parses it: valid UTF-8, paired
// surrogates, no leading zeros, nothing after the value.
class JsonIn {
 public:
  JsonIn(const char *p, const char *e) : p_(p), e_(e) {}
  const char *pos() const { return p_; }
  void ws() {
    while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) p_++;
  }
  bool eat(char c) {
    ws();
    if (p_ < e_ && *p_ == c) {
      p_++;
      return true;
    }
    return false;
  }
  bool at_end() {
    ws();
    return p_ == e_;
  }
  // Any value into v; an object's or array's contents are checked and skipped.
  bool value(JsonVal &v, int depth = 0) {
    ws();
    if (p_ >= e_) return false;
    v = JsonVal();
    char c = *p_;
    if (c == '"') {
      v.kind = JsonVal::STR;
      return string(v.s);
    }
    if (c == '{' || c == '[') {
      if (depth >= JSON_DEPTH) return false;
      v.kind = c == '{' ? JsonVal::OBJ : JsonVal::ARR;
      v.at = p_;
      return skip_container_(depth);
    }
    if (c == 't') return word_("true") && (v.kind = JsonVal::BOOL, v.b = true, true);
    if (c == 'f') return word_("false") && (v.kind = JsonVal::BOOL, v.b = false, true);
    if (c == 'n') return word_("null") && (v.kind = JsonVal::NUL, true);
    return number_(v);
  }
  bool string(Str &out) {
    if (p_ >= e_ || *p_ != '"') return false;
    p_++;
    out.clear();
    while (p_ < e_) {
      unsigned char c = static_cast<unsigned char>(*p_);
      if (c == '"') {
        p_++;
        return true;
      }
      if (c < 0x20) return false;
      if (c == '\\') {
        if (++p_ >= e_) return false;
        char x = *p_++;
        switch (x) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            unsigned cp;
            if (!hex4_(cp)) return false;
            if (cp >= 0xDC00 && cp <= 0xDFFF) return false;  // a low surrogate alone
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              unsigned lo;
              if (e_ - p_ < 2 || p_[0] != '\\' || p_[1] != 'u') return false;
              p_ += 2;
              if (!hex4_(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            utf8_(out, cp);
            break;
          }
          default:
            return false;
        }
        continue;
      }
      size_t n = utf8_len_(p_);
      if (!n) return false;
      out.append(p_, n);
      p_ += n;
    }
    return false;
  }

 private:
  bool word_(const char *w) {
    size_t n = std::strlen(w);
    if (static_cast<size_t>(e_ - p_) < n || std::memcmp(p_, w, n) != 0) return false;
    p_ += n;
    return true;
  }
  bool number_(JsonVal &v) {
    const char *s = p_;
    if (p_ < e_ && *p_ == '-') p_++;
    if (p_ >= e_ || !std::isdigit(static_cast<unsigned char>(*p_))) return false;
    if (*p_ == '0') {
      p_++;
    } else {
      while (p_ < e_ && std::isdigit(static_cast<unsigned char>(*p_))) p_++;
    }
    bool integer = true;
    if (p_ < e_ && *p_ == '.') {
      integer = false;
      p_++;
      if (p_ >= e_ || !std::isdigit(static_cast<unsigned char>(*p_))) return false;
      while (p_ < e_ && std::isdigit(static_cast<unsigned char>(*p_))) p_++;
    }
    if (p_ < e_ && (*p_ == 'e' || *p_ == 'E')) {
      integer = false;
      p_++;
      if (p_ < e_ && (*p_ == '+' || *p_ == '-')) p_++;
      if (p_ >= e_ || !std::isdigit(static_cast<unsigned char>(*p_))) return false;
      while (p_ < e_ && std::isdigit(static_cast<unsigned char>(*p_))) p_++;
    }
    v.kind = JsonVal::NUM;
    // Whole and small enough for a cell: an integer. Anything longer is out
    // of range either way, as a float or a huge number is on the Pi.
    if (integer && p_ - s <= 18) {
      char b[24];
      std::memcpy(b, s, p_ - s);
      b[p_ - s] = '\0';
      v.kind = JsonVal::INT;
      v.i = std::strtoll(b, nullptr, 10);
    }
    return true;
  }
  bool skip_container_(int depth) {
    char open = *p_++;
    char close = open == '{' ? '}' : ']';
    if (eat(close)) return true;
    while (true) {
      if (open == '{') {
        Str k;
        ws();
        if (!string(k) || !eat(':')) return false;
      }
      JsonVal v;
      if (!value(v, depth + 1)) return false;
      if (eat(close)) return true;
      if (!eat(',')) return false;
    }
  }
  bool hex4_(unsigned &cp) {
    if (e_ - p_ < 4) return false;
    cp = 0;
    for (int i = 0; i < 4; i++) {
      char c = *p_++;
      cp <<= 4;
      if (c >= '0' && c <= '9') cp |= c - '0';
      else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
      else return false;
    }
    return true;
  }
  static void utf8_(Str &out, unsigned cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  // The length of the well-formed UTF-8 character at p, or 0.
  size_t utf8_len_(const char *p) const {
    auto at = [&](size_t i) { return static_cast<unsigned char>(p[i]); };
    auto cont = [&](size_t i) { return p + i < e_ && (at(i) & 0xC0) == 0x80; };
    unsigned char c = at(0);
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return cont(1) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
      if (!cont(1) || !cont(2)) return 0;
      if (c == 0xE0 && at(1) < 0xA0) return 0;  // overlong
      if (c == 0xED && at(1) >= 0xA0) return 0;  // a surrogate
      return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
      if (!cont(1) || !cont(2) || !cont(3)) return 0;
      if (c == 0xF0 && at(1) < 0x90) return 0;
      if (c == 0xF4 && at(1) >= 0x90) return 0;
      return 4;
    }
    return 0;
  }

  const char *p_, *e_;
};

// An object's members, each value as JsonVal (the last of a repeated key
// wins, as nlohmann::json has it). False if it isn't one.
template <class Fn> inline bool json_members(const char *at, const char *end, Fn fn) {
  JsonIn in(at, end);
  if (!in.eat('{')) return false;
  if (in.eat('}')) return true;
  while (true) {
    Str k;
    JsonVal v;
    in.ws();
    if (!in.string(k) || !in.eat(':') || !in.value(v)) return false;
    fn(k, v);
    if (in.eat('}')) return true;
    if (!in.eat(',')) return false;
  }
}

// An array's elements, each as JsonVal. False if it isn't one.
template <class Fn> inline bool json_elements(const char *at, const char *end, Fn fn) {
  JsonIn in(at, end);
  if (!in.eat('[')) return false;
  if (in.eat(']')) return true;
  while (true) {
    JsonVal v;
    if (!in.value(v)) return false;
    fn(v);
    if (in.eat(']')) return true;
    if (!in.eat(',')) return false;
  }
}

// ---- the rules -----------------------------------------------------------------

// 4 hex digits, upper-cased into out; false otherwise.
inline bool array_hex4(const JsonVal &v, Str &out) {
  if (v.kind != JsonVal::STR || v.s.size() != 4) return false;
  Str s = v.s;
  for (char &c : s) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  out = s;
  return true;
}

// 1-32 letters and digits, upper-cased into out; false otherwise.
inline bool array_barcode(const JsonVal &v, Str &out) {
  if (v.kind != JsonVal::STR || v.s.empty() || v.s.size() > 32) return false;
  Str s = v.s;
  for (char &c : s) {
    if (!std::isalnum(static_cast<unsigned char>(c))) return false;
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  out = s;
  return true;
}

inline bool array_cell(const JsonVal &x, const JsonVal &y, int &cx, int &cy) {
  auto ok = [](const JsonVal &v) { return v.kind == JsonVal::INT && v.i >= 0 && v.i < ARRAY_CELLS; };
  if (!ok(x) || !ok(y)) return false;
  cx = static_cast<int>(x.i);
  cy = static_cast<int>(y.i);
  return true;
}

inline Str array_num_(long long v) {
  char b[24];
  std::snprintf(b, sizeof(b), "%lld", v);
  return b;
}

// {"panels": [{"addr", "barcode"?, "x", "y", "portrait"}], "taps": [{"id", "x", "y"}]}
// into out; "" when it's an array, else why not (out is then unchanged).
// "Expected a JSON object." when the text isn't JSON at all.
inline Str array_from_text(const char *text, size_t n, ArrayLayout &out) {
  const char *end = text + n;
  {
    JsonIn in(text, end);
    JsonVal all;
    if (!in.value(all) || !in.at_end()) return "Expected a JSON object.";
  }
  JsonVal panels, taps;
  bool object = json_members(text, end, [&](const Str &k, const JsonVal &v) {
    if (k == "panels") panels = v;
    else if (k == "taps") taps = v;
  });
  if (!object || panels.kind != JsonVal::ARR || taps.kind != JsonVal::ARR)
    return "expected {\"panels\": [...], \"taps\": [...]}";
  size_t count = 0;
  json_elements(panels.at, end, [&](const JsonVal &) { count++; });
  if (count > ARRAY_MAX) return "too many panels";
  ArrayLayout r;
  Map<Str, bool> addrs, ids, barcodes;
  Map<std::pair<int, int>, bool> panel_cells, tap_cells;  // a TAP may share a panel's square, as under it
  Str err;
  json_elements(panels.at, end, [&](const JsonVal &p) {
    if (!err.empty()) return;
    JsonVal addr, x, y, portrait, barcode;
    if (p.kind == JsonVal::OBJ)
      json_members(p.at, end, [&](const Str &k, const JsonVal &v) {
        if (k == "addr") addr = v;
        else if (k == "x") x = v;
        else if (k == "y") y = v;
        else if (k == "portrait") portrait = v;
        else if (k == "barcode") barcode = v;
      });
    ArrayPanel rp;
    if (p.kind != JsonVal::OBJ || !array_hex4(addr, rp.addr)) {
      err = "a panel's addr must be 4 hex digits";
      return;
    }
    if (!array_cell(x, y, rp.x, rp.y)) {
      err = "panel " + rp.addr + ": x and y must be whole numbers 0-199";
      return;
    }
    if (barcode.kind != JsonVal::NONE && !array_barcode(barcode, rp.barcode)) {
      err = "panel " + rp.addr + ": barcode must be letters and digits";
      return;
    }
    if (!rp.barcode.empty() && !barcodes.emplace(rp.barcode, true).second) {
      err = "barcode " + rp.barcode + " is placed twice";
      return;
    }
    if (portrait.kind != JsonVal::NONE) {
      if (portrait.kind != JsonVal::BOOL) {
        err = "panel " + rp.addr + ": portrait must be true or false";
        return;
      }
      rp.portrait = portrait.b;
    }
    if (!addrs.emplace(rp.addr, true).second) {
      err = "panel " + rp.addr + " is placed twice";
      return;
    }
    if (!panel_cells.emplace(std::make_pair(rp.x, rp.y), true).second) {
      err = "two panels are in the same place, at " + array_num_(rp.x) + "," + array_num_(rp.y);
      return;
    }
    r.panels.push_back(rp);
  });
  if (!err.empty()) return err;
  json_elements(taps.at, end, [&](const JsonVal &t) {
    if (!err.empty()) return;
    JsonVal id, x, y;
    if (t.kind == JsonVal::OBJ)
      json_members(t.at, end, [&](const Str &k, const JsonVal &v) {
        if (k == "id") id = v;
        else if (k == "x") x = v;
        else if (k == "y") y = v;
      });
    ArrayTap rt;
    if (t.kind != JsonVal::OBJ || !array_hex4(id, rt.id)) {
      err = "a TAP's id must be 4 hex digits";
      return;
    }
    if (!array_cell(x, y, rt.x, rt.y)) {
      err = "TAP " + rt.id + ": x and y must be whole numbers 0-199";
      return;
    }
    if (!ids.emplace(rt.id, true).second) {
      err = "TAP " + rt.id + " is placed twice";
      return;
    }
    if (!tap_cells.emplace(std::make_pair(rt.x, rt.y), true).second) {
      err = "two TAPs are in the same place, at " + array_num_(rt.x) + "," + array_num_(rt.y);
      return;
    }
    r.taps.push_back(rt);
  });
  if (!err.empty()) return err;
  out = std::move(r);
  return "";
}
template <class S> inline Str array_from_text(const S &text, ArrayLayout &out) {
  return array_from_text(text.data(), text.size(), out);
}

// The layout as /api/array gives it, into out (anything with
// append(const char *)): compact, keys in order, as nlohmann::json dumps it.
template <class Out> void array_write_json(Out &out, const ArrayLayout &r) {
  auto str = [&](const Str &s) {  // hex and letters and digits only, by the rules
    out.append("\"");
    out.append(s.c_str());
    out.append("\"");
  };
  auto num = [&](int v) { out.append(array_num_(v).c_str()); };
  out.append("{\"panels\":[");
  for (size_t i = 0; i < r.panels.size(); i++) {
    const ArrayPanel &p = r.panels[i];
    if (i) out.append(",");
    out.append("{\"addr\":"), str(p.addr);
    if (!p.barcode.empty()) out.append(",\"barcode\":"), str(p.barcode);
    out.append(",\"portrait\":"), out.append(p.portrait ? "true" : "false");
    out.append(",\"x\":"), num(p.x), out.append(",\"y\":"), num(p.y), out.append("}");
  }
  out.append("],\"taps\":[");
  for (size_t i = 0; i < r.taps.size(); i++) {
    const ArrayTap &t = r.taps[i];
    if (i) out.append(",");
    out.append("{\"id\":"), str(t.id);
    out.append(",\"x\":"), num(t.x), out.append(",\"y\":"), num(t.y), out.append("}");
  }
  out.append("]}");
}

// ---- following barcodes ------------------------------------------------------

// Each panel to the address its barcode has now (barcode -> address, upper
// case), so a renumbering doesn't move panels around the drawing. A panel
// with no barcode saved keeps its address, unless a panel that has one now
// holds that address: then it can't be told which panel it was, and it goes
// back to the tray. A barcode not in the table yet keeps its address.
template <class M> void array_follow_barcodes(ArrayLayout &r, const M &addr_of) {
  Map<Str, bool> claimed;
  for (auto &p : r.panels) {
    if (p.barcode.empty()) continue;
    auto it = addr_of.find(p.barcode);
    if (it != addr_of.end()) p.addr = it->second;
    claimed[p.addr] = true;
  }
  Vec<ArrayPanel> kept;
  Map<Str, bool> seen;
  for (auto &p : r.panels) {
    if (p.barcode.empty() && claimed.count(p.addr)) continue;
    if (!seen.emplace(p.addr, true).second) continue;  // two barcodes now at one address: the first wins
    kept.push_back(p);
  }
  r.panels = std::move(kept);
}

// Give each panel with no barcode the one its address has now (address ->
// barcode), as a save does, so the layout follows barcodes from then on.
template <class M> void array_fill_barcodes(ArrayLayout &r, const M &barcode_of) {
  for (auto &p : r.panels) {
    if (!p.barcode.empty()) continue;
    auto it = barcode_of.find(p.addr);
    if (it != barcode_of.end()) p.barcode = it->second;
  }
}

// From a node table (anything with .addr and .long_address, each with
// c_str()): barcode -> address and address -> barcode, upper case. A
// provisional alias (0x8000 set) loses to the real address of the same barcode.
template <class Nodes, class M>
void array_node_maps(const Nodes &nodes, M &addr_of, M &barcode_of) {
  auto up = [](const char *v) {
    Str s(v);
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
  };
  auto alias = [](const Str &a) { return !a.empty() && std::strchr("89ABCDEF", a[0]) != nullptr; };
  for (const auto &n : nodes) {
    Str a = up(n.addr.c_str()), b = up(n.long_address.c_str());
    if (b.empty() || a.size() != 4) continue;
    auto it = addr_of.find(b);
    if (it == addr_of.end() || (alias(it->second) && !alias(a))) addr_of[b] = a;
    barcode_of[a] = b;
  }
}

}  // namespace array_grid
}  // namespace tigo_monitor
}  // namespace esphome
