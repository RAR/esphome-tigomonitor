// Host test for components/tigo_monitor/tigo_node_identity.h (#74).
//
//   g++ -std=c++17 -Wall -Wextra -I components/tigo_monitor
//       tests/host/node_identity_test.cpp -o /tmp/node_identity_test && /tmp/node_identity_test
//
// Barcodes here are made up; none belong to a real installation.

#include "tigo_node_identity.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace esphome::tigo_monitor::node_identity;

struct Node {  // the fields of NodeTableData the helpers touch
  std::string long_address, addr;
  int sensor_index = -1;
  bool is_persistent = true;
  std::string cca_label, cca_string_label, cca_mppt_label, cca_channel, cca_object_id;
  bool cca_validated = false;
  bool seen_in_frame27 = false;
  bool prepared = false;  // set by import_backup's prepare callback
};

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static Node labelled(const std::string &addr, const std::string &bc, const std::string &label) {
  Node n;
  n.addr = addr;
  n.long_address = bc;
  n.cca_label = label;
  n.cca_string_label = label.substr(0, 1);
  n.cca_mppt_label = "MPPT 1";
  return n;
}

struct Harness {
  std::vector<Node> table;
  std::vector<Node> pool;
  std::vector<std::string> removed;
  size_t max_nodes = 64;

  Result feed(const std::string &addr, const std::string &bc) {
    return apply_frame27_entry(
        table, pool, addr, bc, max_nodes,
        [](const std::string &a, const std::string &l) {
          Node n;
          n.addr = a;
          n.long_address = l;
          return n;
        },
        [this](const Node &n) { removed.push_back(n.addr); });
  }
  const Node *at(const std::string &addr) const {
    for (auto &n : table)
      if (n.addr == addr) return &n;
    return nullptr;
  }
  std::string label_of_barcode(const std::string &bc) const {
    for (auto &n : table)
      if (n.long_address == bc) return n.cca_label;
    return "<absent>";
  }
};

static void test_unchanged_frame_is_a_no_op() {
  std::puts("unchanged frame is a no-op");
  Harness h;
  h.table = {labelled("0002", "BC0000000000000A", "A1")};
  auto r = h.feed("0002", "BC0000000000000A");
  CHECK(!r.table_changed);
  CHECK(!r.metadata_moved);
  CHECK(h.at("0002")->cca_label == "A1");
}

static void test_two_panels_swap_addresses() {
  std::puts("two panels swap addresses: labels follow the barcode");
  Harness h;
  h.table = {labelled("0002", "BC0000000000000A", "A1"), labelled("0003", "BC0000000000000B", "A2")};
  h.feed("0002", "BC0000000000000B");
  h.feed("0003", "BC0000000000000A");
  CHECK(h.label_of_barcode("BC0000000000000A") == "A1");
  CHECK(h.label_of_barcode("BC0000000000000B") == "A2");
  CHECK(h.at("0002")->cca_label == "A2");
  CHECK(h.at("0003")->cca_label == "A1");
  CHECK(h.pool.empty());
  CHECK(h.table.size() == 2);
}

static void test_rotation_split_across_frames() {
  std::puts("three-way rotation delivered one entry at a time");
  Harness h;
  h.table = {labelled("0002", "BC000000000000AA", "B1"), labelled("0003", "BC000000000000BB", "B2"),
             labelled("0004", "BC000000000000CC", "B3")};
  // 0002 <- CC, 0003 <- AA, 0004 <- BB, as three separate frames
  auto r = h.feed("0002", "BC000000000000CC");
  CHECK(r.renumbered);
  h.feed("0003", "BC000000000000AA");
  h.feed("0004", "BC000000000000BB");
  CHECK(h.label_of_barcode("BC000000000000AA") == "B1");
  CHECK(h.label_of_barcode("BC000000000000BB") == "B2");
  CHECK(h.label_of_barcode("BC000000000000CC") == "B3");
  CHECK(h.pool.empty());
  CHECK(h.table.size() == 3);
}

static void test_full_shuffle_of_thirty_panels() {
  std::puts("30 panels, every short address reassigned in random order");
  std::mt19937 rng(74);
  Harness h;
  std::vector<std::string> addrs, bcs;
  for (int i = 0; i < 30; i++) {
    char a[5], b[17];
    std::snprintf(a, sizeof(a), "%04X", 2 + i);
    std::snprintf(b, sizeof(b), "04C05BFF%08X", 0x100000 + i * 7919);
    addrs.push_back(a);
    bcs.push_back(b);
    h.table.push_back(labelled(a, b, "P" + std::to_string(i)));
    h.table.back().sensor_index = i;
  }
  std::vector<std::string> new_addrs = addrs;
  std::shuffle(new_addrs.begin(), new_addrs.end(), rng);
  std::vector<int> order(30);
  for (int i = 0; i < 30; i++) order[i] = i;
  std::shuffle(order.begin(), order.end(), rng);
  for (int i : order) h.feed(new_addrs[i], bcs[i]);

  int right = 0;
  for (int i = 0; i < 30; i++)
    if (h.label_of_barcode(bcs[i]) == "P" + std::to_string(i)) right++;
  std::printf("  %d/30 labels on the right panel\n", right);
  CHECK(right == 30);
  CHECK(h.pool.empty());
  CHECK(h.table.size() == 30);
  for (int i = 0; i < 30; i++) CHECK(h.at(new_addrs[i]) && h.at(new_addrs[i])->long_address == bcs[i]);
}

static void test_commissioning_alias_is_merged() {
  std::puts("#25: a commissioning alias carrying the labels is folded into the real address");
  Harness h;
  Node alias = labelled("8002", "BC0000000000000A", "A1");
  alias.sensor_index = 4;
  h.table = {alias};
  auto r = h.feed("0002", "BC0000000000000A");
  CHECK(r.created);
  CHECK(h.table.size() == 1);
  CHECK(h.at("0002") && h.at("0002")->cca_label == "A1");
  CHECK(h.at("0002")->sensor_index == 4);
  CHECK(h.removed.size() == 1 && h.removed[0] == "8002");
}

static void test_first_barcode_keeps_imported_labels() {
  std::puts("an entry learning its barcode for the first time keeps its labels");
  Harness h;
  Node n = labelled("0002", "", "A1");
  h.table = {n};
  auto r = h.feed("0002", "BC0000000000000A");
  CHECK(!r.renumbered);
  CHECK(h.at("0002")->cca_label == "A1");
}

static void test_table_full() {
  std::puts("a full table refuses a new address without touching anything");
  Harness h;
  h.max_nodes = 1;
  h.table = {labelled("0002", "BC0000000000000A", "A1")};
  auto r = h.feed("0003", "BC0000000000000B");
  CHECK(r.table_full);
  CHECK(h.table.size() == 1);
  CHECK(h.at("0002")->cca_label == "A1");
}

struct Import {
  std::vector<Node> table, pool;
  std::vector<std::pair<ImportEvent, std::string>> events;  // event, entry's barcode or address
  ImportCounts run(const std::vector<Node> &backup) {
    return import_backup(
        table, pool, backup, [](Node &n) { n.prepared = true; },
        [this](ImportEvent e, const Node &n, const std::string &) {
          events.emplace_back(e, n.long_address.empty() ? n.addr : n.long_address);
        });
  }
  const Node *at(const std::string &addr) const {
    for (auto &n : table)
      if (n.addr == addr) return &n;
    return nullptr;
  }
};

static Node seen(const std::string &addr, const std::string &bc) {
  Node n;
  n.addr = addr;
  n.long_address = bc;
  n.seen_in_frame27 = true;
  return n;
}

static void test_import_restores_a_backup() {
  std::puts("import: a backup is restored as written when nothing has been seen");
  Import im;
  auto c = im.run({labelled("0002", "BC0000000000000A", "A1"), labelled("0003", "BC0000000000000B", "A2")});
  CHECK(c.remapped == 0 && c.parked == 0);
  CHECK(im.table.size() == 2);
  CHECK(im.at("0002")->cca_label == "A1" && im.at("0002")->prepared);
  CHECK(im.at("0003")->cca_label == "A2" && im.at("0003")->prepared);
  CHECK(im.events.empty());
}

static void test_import_follows_a_renumbering() {
  std::puts("import: an entry goes to the address its barcode has now");
  Import im;
  im.table = {seen("0005", "BC0000000000000A")};
  auto c = im.run({labelled("0002", "BC0000000000000A", "A1")});
  CHECK(c.remapped == 1);
  CHECK(im.at("0002") == nullptr);
  CHECK(im.at("0005") && im.at("0005")->cca_label == "A1" && im.at("0005")->seen_in_frame27);
  CHECK(im.events.size() == 1 && im.events[0].first == ImportEvent::moved);
}

static void test_import_holds_labels_for_an_unseen_panel() {
  std::puts("import: labels for an address another panel holds now wait for their panel");
  Harness h;
  h.table = {seen("0002", "BC0000000000000B")};
  Import im;
  im.table = h.table;
  auto c = im.run({labelled("0002", "BC0000000000000A", "A1"), labelled("0009", "BC0000000000000B", "B1")});
  CHECK(c.parked == 1 && c.remapped == 1);
  CHECK(im.at("0002") && im.at("0002")->cca_label == "B1");  // B is confirmed at 0002
  CHECK(im.pool.size() == 1 && im.pool[0].cca_label == "A1");
  // A's Frame 27 arrives at its new address and picks its labels up.
  h.table = im.table;
  h.pool = im.pool;
  h.feed("0007", "BC0000000000000A");
  CHECK(h.at("0007") && h.at("0007")->cca_label == "A1");
  CHECK(h.pool.empty());
}

static void test_import_confirmed_entry_wins_an_address() {
  std::puts("import: a confirmed entry keeps its address over a stale one listed first");
  Import im;
  im.table = {seen("0002", "BC0000000000000B")};
  Node stale = labelled("0002", "", "old");
  auto c = im.run({stale, labelled("0005", "BC0000000000000B", "B1")});
  CHECK(c.remapped == 1);
  CHECK(im.table.size() == 1);
  CHECK(im.at("0002")->cca_label == "B1");
  CHECK(im.events.size() == 2 && im.events[1].first == ImportEvent::duplicate_address);
}

static void test_import_merges_an_alias_and_skips_no_address() {
  std::puts("import: two entries for one barcode merge; an entry with no address is skipped");
  Import im;
  Node first = labelled("0002", "BC0000000000000A", "A1");
  first.cca_mppt_label.clear();
  Node alias = labelled("8002", "BC0000000000000A", "ignored");
  alias.cca_mppt_label = "MPPT 2";
  alias.sensor_index = 3;
  Node nowhere = labelled("", "BC0000000000000C", "C1");
  im.run({first, alias, nowhere});
  CHECK(im.table.size() == 1);
  CHECK(im.at("0002")->cca_label == "A1");
  CHECK(im.at("0002")->cca_mppt_label == "MPPT 2");
  CHECK(im.at("0002")->sensor_index == 3);
  CHECK(im.events.size() == 2 && im.events[0].first == ImportEvent::merged &&
        im.events[1].first == ImportEvent::no_address);
}

int main() {
  test_unchanged_frame_is_a_no_op();
  test_two_panels_swap_addresses();
  test_rotation_split_across_frames();
  test_full_shuffle_of_thirty_panels();
  test_commissioning_alias_is_merged();
  test_first_barcode_keeps_imported_labels();
  test_table_full();
  test_import_restores_a_backup();
  test_import_follows_a_renumbering();
  test_import_holds_labels_for_an_unseen_panel();
  test_import_confirmed_entry_wins_an_address();
  test_import_merges_an_alias_and_skips_no_address();
  std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
