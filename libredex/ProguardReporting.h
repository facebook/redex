/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexClass.h"
#include "DexUtil.h"
#include "ProguardMap.h"
#include <iostream>

namespace redex {

std::string dexdump_name_to_dot_name(const std::string& dexdump_name);

void print_methods(std::ostream& output,
                   const ProguardMap& pg_map,
                   const std::string& class_name,
                   const std::list<DexMethod*>& methods);

void print_method(std::ostream& output,
                  const ProguardMap& pg_map,
                  const std::string& class_name,
                  const DexMethod* methods);

void print_fields(std::ostream& output,
                  const ProguardMap& pg_map,
                  const std::string& class_name,
                  const std::list<DexField*>& fields);

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

void alert_seeds(std::ostream& output, const Scope& classes);
}
