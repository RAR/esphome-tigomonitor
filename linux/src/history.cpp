#include "history.h"

#include "log.h"

#include <sqlite3.h>

#include <ctime>

namespace tigod {

History::History(std::string path) : path_(std::move(path)) {}

History::~History() {
  if (db_) sqlite3_close(db_);
}

static bool exec(sqlite3 *db, const char *sql) {
  char *err = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &err) == SQLITE_OK) return true;
  LOGE("history: %s", err ? err : "unknown error");
  sqlite3_free(err);
  return false;
}

bool History::open() {
  std::lock_guard<std::mutex> lock(mu_);
  if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
    error_ = std::string("cannot open ") + path_ + ": " + sqlite3_errmsg(db_);
    LOGE("history: %s", error_.c_str());
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }
  sqlite3_busy_timeout(db_, 5000);
  // WAL with synchronous=NORMAL: a power cut loses at most the last snapshot,
  // never the database, and each snapshot is one small sequential write.
  bool ok = exec(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;") &&
            exec(db_,
                 "CREATE TABLE IF NOT EXISTS system (t INTEGER PRIMARY KEY, p REAL NOT NULL, e REAL NOT NULL);"
                 "CREATE TABLE IF NOT EXISTS panel (slot INTEGER NOT NULL, t INTEGER NOT NULL, p REAL NOT NULL,"
                 "  PRIMARY KEY (slot, t)) WITHOUT ROWID;"
                 "CREATE TABLE IF NOT EXISTS slots (slot INTEGER PRIMARY KEY, barcode TEXT NOT NULL UNIQUE);");
  if (!ok) {
    error_ = "cannot create the history tables";
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }
  LOGI("history: %s", path_.c_str());
  return true;
}

int History::slot_for_locked_(const std::string &barcode) {
  sqlite3_stmt *st = nullptr;
  int slot = -1;
  sqlite3_prepare_v2(db_, "SELECT slot FROM slots WHERE barcode = ?", -1, &st, nullptr);
  sqlite3_bind_text(st, 1, barcode.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) slot = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  if (slot >= 0) return slot;
  sqlite3_prepare_v2(db_, "INSERT INTO slots (slot, barcode) VALUES ((SELECT COALESCE(MAX(slot) + 1, 0) FROM slots), ?)",
                     -1, &st, nullptr);
  sqlite3_bind_text(st, 1, barcode.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_DONE) slot = static_cast<int>(sqlite3_last_insert_rowid(db_));
  sqlite3_finalize(st);
  return slot;
}

void History::record(int64_t t, double total_power, double period_kwh,
                     const std::vector<std::pair<std::string, double>> &panels) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) return;
  exec(db_, "BEGIN");
  sqlite3_stmt *st = nullptr;
  sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO system (t, p, e) VALUES (?, ?, ?)", -1, &st, nullptr);
  sqlite3_bind_int64(st, 1, t);
  sqlite3_bind_double(st, 2, total_power);
  sqlite3_bind_double(st, 3, period_kwh);
  sqlite3_step(st);
  sqlite3_finalize(st);
  sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO panel (slot, t, p) VALUES (?, ?, ?)", -1, &st, nullptr);
  for (const auto &kv : panels) {
    int slot = slot_for_locked_(kv.first);
    if (slot < 0) continue;
    sqlite3_reset(st);
    sqlite3_bind_int(st, 1, slot);
    sqlite3_bind_int64(st, 2, t);
    sqlite3_bind_double(st, 3, kv.second);
    sqlite3_step(st);
  }
  sqlite3_finalize(st);
  exec(db_, "COMMIT");
}

std::vector<HistoryRow> History::query_locked_(const char *sql, int64_t start, int64_t end, int64_t bucket_s,
                                               int slot) {
  std::vector<HistoryRow> rows;
  if (!db_) return rows;
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    LOGE("history: %s", sqlite3_errmsg(db_));
    return rows;
  }
  int i = 1;
  sqlite3_bind_int64(st, i++, bucket_s);
  sqlite3_bind_int64(st, i++, bucket_s);
  if (slot >= 0) sqlite3_bind_int(st, i++, slot);
  sqlite3_bind_int64(st, i++, start);
  sqlite3_bind_int64(st, i++, end);
  while (sqlite3_step(st) == SQLITE_ROW) {
    HistoryRow r;
    r.t = sqlite3_column_int64(st, 0);
    r.p = sqlite3_column_double(st, 1);
    if (sqlite3_column_count(st) > 2) r.e = sqlite3_column_double(st, 2);
    rows.push_back(r);
  }
  sqlite3_finalize(st);
  return rows;
}

std::vector<HistoryRow> History::system(int64_t start, int64_t end, int64_t bucket_s) {
  std::lock_guard<std::mutex> lock(mu_);
  return query_locked_("SELECT (t / ?) * ?, AVG(p), SUM(e) FROM system WHERE t BETWEEN ? AND ? "
                       "GROUP BY t / ?1 ORDER BY 1",
                       start, end, bucket_s, -1);
}

std::vector<HistoryRow> History::panel(int slot, int64_t start, int64_t end, int64_t bucket_s) {
  std::lock_guard<std::mutex> lock(mu_);
  return query_locked_("SELECT (t / ?) * ?, AVG(p) FROM panel WHERE slot = ? AND t BETWEEN ? AND ? "
                       "GROUP BY t / ?1 ORDER BY 1",
                       start, end, bucket_s, slot);
}

std::vector<std::pair<std::string, int>> History::slots() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::pair<std::string, int>> out;
  if (!db_) return out;
  sqlite3_stmt *st = nullptr;
  sqlite3_prepare_v2(db_, "SELECT barcode, slot FROM slots ORDER BY slot", -1, &st, nullptr);
  while (sqlite3_step(st) == SQLITE_ROW)
    out.emplace_back(reinterpret_cast<const char *>(sqlite3_column_text(st, 0)), sqlite3_column_int(st, 1));
  sqlite3_finalize(st);
  return out;
}

void History::prune(int keep_days) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) return;
  int64_t cutoff = static_cast<int64_t>(time(nullptr)) - static_cast<int64_t>(keep_days) * 86400;
  sqlite3_stmt *st = nullptr;
  for (const char *sql : {"DELETE FROM system WHERE t < ?", "DELETE FROM panel WHERE t < ?"}) {
    sqlite3_prepare_v2(db_, sql, -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, cutoff);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
}

}  // namespace tigod
