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
#include "DexUtil.h"
#include "IRInstruction.h"
#include "Resolver.h"

namespace {

constexpr const char* METRIC_BAD_FIELDS = "bad_fields";
constexpr const char* METRIC_BAD_METHODS = "bad_methods";
constexpr const char* METRIC_BAD_TYPE_INSTRUCTIONS = "bad_type_instructions";
constexpr const char* METRIC_BAD_FIELD_INSTRUCTIONS = "bad_field_instructions";
constexpr const char* METRIC_BAD_METHOD_INSTRUCTIONS =
    "bad_method_instructions";
constexpr const char* METRIC_ILLEGAL_CROSS_STORE_REFS =
    "illegal_cross_store_refs";

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

size_t illegal_elements(const MethodInsns& method_to_insns,
                        const char* msj,
                        std::ostringstream& ss) {
  size_t num_illegal_cross_store_refs = 0;
  for (const auto& pair : method_to_insns) {
    const auto method = pair.first;
    const auto& insns = pair.second;
    ss << "Illegal " << msj << " in method "
       << method->get_deobfuscated_name() << std::endl;
    num_illegal_cross_store_refs += insns.size();
    for (const auto insn : insns) {
      ss << "\t" << show_deobfuscated(insn) << std::endl;
    }
  }

  return num_illegal_cross_store_refs;
}

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
  std::map<const DexType*, Fields, dextypes_comparator> illegal_field;
  MethodInsns illegal_type;
  MethodInsns illegal_field_type;
  MethodInsns illegal_field_cls;
  MethodInsns illegal_method_call;
  XStoreRefs xstores;
  bool multiple_root_store_dexes;

 public:

  explicit Breadcrumbs(const Scope& scope, DexStoresVector& stores)
    : scope(scope),
      xstores(stores) {
    classes.insert(scope.begin(), scope.end());
    multiple_root_store_dexes = stores[0].get_dexen().size() > 1;
  }

  void check_breadcrumbs() {
    check_fields();
    check_methods();
    check_opcodes();
  }

  void report_deleted_types(bool report_only, PassManager& mgr) {
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
      std::ostringstream ss;
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


  void report_illegal_refs(bool fail_if_illegal_refs, PassManager& mgr) {
    size_t num_illegal_fields = 0;
    std::ostringstream ss;
    for (const auto& pair : illegal_field) {
      const auto type = pair.first;
      const auto& fields = pair.second;
      num_illegal_fields += fields.size();

      ss << "Illegal fields in class "
         << type_class(type)->get_deobfuscated_name()
         << std::endl;;
      for (const auto field : fields) {
        ss << "\t" << field->get_deobfuscated_name() << std::endl;
      }
    }

    size_t num_illegal_type_refs =
      illegal_elements(illegal_type, "type refs", ss);
    size_t num_illegal_field_type_refs =
      illegal_elements(illegal_field_type, "field type refs", ss);
    size_t num_illegal_field_cls =
      illegal_elements(illegal_field_cls, "field class refs", ss);
    size_t num_illegal_method_calls =
      illegal_elements(illegal_method_call, "method call", ss);

    size_t num_illegal_cross_store_refs =
      num_illegal_fields + num_illegal_type_refs +
      num_illegal_field_cls + num_illegal_field_type_refs +
      num_illegal_method_calls;
    mgr.set_metric(METRIC_ILLEGAL_CROSS_STORE_REFS, num_illegal_cross_store_refs);

    TRACE(BRCR,
          1,
          "Illegal fields : %ld\n"
          "Illegal type refs : %ld\n"
          "Illegal field type refs : %ld\n"
          "Illegal field cls refs : %ld\n"
          "Illegal method calls : %ld\n",
          num_illegal_fields,
          num_illegal_type_refs,
          num_illegal_field_type_refs,
          num_illegal_field_cls,
          num_illegal_method_calls);
    TRACE(BRCR, 2, "%s", ss.str().c_str());

    always_assert_log(ss.str().empty() || !fail_if_illegal_refs,
                      "ERROR - illegal cross store references "
                      "(contact redex@on-call):\n%s",
                      ss.str().c_str());
  }

 private:
  bool is_illegal_cross_store(const DexType* caller, const DexType* callee) {
    // Skip deleted types, as we don't know the store for those.
    if (classes.count(type_class(caller)) == 0 ||
        classes.count(type_class(callee)) == 0) {
      return false;
    }

    size_t caller_store_idx = xstores.get_store_idx(caller);
    size_t callee_store_idx = xstores.get_store_idx(callee);
    if (caller_store_idx < callee_store_idx) {
      // Accept primary to secondary references.
      if (multiple_root_store_dexes &&
          caller_store_idx == 0 &&
          callee_store_idx == 1) {
        return false;
      }

      return true;
    }

    return false;
  }

  const DexType* check_type(const DexType* type) {
    const auto& cls = type_class(type);
    if (cls == nullptr) return nullptr;
    if (cls->is_external()) return nullptr;
    if (classes.count(cls) > 0) return nullptr;
    return type;
  }

  const DexType* check_method(const DexMethodRef* method) {
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
    walk::fields(scope,
        [&](DexField* field) {
          const auto& type = check_type(field->get_type());
          if (type == nullptr) {
            const auto cls = field->get_class();
            const auto field_type = field->get_type();
            if (is_illegal_cross_store(cls, field_type)) {
              illegal_field[cls].emplace_back(field);
            }
            return;
          }
          bad_fields[type].emplace_back(field);
        });
  }

  // verify that all method definitions use not deleted types in their sig
  void check_methods() {
    walk::methods(scope,
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
    } else {
      const auto cls = method->get_class();
      if (is_illegal_cross_store(cls, insn->get_type())) {
        illegal_type[method].emplace_back(insn);
      }
    }
  }

  void check_field_opcode(const DexMethod* method, IRInstruction* insn) {
    auto field = insn->get_field();
    const DexType* type = check_type(field->get_class());
    if (type != nullptr) {
      bad_type(type, method, insn);
      return;
    }

    auto cls = method->get_class();
    if (is_illegal_cross_store(cls, field->get_class())) {
      illegal_field_type[method].emplace_back(insn);
    }

    type = check_type(field->get_type());
    if (type != nullptr) {
      bad_type(type, method, insn);
      return;
    }

    if (is_illegal_cross_store(cls, field->get_type())) {
      illegal_field_cls[method].emplace_back(insn);
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
      if (field->is_def() && !class_contains(static_cast<DexField*>(field))) {
        bad_field_insns[static_cast<DexField*>(field)][method].
            emplace_back(insn);
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
    if (is_illegal_cross_store(method->get_class(), meth->get_class())) {
      illegal_method_call[method].emplace_back(insn);
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
      if (meth->is_def()) {
        const auto meth_def = static_cast<DexMethod*>(meth);
        if (!class_contains(meth_def)) {
          bad_meth_insns[meth_def][method].emplace_back(insn);
          return;
        }
      }
    }
  }

  void check_opcodes() {
    walk::opcodes(scope,
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
  Breadcrumbs bc(scope, stores);
  bc.check_breadcrumbs();
  bc.report_deleted_types(!fail, mgr);
  bc.report_illegal_refs(fail_if_illegal_refs, mgr);
}

static CheckBreadcrumbsPass s_pass;
