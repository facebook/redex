// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include <string>
#include <vector>

class ApkManager {
 public:
   ApkManager(std::string&& apk_dir)
      : m_apk_dir(apk_dir) {
   }

   virtual ~ApkManager() {
     for (auto fd : m_files) {
       fclose(fd);
     }
   }

   FILE* new_asset_file(const char* filename);

 private:
   std::vector<FILE*> m_files;
   std::string m_apk_dir;
};
