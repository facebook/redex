/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <iostream>
#include <string>
#include <vector>

#include "DexClass.h"
#include "DexLoader.h"
#include "DexStore.h"
#include "RedexContext.h"

inline Scope scope_from_dex_files(const std::vector<std::string>& dex_files) {
  DexMetadata dm;
  dm.set_id("classes");
  DexStore root_store(dm);

  for (const std::string& dex_file : dex_files) {
    std::cout << "Loading " << dex_file << "...";
    root_store.add_classes(load_classes_from_dex(dex_file.c_str()));
    std::cout << "done." << std::endl;
  }

  std::vector<DexStore> stores;
  stores.emplace_back(std::move(root_store));
  DexStoreClassesIterator it(stores);
  return build_class_scope(it);
}
