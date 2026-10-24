#include "fsutil.h"

#include "log.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace tigod {

bool write_file_atomic(const std::string &path, const std::string &data, mode_t mode) {
  std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) {
    LOGE("cannot write %s: %s", tmp.c_str(), strerror(errno));
    return false;
  }
  const char *p = data.data();
  size_t left = data.size();
  while (left > 0) {
    ssize_t n = ::write(fd, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      LOGE("write %s: %s", tmp.c_str(), strerror(errno));
      ::close(fd);
      ::unlink(tmp.c_str());
      return false;
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
  bool ok = ::fsync(fd) == 0;
  ::close(fd);
  if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
    LOGE("cannot replace %s: %s", path.c_str(), strerror(errno));
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace tigod
