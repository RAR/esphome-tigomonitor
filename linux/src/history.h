#pragma once

// Power history in SQLite, served in the firmware's /api/history/* shapes so
// the History page and the panel charts work unchanged.
//
// The ESP32 keeps fixed-size esp_tsdb rings and snapshots every 30 minutes
// to spare its flash. An SD card has room for years of 5-minute snapshots,
// so a query over a long range is averaged into at most MAX_POINTS buckets
// instead — the page then draws the same number of points the ESP32's would.

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct sqlite3;

namespace tigod {

struct HistoryRow {
  int64_t t;      // unix seconds, the bucket's start
  double p;       // W, average over the bucket
  double e = 0;   // kWh produced in the bucket (system rows only)
};

class History {
 public:
  explicit History(std::string path);
  ~History();
  bool open();
  bool ok() const { return db_ != nullptr; }
  const std::string &error() const { return error_; }

  // One snapshot: system power, energy since the last snapshot, and each
  // panel's output power keyed by barcode.
  void record(int64_t t, double total_power, double period_kwh,
              const std::vector<std::pair<std::string, double>> &panels);

  // Rows in [start, end], averaged into buckets of bucket_s seconds.
  std::vector<HistoryRow> system(int64_t start, int64_t end, int64_t bucket_s);
  std::vector<HistoryRow> panel(int slot, int64_t start, int64_t end, int64_t bucket_s);

  // Barcode -> slot, in the order panels were first recorded. Slots are
  // never reused, so a replaced panel keeps its own history.
  std::vector<std::pair<std::string, int>> slots();

  // Drops rows older than keep_days.
  void prune(int keep_days);

  static constexpr int64_t MAX_POINTS = 2000;

 private:
  int slot_for_locked_(const std::string &barcode);
  std::vector<HistoryRow> query_locked_(const char *sql, int64_t start, int64_t end, int64_t bucket_s, int slot);
  std::string path_, error_;
  sqlite3 *db_ = nullptr;
  std::mutex mu_;
};

}  // namespace tigod
