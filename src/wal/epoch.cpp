#include "wal/epoch.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace rv::wal {

uint32_t random_epoch() noexcept {
  uint32_t val = 0;
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    const ssize_t n = ::read(fd, &val, sizeof(val));
    ::close(fd);
    if (n == static_cast<ssize_t>(sizeof(val)) && val != 0) {
      return val;
    }
  }
  // Fallback se /dev/urandom falhar
  static thread_local uint64_t seed = 0x853c49e6748fea9bULL;
  seed ^= seed >> 12;
  seed ^= seed << 25;
  seed ^= seed >> 27;
  val = static_cast<uint32_t>(seed * 0x2545F4914F6CDD1DULL);
  return val != 0 ? val : kDefaultEpoch;
}

}  // namespace rv::wal
