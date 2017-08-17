/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "CheckBreadcrumbs.h"

#include <algorithm>
#include <unordered_set>
#include <sstream>

#include "Walkers.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "DexUtil.h"
#include "Resolver.h"

namespace {

constexpr const char* METRIC_BAD_FIELDS = "bad_fields";
constexpr const char* METRIC_BAD_METHODS = "bad_methods";
constexpr const char* METRIC_BAD_TYPE_INSTRUCTIONS = "bad_type_instructions";
constexpr const char* METRIC_BAD_FIELD_INSTRUCTIONS = "bad_field_instructions";
constexpr const char* METRIC_BAD_METHOD_INSTRUCTIONS =
    "bad_method_instructions";

bool class_contains(const DexField* field) {
  const auto& cls = type_class(field->get_class());
  if (cls == nullptr) return true;
  for (const auto& cls_field : cls->get_ifields()) {
    if (field == cls_field) return true;
  }
  for (const auto& cls_field : cls->get_sfields()) {
    if (field == cls_field) return true;
  }
  return false;
}

bool class_contains(const DexMethod* method) {
  const auto& cls = type_class(method->get_class());
  if (cls == nullptr) return true;
  for (const auto& cls_meth : cls->get_vmethods()) {
    if (method == cls_meth) return true;
  }
  for (const auto& cls_meth : cls->get_dmethods()) {
    if (method == cls_meth) return true;
  }
  return false;
}

using Fields = std::vector<const DexField*>;
using Methods = std::vector<const DexMethod*>;
using Instructions = std::vector<const IRInstruction*>;
using MethodInsns = std::map<const DexMethod*, Instructions, dexmethods_comparator>;

/**
 * Performs 2 kind of verifications:
 * 1- no references should be to a DexClass that is "internal"
 * but not in scope (effectively deleted)
 * 2- if a field or method reference is a def the field or method
 * must exist on the class it is defined on
 * Those are 2 relatively common problems we introduce: leave references
 * to deleted types, methods or fields.
 */
class Breadcrumbs {
  const Scope& scope;
  std::unordered_set<const DexClass*> classes;
  std::map<const DexType*, Fields, dextypes_comparator> bad_fields;
  std::map<const DexType*, Methods, dextypes_comparator> bad_methods;
  std::map<const DexType*, MethodInsns, dextypes_comparator> bad_type_insns;
  std::map<const DexField*, MethodInsns, dexfields_comparator> bad_field_insns;
  std::map<const DexMethod*, MethodInsns, dexmethods_comparator> bad_meth_insns;

 public:

  explicit Breadcrumbs(const Scope& s) : scope(s) {
    classes.insert(scope.begin(), scope.end());
  }

  void check_breadcrumbs() {
    check_fields();
    check_methods();
    check_opcodes();
  }

  void report(bool report_only, PassManager& mgr) {
    size_t bad_fields_count = 0;
    size_t bad_methods_count = 0;
    size_t bad_type_insns_count = 0;
    size_t bad_field_insns_count = 0;
    size_t bad_meths_insns_count = 0;
    if (bad_fields.size() > 0 ||
        bad_methods.size() > 0 ||
        bad_type_insns.size() > 0 ||
        bad_field_insns.size() > 0 ||
        bad_meth_insns.size() > 0) {
      std::stringstream ss;
      for (const auto& bad_field : bad_fields) {
        for (const auto& field : bad_field.second) {
          bad_fields_count++;
          ss << "Reference to deleted type " <<
              SHOW(bad_field.first) << " in field " <<
              SHOW(field) << std::endl;
        }
      }
      for (const auto& bad_meth : bad_methods) {
        for (const auto& meth : bad_meth.second) {
          bad_methods_count++;
          ss << "Reference to deleted type " <<
              SHOW(bad_meth.first) << " in method " <<
              SHOW(meth) << std::endl;
        }
      }
      for (const auto& bad_insns : bad_type_insns) {
        for (const auto& insns : bad_insns.second) {
          for (const auto& insn : insns.second) {
            bad_type_insns_count++;
            ss << "Reference to deleted type " <<
                SHOW(bad_insns.first) << " in instruction " <<
                SHOW(insn) << " in method " << SHOW(insns.first) << std::endl;
          }
        }
      }
      for (const auto& bad_insns : bad_field_insns) {
        for (const auto& insns : bad_insns.second) {
          for (const auto& insn : insns.second) {
            bad_field_insns_count++;
            ss << "Reference to deleted field " <<
                SHOW(bad_insns.first) << " in instruction " <<
                SHOW(insn) << " in method " << SHOW(insns.first) << std::endl;
          }
        }
      }
      for (const auto& bad_insns : bad_meth_insns) {
        for (const auto& insns : bad_insns.second) {
          for (const auto& insn : insns.second) {
            bad_meths_insns_count++;
            ss << "Reference to deleted method " <<
                SHOW(bad_insns.first) << " in instruction " <<
                SHOW(insn) << " in method " << SHOW(insns.first) << std::endl;
          }
        }
      }
      TRACE(BRCR, 1,
          "Dangling References in Fields: %ld\n"
          "Dangling References in Methods: %ld\n"
          "Dangling References in Type Instructions: %ld\n"
          "Dangling References in Fields Field Instructions: %ld\n"
          "Dangling References in Method Instructions: %ld\n",
          bad_fields_count,
          bad_methods_count,
          bad_type_insns_count,
          bad_field_insns_count,
          bad_meths_insns_count);
      TRACE(BRCR, 2, "%s", ss.str().c_str());
      always_assert_log(report_only,
          "ERROR - Dangling References (contact redex@on-call):\n%s",
          ss.str().c_str());
    } else {
      TRACE(BRCR, 1, "No dangling references\n");
    }
    mgr.incr_metric(METRIC_BAD_FIELDS, bad_fields_count);
    mgr.incr_metric(METRIC_BAD_METHODS, bad_methods_count);
    mgr.incr_metric(METRIC_BAD_TYPE_INSTRUCTIONS, bad_type_insns_count);
    mgr.incr_metric(METRIC_BAD_FIELD_INSTRUCTIONS, bad_field_insns_count);
    mgr.incr_metric(METRIC_BAD_METHOD_INSTRUCTIONS, bad_meths_insns_count);
  }

 private:
  const DexType* check_type(const DexType* type) {
    const auto& cls = type_class(type);
    if (cls == nullptr) return nullptr;
    if (cls->is_external()) return nullptr;
    if (classes.count(cls) > 0) return nullptr;
    return type;
  }

  const DexType* check_method(const DexMethod* method) {
    const auto& proto = method->get_proto();
    auto type = check_type(proto->get_rtype());
    if (type != nullptr) return type;
    const auto& args = proto->get_args();
    if (args == nullptr) return nullptr;
    for (const auto& arg : args->get_type_list()) {
      type = check_type(arg);
      if (type != nullptr) return type;
    }
    return nullptr;
  }

  void bad_type(
      const DexType* type,
      const DexMethod* method,
      const IRInstruction* insn) {
    bad_type_insns[type][method].emplace_back(insn);
  }

  // verify that all field definitions are of a type not deleted
  void check_fields() {
    walk_fields(scope,
        [&](DexField* field) {
          const auto& type = check_type(field->get_type());
          if (type == nullptr) return;
          bad_fields[type].emplace_back(field);
        });
  }

  // verify that all method definitions use not deleted types in their sig
  void check_methods() {
    walk_methods(scope,
        [&](DexMethod* method) {
          const auto& type = check_method(method);
          if (type == nullptr) return;
          bad_methods[type].emplace_back(method);
        });
  }

  // verify that all opcodes are to non deleted references
  void check_type_opcode(const DexMethod* method, IRInstruction* insn) {
    const DexType* type = insn->get_type();
    type = check_type(type);
    if (type != nullptr) {
      bad_type(type, method, insn);
    }
  }

  void check_field_opcode(const DexMethod* method, IRInstruction* insn) {
    auto field = insn->get_field();
    const DexType* type = check_type(field->get_class());
    if (type != nullptr) {
      bad_type(type, method, insn);
      return;
    }
    type = check_type(field->get_type());
    if (type != nullptr) {
      bad_type(type, method, insn);
      return;
    }
    auto res_field = resolve_field(field);
    if (res_field != nullptr) {
      // a resolved field can only differ in the owner class
      if (field != res_field) {
        type = check_type(field->get_class());
        if (type != nullptr) {
          bad_type(type, method, insn);
          return;
        }
      }
    } else {
      // the class of the field is around but the field may have
      // been deleted so let's verify the field exists on the class
      if (field->is_def() && !class_contains(field)) {
        bad_field_insns[field][method].emplace_back(insn);
        return;
      }
    }
  }

  void check_method_opcode(const DexMethod* method, IRInstruction* insn) {
    const auto& meth = insn->get_method();
    const DexType* type = check_method(meth);
    if (type != nullptr) {
      bad_type(type, method, insn);
      return;
    }

    DexMethod* res_meth = resolve_method(meth, opcode_to_search(insn));
    if (res_meth != nullptr) {
      // a resolved method can only differ in the owner class
      if (res_meth != meth) {
        type = check_type(res_meth->get_class());
        if (type != nullptr) {
          bad_type(type, method, insn);
          return;
        }
      }
    } else {
      // the class of the method is around but the method may have
      // been deleted so let's verify the method exists on the class
      if (meth->is_def() && !class_contains(meth)) {
        bad_meth_insns[meth][method].emplace_back(insn);
        return;
      }
    }
  }

  void check_opcodes() {
    walk_opcodes(scope,
      [](DexMethod*) { return true; },
      [&](DexMethod* method, IRInstruction* insn) {
        if (insn->has_type()) {
          check_type_opcode(method, insn);
          return;
        }
        if (insn->has_field()) {
          check_field_opcode(method, insn);
          return;
        }
        if (insn->has_method()) {
          check_method_opcode(method, insn);
        }
      });
  }
};

}

void CheckBreadcrumbsPass::run_pass(
    DexStoresVector& stores,
    ConfigFiles& cfg,
    PassManager& mgr) {
  auto scope = build_class_scope(stores);
  Breadcrumbs bc(scope);
  bc.check_breadcrumbs();
  bc.report(!fail, mgr);
}

static CheckBreadcrumbsPass s_pass;
