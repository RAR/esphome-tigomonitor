#include "tls.h"

#include "log.h"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cstdio>
#include <sys/stat.h>

namespace tigod {

struct TlsCerts::Pair {
  X509 *leaf = nullptr;
  STACK_OF(X509) *chain = nullptr;  // the intermediates after the leaf
  EVP_PKEY *key = nullptr;
  int64_t not_after = 0;
  ~Pair() {
    X509_free(leaf);
    sk_X509_pop_free(chain, X509_free);
    EVP_PKEY_free(key);
  }
};

// Let's Encrypt's fullchain.pem is the certificate then its intermediate;
// browsers need both, so everything after the first is sent as the chain.
static std::string load_pair(const std::string &cert_path, const std::string &key_path,
                             std::shared_ptr<TlsCerts::Pair> &out) {
  ERR_clear_error();
  auto p = std::make_shared<TlsCerts::Pair>();
  FILE *f = fopen(cert_path.c_str(), "r");
  if (!f) return "cannot read " + cert_path;
  p->leaf = PEM_read_X509(f, nullptr, nullptr, nullptr);
  if (p->leaf) {
    p->chain = sk_X509_new_null();
    while (X509 *c = PEM_read_X509(f, nullptr, nullptr, nullptr)) sk_X509_push(p->chain, c);
    ERR_clear_error();  // the end of the file reads as an error
  }
  fclose(f);
  if (!p->leaf) return cert_path + " holds no PEM certificate";
  f = fopen(key_path.c_str(), "r");
  if (!f) return "cannot read " + key_path;
  p->key = PEM_read_PrivateKey(f, nullptr, nullptr, nullptr);
  fclose(f);
  if (!p->key) return key_path + " holds no unencrypted PEM private key";
  if (X509_check_private_key(p->leaf, p->key) != 1) {
    ERR_clear_error();
    return "the key in " + key_path + " is not the one for the certificate in " + cert_path;
  }
  struct tm tm {};
  if (ASN1_TIME_to_tm(X509_get0_notAfter(p->leaf), &tm) == 1) p->not_after = timegm(&tm);
  out = std::move(p);
  return "";
}

static struct timespec mtime_of(const std::string &path) {
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) return {};
  return st.st_mtim;
}

static bool same_time(const struct timespec &a, const struct timespec &b) {
  return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

TlsCerts::TlsCerts(std::string cert_path, std::string key_path)
    : cert_path_(std::move(cert_path)), key_path_(std::move(key_path)) {}

TlsCerts::~TlsCerts() = default;

std::string TlsCerts::load() {
  cert_mtime_ = mtime_of(cert_path_);
  key_mtime_ = mtime_of(key_path_);
  std::shared_ptr<Pair> p;
  std::string err = load_pair(cert_path_, key_path_, p);
  if (!err.empty()) return err;
  std::lock_guard<std::mutex> lock(mu_);
  current_ = std::move(p);
  return "";
}

void TlsCerts::reload_if_changed() {
  // A renewal writes two files, usually not at once: a check between them
  // finds a mismatched pair, keeps the old one, and tries again when the
  // second file lands and changes its time.
  if (same_time(mtime_of(cert_path_), cert_mtime_) && same_time(mtime_of(key_path_), key_mtime_)) return;
  std::string err = load();
  if (err.empty()) {
    time_t t = static_cast<time_t>(not_after());
    char when[32];
    strftime(when, sizeof when, "%Y-%m-%d", gmtime(&t));
    LOGI("web: loaded the renewed certificate, valid until %s", when);
  } else {
    LOGE("web: the certificate files changed but did not load (%s); still serving the previous one", err.c_str());
  }
}

int64_t TlsCerts::not_after() const {
  std::lock_guard<std::mutex> lock(mu_);
  return current_ ? current_->not_after : 0;
}

bool TlsCerts::install(SSL_CTX &ctx) {
  // As httplib's own SSLServer does for a certificate given as files.
  SSL_CTX_set_options(&ctx, SSL_OP_NO_COMPRESSION | SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);
  SSL_CTX_set_min_proto_version(&ctx, TLS1_2_VERSION);
  // Chosen per connection rather than set on the context, which httplib
  // reads from its own threads; a reload only swaps current_.
  SSL_CTX_set_cert_cb(
      &ctx,
      [](SSL *ssl, void *arg) -> int {
        auto *self = static_cast<TlsCerts *>(arg);
        std::shared_ptr<const Pair> p;
        {
          std::lock_guard<std::mutex> lock(self->mu_);
          p = self->current_;
        }
        if (!p) return 0;
        // Takes its own references, so the pair can be replaced mid-handshake.
        return SSL_use_cert_and_key(ssl, p->leaf, p->key, p->chain, 1);
      },
      this);
  return true;
}

std::string TlsCerts::check(const std::string &cert_path, const std::string &key_path) {
  std::shared_ptr<Pair> p;
  return load_pair(cert_path, key_path, p);
}

}  // namespace tigod
