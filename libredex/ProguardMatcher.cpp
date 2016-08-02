/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <string>

#include "ProguardMap.h"
#include "ProguardMatcher.h"
#include "keeprules.h"

namespace redex {

std::string dextype_from_dotname(std::string dotname) {
  std::string buf;
  buf.reserve(dotname.size() + 2);
  buf += 'L';
  buf += dotname;
  buf += ';';
  std::replace(buf.begin(), buf.end(), '.', '/');
  return buf;
}

void process_proguard_rules(const ProguardConfiguration& pg_config,
                            ProguardMap* proguard_map,
                            Scope& classes) {
  for (const auto& cls : classes) {
    auto cname = cls->get_type()->get_name()->c_str();
    auto cls_len = strlen(cname);
    TRACE(PGR, 8, "Examining class %s\n", cname);
    for (const auto& k : pg_config.keep_rules) {
		  auto keep_name = dextype_from_dotname(k.class_spec.className);
      std::string translated_keep_name = proguard_map->translate_class(keep_name);
      TRACE(PGR,
            8,
            "==> Checking against keep rule for %s (%s)\n",
            keep_name.c_str(), translated_keep_name.c_str());
      if (type_matches(translated_keep_name.c_str(),
                       cname,
                       translated_keep_name.size(),
                       cls_len)) {
				TRACE(PGR, 8, "Setting keep for %s\n", cname);
        cls->rstate.set_keep();
      }
    }
  }
}

} // namespace redex
