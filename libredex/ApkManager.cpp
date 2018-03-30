// Copyright 2004-present Facebook. All Rights Reserved.

#include "ApkManager.h"

#include <boost/filesystem.hpp>
#include <iostream>
#include <sstream>

namespace {

bool check_directory(std::string& apkdir) {
  if (!boost::filesystem::is_directory(apkdir.c_str())) {
    std::cerr << "error: apkdir is not a writable directory: " << apkdir
              << std::endl;
    exit(EXIT_FAILURE);
  }
}

}

FILE* ApkManager::new_asset_file(const char* filename) {
  check_directory(m_apk_dir);
  std::ostringstream path;
  path << m_apk_dir << "/assets/";
  if (!boost::filesystem::is_directory(path.str().c_str())) {
    std::cerr << "error: assets dir is not a writable directory: " << path.str()
              << std::endl;
    exit(EXIT_FAILURE);
  }
  path << filename;

  FILE* fd = fopen(path.str().c_str(), "w");
  if (fd != nullptr) {
    m_files.emplace_back(fd);
  } else {
    perror("Error creating new asset file");
  }
  return fd;
}
