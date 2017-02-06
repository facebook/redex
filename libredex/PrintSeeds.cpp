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

template <class Container>
void print_method_seeds(std::ostream& output,
                        const ProguardMap& pg_map,
                        const std::string& class_name,
                        const Container& methods,
                        const bool allowshrinking_filter,
                        const bool allowobfuscation_filter) {

  for (DexMethod* method : methods) {
    if (keep(method) ||
      (allowshrinking_filter && !allowshrinking(method)) ||
      (allowobfuscation_filter && !allowobfuscation(method))
    ) {
      return;
    }
    redex::print_method(output, pg_map, class_name, method);
  }
}

template <class Container>
void print_field_seeds(std::ostream& output,
                       const ProguardMap& pg_map,
                       const std::string& class_name,
                       const Container& fields,
                       const bool allowshrinking_filter,
                       const bool allowobfuscation_filter) {
  for (DexField* field : fields) {
    if (!keep(field) ||
      (allowshrinking_filter && !allowshrinking(field)) ||
      (allowobfuscation_filter && !allowobfuscation(field))
    ) {
      return;
    }
    redex::print_field(output, pg_map, class_name, field);
  };
}

void show_class(std::ostream& output,
                const DexClass* cls,
                const std::string& name,
                const bool allowshrinking_filter,
                const bool allowobfuscation_filter) {
  if (allowshrinking_filter) {
    if (allowshrinking(cls)) {
      output << name << std::endl;
    }
    return;
  }
  if (allowobfuscation_filter) {
    if (allowobfuscation(cls)) {
      output << name << std::endl;
    }
    return;
  }
  output << name << std::endl;
}

// Print out the seeds computed in classes by Redex to the specified ostream.
// The ProGuard map is used to help deobfuscate type descriptors.
void redex::print_seeds(std::ostream& output,
                        const ProguardMap& pg_map,
                        const Scope& classes,
                        const bool allowshrinking_filter,
                        const bool allowobfuscation_filter) {
  for (const auto& cls : classes) {
    auto deob = cls->get_deobfuscated_name();
    if (deob.empty()) {
      std::cerr << "WARNING: this class has no deobu name: "
                << cls->get_name()->c_str() << std::endl;
      deob = cls->get_name()->c_str();
    }
    std::string name = redex::dexdump_name_to_dot_name(deob);
    if (keep(cls)) {
      show_class(
          output, cls, name, allowshrinking_filter, allowobfuscation_filter);
    }
    print_field_seeds(output,
                      pg_map,
                      name,
                      cls->get_ifields(),
                      allowshrinking_filter,
                      allowobfuscation_filter);
    print_field_seeds(output,
                      pg_map,
                      name,
                      cls->get_sfields(),
                      allowshrinking_filter,
                      allowobfuscation_filter);
    print_method_seeds(output,
                       pg_map,
                       name,
                       cls->get_dmethods(),
                       allowshrinking_filter,
                       allowobfuscation_filter);
    print_method_seeds(output,
                       pg_map,
                       name,
                       cls->get_vmethods(),
                       allowshrinking_filter,
                       allowobfuscation_filter);
  }
}
