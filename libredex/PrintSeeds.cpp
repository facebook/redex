/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "PrintSeeds.h"
#include "ProguardReporting.h"
#include "ReachableClasses.h"
#include "ReferencedState.h"

void print_method_seeds(std::ostream& output,
                        const ProguardMap& pg_map,
                        const std::string& class_name,
                        const std::list<DexMethod*>& methods) {
  for (const auto& method : methods) {
    if (keep(method)) {
      redex::print_method(output, pg_map, class_name, method);
    }
  }
}

void print_field_seeds(std::ostream& output,
                       const ProguardMap& pg_map,
                       const std::string& class_name,
                       const std::list<DexField*>& fields) {
  for (const auto& field : fields) {
    if (keep(field)) {
      redex::print_field(output, pg_map, class_name, field);
    }
  }
}

// Print out the seeds computed in classes by Redex to the specified ostream.
// The ProGuard map is used to help deobfuscate type descriptors.
void redex::print_seeds(std::ostream& output,
                        const ProguardMap& pg_map,
                        const Scope& classes) {
  for (const auto& cls : classes) {
    if (keep(cls) || keepclassmembers(cls)) {
      auto deob = cls->get_deobfuscated_name();
      if (deob.empty()) {
        std::cerr << "WARNING: this class has no deobu name: "
                  << cls->get_name()->c_str() << std::endl;
        deob = cls->get_name()->c_str();
      }
      std::string name = redex::dexdump_name_to_dot_name(deob);
      if (keep(cls)) {
        output << name << std::endl;
      }
      print_field_seeds(output, pg_map, name, cls->get_ifields());
      print_field_seeds(output, pg_map, name, cls->get_sfields());
      print_method_seeds(output, pg_map, name, cls->get_dmethods());
      print_method_seeds(output, pg_map, name, cls->get_vmethods());
    }
  }
}
