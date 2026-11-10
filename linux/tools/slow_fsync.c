// LD_PRELOAD shim for the e2e tests: every fsync takes SLOW_FSYNC_MS
// (default 900), as a slow SD card's can. Build:
//   cc -shared -fPIC -o slow_fsync.so linux/tools/slow_fsync.c -ldl
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <time.h>

int fsync(int fd) {
  static int (*real)(int);
  if (!real) real = (int (*)(int))dlsym(RTLD_NEXT, "fsync");
  const char *ms = getenv("SLOW_FSYNC_MS");
  long d = ms ? atol(ms) : 900;
  struct timespec ts = {d / 1000, (d % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  return real(fd);
}
