// trials_extract: checks an Xbox Live Arcade package and writes its files to a folder, with the launcher's STFS reader.
// Used by build-linux.sh for the code generation, and for putting a game on an Android device.
//   trials_extract <package> --title-id          prints the package's title ID
//   trials_extract <package> <folder>            extracts it, as the launcher installs a game

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>

#include "stfs_package.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <package> --title-id | <folder>\n", argv[0]);
    return 2;
  }
  stfs::StfsPackage package;
  std::string error;
  if (!package.Open(argv[1], &error)) {
    std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
    return 1;
  }
  if (std::strcmp(argv[2], "--title-id") == 0) {
    std::printf("%08X\n", package.title_id());
    return 0;
  }
  uint64_t last = 0;
  const uint64_t total = package.total_file_bytes();
  const bool done = package.ExtractAll(
      argv[2], true,
      [&](const stfs::StfsEntry&, uint64_t written, uint64_t) {
        if (written - last >= (uint64_t(64) << 20)) {
          last = written;
          std::fprintf(stderr, "\r%" PRIu64 " / %" PRIu64 " MB", written >> 20, total >> 20);
        }
        return true;
      },
      &error);
  std::fprintf(stderr, "\n");
  if (!done) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  return 0;
}
