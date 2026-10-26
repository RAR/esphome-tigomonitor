#pragma once

#include <string>
#include <sys/types.h>

namespace tigod {

// Write via a temp file + fsync + rename, so a power cut leaves either the old
// file or the new one — never half of each. SD cards on a Pi lose power often.
bool write_file_atomic(const std::string &path, const std::string &data, mode_t mode = 0644);

}  // namespace tigod
