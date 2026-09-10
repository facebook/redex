/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>

#include "DexClass.h"
#include "DexUtil.h"
#include "ProguardMap.h"

namespace keep_rules {
struct ProguardRuleRecorder;
struct ProguardConfiguration;

namespace proguard_parser {
struct Diagnostics;
struct Stats;
} // namespace proguard_parser
} // namespace keep_rules

namespace redex {

template <class Container>
void print_methods(std::ostream& output,
                   const ProguardMap& pg_map,
                   const std::string& class_name,
                   const Container& methods);

void print_method(std::ostream& output,
                  const ProguardMap& pg_map,
                  const std::string& class_name,
                  const DexMethod* methods);

template <class Container>
void print_fields(std::ostream& output,
                  const ProguardMap& pg_map,
                  const std::string& class_name,
                  const Container& fields);

void print_field(std::ostream& output,
                 const ProguardMap& pg_map,
                 const std::string& class_name,
                 const DexField* field);

void print_class(std::ostream& output,
                 const ProguardMap& pg_map,
                 const DexClass* cls);

void print_classes(std::ostream& output,
                   const ProguardMap& pg_map,
                   const Scope& classes);

void dump_proguard_lens_json(
    const std::string& path,
    const DexStoresVector& stores,
    const char* phase,
    const keep_rules::ProguardConfiguration* pg_config = nullptr,
    const keep_rules::ProguardRuleRecorder* recorder = nullptr,
    const keep_rules::proguard_parser::Stats* parser_stats = nullptr,
    const keep_rules::proguard_parser::Diagnostics* diagnostics = nullptr,
    size_t blocklisted_rules = 0);
} // namespace redex
