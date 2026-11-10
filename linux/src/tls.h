#pragma once

// The web UI's certificate and key, for HTTPS. They come from outside the
// daemon (certbot or lego, say, renewing a Let's Encrypt certificate), so
// the files are watched: a renewal is picked up by the next connection, with
// no restart. Only a pair that loads, and whose key matches, replaces the
// one in use.

#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>

typedef struct ssl_ctx_st SSL_CTX;

namespace tigod {

class TlsCerts {
 public:
  TlsCerts(std::string cert_path, std::string key_path);
  ~TlsCerts();

  // Loads the pair. Returns "" on success, or what is wrong with it.
  std::string load();
  // Reloads if either file has changed since the last attempt; a pair that
  // fails to load is logged and the one in use kept.
  void reload_if_changed();
  // Makes ctx serve whatever pair is current on each new connection.
  bool install(SSL_CTX &ctx);
  // The current certificate's expiry, in Unix seconds; 0 if none is loaded.
  int64_t not_after() const;

  // Loads a pair just to check it, as a settings save does.
  static std::string check(const std::string &cert_path, const std::string &key_path);

  struct Pair;

 private:
  std::string cert_path_, key_path_;
  struct timespec cert_mtime_ {}, key_mtime_ {};
  mutable std::mutex mu_;
  std::shared_ptr<const Pair> current_;
};

}  // namespace tigod
