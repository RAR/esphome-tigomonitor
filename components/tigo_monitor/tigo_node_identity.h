#pragma once

// Keeping a panel's CCA metadata attached to the panel, not to its short
// address (#74).
//
// A panel has two identifiers on the bus: a 16-hex-char long address, which is
// its barcode and never changes, and a 4-hex-char short address, which the
// gateway hands out at commissioning. The node table is keyed by the short
// address, but the labels in it (cca_label, string, MPPT, ...) describe a
// physical panel. When the gateway re-commissions — a rediscovery is enough —
// every panel can get a new short address at once, and a Frame 27 then pairs
// an existing short address with a different barcode. Before this, the entry
// kept its labels and swapped only the barcode, so every label moved to
// whichever panel inherited the address.
//
// The functions here make labels follow the barcode instead, both as Frame 27
// arrives and when a backup of the table is imported. Header-only and
// free of ESPHome dependencies so the renumbering cases can be exercised on
// the host (tests/host/node_identity_test.cpp); Node is NodeTableData in the
// firmware.

#include <cstddef>

namespace esphome {
namespace tigo_monitor {
namespace node_identity {

template<class Node> bool has_cca_metadata(const Node &n) {
  return !n.cca_label.empty() || !n.cca_string_label.empty() || !n.cca_mppt_label.empty() ||
         !n.cca_channel.empty() || !n.cca_object_id.empty() || n.cca_validated;
}

template<class Node> void clear_cca_metadata(Node &n) {
  n.cca_label.clear();
  n.cca_string_label.clear();
  n.cca_mppt_label.clear();
  n.cca_channel.clear();
  n.cca_object_id.clear();
  n.cca_validated = false;
}

// Copy src's metadata into the fields dst has empty. The fill-gaps merge the
// long-address dedup has always used.
template<class Node> void fill_cca_metadata(Node &dst, const Node &src) {
  if (dst.cca_label.empty()) dst.cca_label = src.cca_label;
  if (dst.cca_string_label.empty()) dst.cca_string_label = src.cca_string_label;
  if (dst.cca_mppt_label.empty()) dst.cca_mppt_label = src.cca_mppt_label;
  if (dst.cca_channel.empty()) dst.cca_channel = src.cca_channel;
  if (dst.cca_object_id.empty()) dst.cca_object_id = src.cca_object_id;
  dst.cca_validated = dst.cca_validated || src.cca_validated;
}

// Metadata whose panel has not been seen at its new short address yet.
//
// A renumbering arrives one Frame 27 entry at a time, and the gateway may
// split it across several frames. When entry `0004 -> barcode B` lands, the
// labels that were on 0004 belong to its previous barcode A — whose new
// address may not have been announced yet. They wait here, keyed by A, until
// it is. Runtime only: the node table on flash is saved after the moves.
template<class Node, class Pool> void park(Pool &pool, const Node &from) {
  if (from.long_address.empty() || !has_cca_metadata(from)) return;
  for (auto &p : pool) {
    if (p.long_address == from.long_address) {
      p = from;
      return;
    }
  }
  pool.push_back(from);
}

// Hand `to` the metadata parked for its barcode, if `to` has none of its own.
template<class Node, class Pool> bool adopt_parked(Pool &pool, Node &to) {
  if (to.long_address.empty() || has_cca_metadata(to)) return false;
  for (auto it = pool.begin(); it != pool.end(); ++it) {
    if (it->long_address == to.long_address) {
      fill_cca_metadata(to, *it);
      pool.erase(it);
      return true;
    }
  }
  return false;
}

struct Result {
  bool table_changed = false;
  bool metadata_moved = false;  // string groups need rebuilding
  bool created = false;
  bool table_full = false;      // addr had no entry and none could be made
  bool renumbered = false;      // addr previously held a different barcode
};

// Apply one Frame 27 entry — the gateway vouching that short address `addr`
// is the panel with barcode `long_addr` — to the node table.
//
// make_node(addr, long_addr) builds a fresh entry (the firmware fills in the
// checksum). on_alias_removed(node) is called just before an entry is erased
// as a stale alias of the same barcode, so the caller can drop its runtime
// device row.
template<class Table, class Pool, class Str, class MakeNode, class OnRemoved>
Result apply_frame27_entry(Table &table, Pool &pool, const Str &addr, const Str &long_addr,
                           size_t max_nodes, MakeNode make_node, OnRemoved on_alias_removed) {
  Result r;
  auto find = [&](const Str &a) -> decltype(&table[0]) {
    for (auto &n : table)
      if (n.addr == a) return &n;
    return nullptr;
  };

  auto *node = find(addr);
  if (node != nullptr) {
    if (node->long_address != long_addr) {
      if (!node->long_address.empty()) {
        // Renumbering: this address now belongs to a different panel. Its
        // labels go with the panel that left, not the one that arrived.
        park(pool, *node);
        if (has_cca_metadata(*node)) r.metadata_moved = true;
        clear_cca_metadata(*node);
        r.renumbered = true;
      }
      node->long_address = long_addr;
      r.table_changed = true;
    }
  } else if (table.size() < max_nodes) {
    table.push_back(make_node(addr, long_addr));
    r.created = true;
    r.table_changed = true;
  } else {
    r.table_full = true;
    return r;
  }

  // Any OTHER entry carrying this barcode is where the panel used to be: a
  // pre-renumbering address, or an ephemeral commissioning alias (#25, e.g.
  // 8002 alongside 0002). Its metadata comes here and the entry goes.
  for (size_t i = 0; i < table.size(); ++i) {
    if (table[i].addr == addr || table[i].long_address != long_addr) continue;
    auto *keep = find(addr);
    if (keep == nullptr) break;
    auto &dup = table[i];
    if (keep->sensor_index < 0 && dup.sensor_index >= 0) keep->sensor_index = dup.sensor_index;
    if (has_cca_metadata(dup)) r.metadata_moved = true;
    fill_cca_metadata(*keep, dup);
    keep->is_persistent = keep->is_persistent || dup.is_persistent;
    on_alias_removed(dup);
    table.erase(table.begin() + i);
    --i;
    r.table_changed = true;
  }

  // The panel's previous address may already have been reassigned earlier in
  // this renumbering, in which case its labels are parked rather than in the
  // table.
  if (auto *keep = find(addr)) {
    keep->seen_in_frame27 = true;
    if (adopt_parked(pool, *keep)) {
      r.metadata_moved = true;
      r.table_changed = true;
    }
  }
  return r;
}

// What import_backup did with an entry of the backup.
enum class ImportEvent {
  moved,              // its barcode is at another address now; placed there
  held,               // its address holds another panel now; labels parked
  no_address,         // skipped
  duplicate_address,  // a second entry for one address; skipped
  merged,             // its barcode was already imported; labels merged there
};

struct ImportCounts {
  int remapped = 0;
  int parked = 0;
};

// Replace the node table with a backup of it, such as a Node Table export.
//
// A backup records the short addresses of the day it was exported. If the
// gateway has renumbered since, restoring those as-is puts every label on
// whichever panel holds the old address now. The barcode -> address pairs a
// Frame 27 has confirmed since start (seen_in_frame27) win: an entry whose
// barcode is confirmed goes to its confirmed address, and one whose address
// is confirmed for a different panel, while its own barcode has not been
// announced yet, has its labels parked until it is. Confirmed entries are
// placed first, so the duplicate-address check keeps them over a stale entry
// naming the same address. Two entries sharing a barcode are one panel (a
// commissioning alias, #25): the first stays, the second fills its gaps.
//
// prepare(node) runs on every entry kept (the firmware sets its checksum).
// on_event(event, entry, addr) reports each entry the backup did not restore
// as written; addr is where it went (moved, merged) or its own address.
template<class Table, class Pool, class Prepare, class OnEvent>
ImportCounts import_backup(Table &table, Pool &pool, const Table &backup, Prepare prepare, OnEvent on_event) {
  ImportCounts c;
  Table live;  // the confirmed pairs, from the table being replaced
  for (const auto &n : table)
    if (n.seen_in_frame27 && !n.long_address.empty()) live.push_back(n);
  table.clear();
  pool.clear();

  for (int pass = 0; pass < 2; pass++) {
    for (const auto &in : backup) {
      const typename Table::value_type *confirmed = nullptr;
      if (!in.long_address.empty())
        for (const auto &n : live)
          if (n.long_address == in.long_address) confirmed = &n;
      if ((pass == 0) != (confirmed != nullptr)) continue;

      auto node = in;
      if (confirmed != nullptr) {
        node.seen_in_frame27 = true;
        if (node.addr != confirmed->addr) {
          on_event(ImportEvent::moved, in, confirmed->addr);
          node.addr = confirmed->addr;
          c.remapped++;
        }
      } else if (node.addr.empty()) {
        on_event(ImportEvent::no_address, in, in.addr);
        continue;
      } else if (!node.long_address.empty()) {
        bool taken = false;
        for (const auto &n : live)
          if (n.addr == node.addr) taken = true;
        if (taken) {
          on_event(ImportEvent::held, in, in.addr);
          park(pool, node);
          c.parked++;
          continue;
        }
      }

      bool placed = false;
      for (auto &existing : table) {
        if (existing.addr == node.addr) {
          on_event(ImportEvent::duplicate_address, in, node.addr);
          placed = true;
          break;
        }
        if (!node.long_address.empty() && existing.long_address == node.long_address) {
          if (existing.sensor_index < 0 && node.sensor_index >= 0) existing.sensor_index = node.sensor_index;
          fill_cca_metadata(existing, node);
          on_event(ImportEvent::merged, in, existing.addr);
          placed = true;
          break;
        }
      }
      if (placed) continue;
      prepare(node);
      table.push_back(node);
    }
  }
  return c;
}

}  // namespace node_identity
}  // namespace tigo_monitor
}  // namespace esphome
