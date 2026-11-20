#pragma once

// The container types the shared headers keep what they hear in: bus_watch.h,
// bus_book.h, array_grid.h, link_stats.h and mesh_history.h. Plain std types,
// unless the build sets TIGO_BUS_ALLOCATOR first.

#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace tigo_monitor {
namespace master {

// What they keep takes its memory from TIGO_BUS_ALLOCATOR when it is
// defined, as tigo_layout.h does.
// The ESP32 sets it to its PSRAM allocator: plain std types there come from
// the ~95 KB internal heap, which five minutes of poll marks alone ran out
// on the rig (2026-11-19). The Pi and the host tests leave it unset, so these
// are the std types there.
#ifdef TIGO_BUS_ALLOCATOR
template <class T> using BusAlloc = TIGO_BUS_ALLOCATOR<T>;
#else
template <class T> using BusAlloc = std::allocator<T>;
#endif
using Str = std::basic_string<char, std::char_traits<char>, BusAlloc<char>>;
template <class T> using Vec = std::vector<T, BusAlloc<T>>;
template <class T> using Deque = std::deque<T, BusAlloc<T>>;
template <class K, class V> using Map = std::map<K, V, std::less<K>, BusAlloc<std::pair<const K, V>>>;
// Text from elsewhere (std::string) into one of these, and back.
template <class S> inline Str to_str(const S &s) { return Str(s.data(), s.size()); }
template <class S> inline std::string to_std(const S &s) { return std::string(s.data(), s.size()); }
// std::to_string, into a Str.
inline Str num_str(long long v) {
  char b[24];
  std::snprintf(b, sizeof(b), "%lld", v);
  return Str(b);
}

}  // namespace master
}  // namespace tigo_monitor
}  // namespace esphome
