/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <unordered_map>
#include <unordered_set>

#include "DexClass.h"
#include "Pass.h"

namespace reachable_objects {

enum class ReachableObjectType {
  ANNO,
  CLASS,
  FIELD,
  METHOD,
  SEED,
};

/**
 * Represents an object (class, method, or field) that's considered reachable
 * by this pass.
 *
 * Used for logging what retains what so that we can see what things which
 * should be removed aren't being removed.
 */
struct ReachableObject {
  ReachableObjectType type;
  const DexAnnotation* anno{nullptr};
  const DexClass* cls{nullptr};
  const DexFieldRef* field{nullptr};
  const DexMethodRef* method{nullptr};

  explicit ReachableObject(const DexAnnotation* anno)
      : type{ReachableObjectType::ANNO}, anno{anno} {}
  explicit ReachableObject(const DexClass* cls)
      : type{ReachableObjectType::CLASS}, cls{cls} {}
  explicit ReachableObject(const DexMethodRef* method)
      : type{ReachableObjectType::METHOD}, method{method} {}
  explicit ReachableObject(const DexFieldRef* field)
      : type{ReachableObjectType::FIELD}, field{field} {}
  explicit ReachableObject() : type{ReachableObjectType::SEED} {}

  std::string str() const {
    switch (type) {
    case ReachableObjectType::ANNO:
      return show_deobfuscated(anno);
    case ReachableObjectType::CLASS:
      return show_deobfuscated(cls);
    case ReachableObjectType::FIELD:
      return show_deobfuscated(field);
    case ReachableObjectType::METHOD:
      return show_deobfuscated(method);
    case ReachableObjectType::SEED:
      return std::string("<SEED>");
    }
  }

  std::string type_str() const {
    switch (type) {
    case ReachableObjectType::ANNO:
      return "ANNO";
    case ReachableObjectType::CLASS:
      return "CLASS";
    case ReachableObjectType::FIELD:
      return "FIELD";
    case ReachableObjectType::METHOD:
      return "METHOD";
    case ReachableObjectType::SEED:
      return "SEED";
    }
  }

  std::string state_str() const {
    switch (type) {
    case ReachableObjectType::ANNO:
      return "Annotation";
    case ReachableObjectType::CLASS:
      return cls->rstate.str();
    case ReachableObjectType::FIELD:
      if (field->is_def()) {
        return static_cast<const DexField*>(field)->rstate.str();
      } else {
        return "DexFieldRef";
      }
    case ReachableObjectType::METHOD:
      if (method->is_def()) {
        return static_cast<const DexMethod*>(method)->rstate.str();
      } else {
        return "DexFieldRef";
      }
    case ReachableObjectType::SEED:
      return "Seed";
    }
  }
};

struct ReachableObjectHash {
  std::size_t operator()(const ReachableObject& obj) const {
    switch (obj.type) {
    case ReachableObjectType::ANNO:
      return std::hash<const DexAnnotation*>{}(obj.anno);
    case ReachableObjectType::CLASS:
      return std::hash<const DexClass*>{}(obj.cls);
    case ReachableObjectType::FIELD:
      return std::hash<const DexFieldRef*>{}(obj.field);
    case ReachableObjectType::METHOD:
      return std::hash<const DexMethodRef*>{}(obj.method);
    case ReachableObjectType::SEED:
      return 0;
    }
  }
};

struct ReachableObjectEq {
  bool operator()(const ReachableObject& lhs,
                  const ReachableObject& rhs) const {
    if (lhs.type != rhs.type) {
      return false;
    }
    switch (lhs.type) {
    case ReachableObjectType::ANNO:
      return lhs.anno == rhs.anno;
    case ReachableObjectType::CLASS:
      return lhs.cls == rhs.cls;
    case ReachableObjectType::FIELD:
      return lhs.field == rhs.field;
    case ReachableObjectType::METHOD:
      return lhs.method == rhs.method;
    case ReachableObjectType::SEED:
      return true;
    }
  }
};

using ReachableObjectSet =
    std::unordered_set<ReachableObject, ReachableObjectHash, ReachableObjectEq>;
using ReachableObjectGraph = std::unordered_map<ReachableObject,
                                                ReachableObjectSet,
                                                ReachableObjectHash,
                                                ReachableObjectEq>;

} // namespace

struct ReachableObjects {
  std::unordered_set<const DexClass*> marked_classes;
  std::unordered_set<const DexFieldRef*> marked_fields;
  std::unordered_set<const DexMethodRef*> marked_methods;
  reachable_objects::ReachableObjectGraph retainers_of;
};

ReachableObjects compute_reachable_objects(
    DexStoresVector& stores,
    const std::unordered_set<const DexType*>& ignore_string_literals,
    const std::unordered_set<const DexType*>& ignore_string_literal_annos,
    const std::unordered_set<const DexType*>& ignore_system_annos,
    int* num_ignore_check_strings,
    bool record_reachability = false);

// Dump reachability information to TRACE(REACH_DUMP, 5).
void dump_reachability(DexStoresVector& stores,
                       reachable_objects::ReachableObjectGraph& retainers_of,
                       const std::string& dump_tag);

void dump_reachability_graph(
    DexStoresVector& stores,
    reachable_objects::ReachableObjectGraph& retainers_of,
    const std::string& dump_tag,
    std::ostream& os);
