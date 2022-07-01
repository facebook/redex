/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReadMaybeMapped.h"

#include <boost/iostreams/device/mapped_file.hpp>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef __linux__
#include <sys/mman.h> // For madvise
#endif

#include "utils/Compat.h" // TEMP_FAILURE_RETRY, if necessary.

#include "Debug.h"
#include "Macros.h"

namespace redex {
namespace {

std::string strerror_str(int no) { return std::string(strerror(no)); };

// Implementation based on posix read. Avoids heap allocation when data
// is small enough.
template <size_t kDataSize>
struct ReadFileContents final {
  size_t size;
  std::unique_ptr<char[]> content;
  char inline_data[kDataSize];

  explicit ReadFileContents(const std::string& file) {
    int fd = open(file.c_str(), O_RDONLY | O_BINARY);
    if (fd < 0) {
      throw std::runtime_error(std::string("Failed to open ") + file + ": " +
                               strerror_str(errno));
    }
    struct stat st = {};
    if (fstat(fd, &st) == -1) {
      auto saved_errno = errno;
      close(fd);
      throw std::runtime_error(std::string("Failed to get file length of ") +
                               file + ": " + strerror_str(saved_errno));
    }
    read_data(file, fd, static_cast<size_t>(st.st_size));
  }
  ReadFileContents(const std::string& file, int fd, size_t size) {
    read_data(file, fd, size);
  }

  void read_data(const std::string& file, int fd, size_t size_in) {
    size = size_in;
    if (size == 0) {
      close(fd);
      return;
    }
    char* data;
    if (size > kDataSize) {
      content = std::make_unique<char[]>(size);
      data = content.get();
    } else {
      data = inline_data;
    }

    // Now read.
    size_t remaining = size;
    while (remaining > 0) {
      ssize_t n = TEMP_FAILURE_RETRY(read(fd, data, remaining));
      if (n <= 0) {
        auto saved_errno = errno;
        close(fd);
        throw std::runtime_error(std::string("Failed reading ") + file + ": " +
                                 strerror_str(saved_errno));
      }
      data += n;
      remaining -= n;
    }
    close(fd);
  }

  const char* get_content() const {
    return size > kDataSize ? content.get() : inline_data;
  }
  size_t get_content_size() const { return size; }
};
using PageSizeReadFileContents = ReadFileContents<4080>;

static_assert(sizeof(PageSizeReadFileContents) == 4096 || sizeof(void*) != 8,
              "Unexpected size");

// Mmap: just map file contents and wrap the mapping, avoiding allocation
// and explicit I/O.
//
// Note: using boost for Windows compat.
template <int kMadviseFlags>
struct MmapFileContents final {
  boost::iostreams::mapped_file_source mapped_file{};

  explicit MmapFileContents(const std::string& file) {
    mapped_file.open(file);
    if (!mapped_file.is_open()) {
      throw std::runtime_error(std::string("Could not open ") + file);
    }
#ifdef __linux__
    if (mapped_file.size() > 0) {
      madvise(const_cast<char*>(mapped_file.data()),
              mapped_file.size(),
              kMadviseFlags);
    }
#endif
  }

  const char* get_content() const { return mapped_file.data(); }
  size_t get_content_size() const { return mapped_file.size(); }
};

} // namespace

// Mmaps may not amortize for small files. Split between `read` and `mmap`.
void read_file_with_contents(const std::string& file,
                             const std::function<void(const char*, size_t)>& fn,
                             size_t threshold) {
  int fd = open(file.c_str(), O_RDONLY | O_BINARY);
  if (fd < 0) {
    throw std::runtime_error(std::string("Failed to open ") + file + ": " +
                             strerror_str(errno));
  }
  struct stat st = {};
  if (fstat(fd, &st) == -1) {
    auto saved_errno = errno;
    close(fd);
    throw std::runtime_error(std::string("Failed to get file length of ") +
                             file + ": " + strerror_str(saved_errno));
  }
  size_t size = static_cast<size_t>(st.st_size);

  if (size <= threshold) {
    auto content = PageSizeReadFileContents(file, fd, size);
    fn(content.get_content(), content.get_content_size());
  } else {
    close(fd);
    constexpr int kAdvFlags =
#ifdef __linux__
        MADV_SEQUENTIAL | MADV_WILLNEED;
#else
        0;
#endif
    auto content = MmapFileContents<kAdvFlags>(file);
    fn(content.get_content(), content.get_content_size());
  }
}

} // namespace redex
