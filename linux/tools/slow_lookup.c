// LD_PRELOAD shim for tests: a lookup of a name ending in ".slow" takes 3 s
// and fails, as an unreachable DNS server does; any other name is looked up
// as usual. Used by mqtt-lookup-e2e.sh.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
  size_t n = node ? strlen(node) : 0;
  if (n > 5 && strcmp(node + n - 5, ".slow") == 0) {
    sleep(3);
    return EAI_NONAME;
  }
  int (*real)(const char *, const char *, const struct addrinfo *, struct addrinfo **) =
      (int (*)(const char *, const char *, const struct addrinfo *, struct addrinfo **))dlsym(RTLD_NEXT, "getaddrinfo");
  return real(node, service, hints, res);
}
