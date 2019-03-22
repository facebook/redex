/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

/**
 * This pass only makes sense when applied at the end of a redex optimization
 * run. It does not work on its own when applied to a "random" apk.
 * It relies on the fact that deleted classes/methods/fields are still
 * around at the end of a run.
 */
class CheckBreadcrumbsPass : public Pass {
 public:
  CheckBreadcrumbsPass() : Pass("CheckBreadcrumbsPass") {}

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("fail", false, fail);
    jw.get("fail_if_illegal_refs", false, fail_if_illegal_refs);
    jw.get("reject_illegal_refs_root_store",
           false,
           reject_illegal_refs_root_store);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool fail;
  bool fail_if_illegal_refs;
  bool reject_illegal_refs_root_store;
};

namespace {

using Fields = std::vector<const DexField*>;
using Methods = std::vector<const DexMethod*>;
using Instructions = std::vector<const IRInstruction*>;
using MethodInsns =
    std::map<const DexMethod*, Instructions, dexmethods_comparator>;

} // namespace

class Breadcrumbs {
 public:
  explicit Breadcrumbs(const Scope& scope, DexStoresVector& stores, bool reject_illegal_refs_root_store);
  void check_breadcrumbs();
  void report_deleted_types(bool report_only, PassManager& mgr);
  std::string get_methods_with_bad_refs();
  void report_illegal_refs(bool fail_if_illegal_refs, PassManager& mgr);
  bool has_illegal_access(const DexMethod* input_method);

 private:
  const Scope& m_scope;
  std::unordered_set<const DexClass*> m_classes;
  std::map<const DexType*, Fields, dextypes_comparator> m_bad_fields;
  std::map<const DexType*, Methods, dextypes_comparator> m_bad_methods;
  std::map<const DexType*, MethodInsns, dextypes_comparator> m_bad_type_insns;
  std::map<const DexField*, MethodInsns, dexfields_comparator>
      m_bad_field_insns;
  std::map<const DexMethod*, MethodInsns, dexmethods_comparator>
      m_bad_meth_insns;
  std::map<const DexType*, Fields, dextypes_comparator> m_illegal_field;
  std::map<const DexMethod*, Fields, dexmethods_comparator> m_bad_fields_refs;
  MethodInsns m_illegal_type;
  MethodInsns m_illegal_field_type;
  MethodInsns m_illegal_field_cls;
  MethodInsns m_illegal_method_call;
  XStoreRefs m_xstores;
  bool m_multiple_root_store_dexes;
  bool m_reject_illegal_refs_root_store;

  bool is_illegal_cross_store(const DexType* caller, const DexType* callee);
  const DexType* check_type(const DexType* type);
  const DexType* check_method(const DexMethodRef* method);
  void bad_type(const DexType* type,
                const DexMethod* method,
                const IRInstruction* insn);
  void check_fields();
  void check_methods();
  bool referenced_field_is_deleted(DexFieldRef* field);
  bool referenced_method_is_deleted(DexMethodRef* method);
  bool check_field_accessibility(const DexMethod* method,
                                 const DexField* res_field);
  bool check_method_accessibility(const DexMethod* method,
                                  const DexMethod* res_called_method);
  void check_type_opcode(const DexMethod* method, IRInstruction* insn);
  void check_field_opcode(const DexMethod* method, IRInstruction* insn);
  void check_method_opcode(const DexMethod* method, IRInstruction* insn);
  void check_opcodes();
};
