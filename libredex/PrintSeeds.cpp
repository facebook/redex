/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PrintSeeds.h"

#include "DexUtil.h"
#include "ProguardConfiguration.h"
#include "ProguardMap.h"
#include "ProguardReporting.h"
#include "ReachableClasses.h"
#include "ReferencedState.h"

using namespace keep_rules;

template <class Container>
void print_method_seeds(std::ostream& output,
                        const ProguardMap& pg_map,
                        const std::string& class_name,
                        const Container& methods,
                        const bool allowshrinking_filter,
                        const bool allowobfuscation_filter) {

  for (DexMethod* method : methods) {
    if (impl::KeepState::has_keep(method) ||
        (allowshrinking_filter && !impl::KeepState::allowshrinking(method)) ||
        (allowobfuscation_filter &&
         !impl::KeepState::allowobfuscation(method))) {
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
    if (!impl::KeepState::has_keep(field) ||
        (allowshrinking_filter && !impl::KeepState::allowshrinking(field)) ||
        (allowobfuscation_filter &&
         !impl::KeepState::allowobfuscation(field))) {
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
    if (impl::KeepState::allowshrinking(cls)) {
      output << name << std::endl;
    }
    return;
  }
  if (allowobfuscation_filter) {
    if (impl::KeepState::allowobfuscation(cls)) {
      output << name << std::endl;
    }
    return;
  }
  output << name << std::endl;
}

// Print out the seeds computed in classes by Redex to the specified ostream.
// The ProGuard map is used to help deobfuscate type descriptors.
void keep_rules::print_seeds(std::ostream& output,
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
    std::string name = java_names::internal_to_external(deob);
    if (impl::KeepState::has_keep(cls)) {
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
