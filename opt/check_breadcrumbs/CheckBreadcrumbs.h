/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <iosfwd>
#include <map>
#include <vector>

#include "DexClass.h" // All the comparators.
#include "DexStore.h" // XStoreRefs.
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

  void bind_config() override {
    bind("fail", false, fail);
    bind("fail_if_illegal_refs", false, fail_if_illegal_refs);
    bind("reject_illegal_refs_root_store",
         false,
         reject_illegal_refs_root_store);
    bind("only_verify_primary_dex", false, only_verify_primary_dex);
    bind("verify_type_hierarchies", false, verify_type_hierarchies);
    bind("verify_proto_cross_dex", false, verify_proto_cross_dex);
    bind("allowed_violations", "", allowed_violations_file_path);
    bind("enforce_allowed_violations_file",
         false,
         enforce_allowed_violations_file);
    trait(Traits::Pass::unique, true);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool fail;
  bool fail_if_illegal_refs;
  bool reject_illegal_refs_root_store;
  bool only_verify_primary_dex;
  bool verify_type_hierarchies;
  bool verify_proto_cross_dex;
  // Path to file with types or type prefixes to permit cross store violations.
  std::string allowed_violations_file_path;
  bool enforce_allowed_violations_file;
};

namespace {

using Fields = std::vector<const DexField*>;
using Methods = std::vector<const DexMethod*>;
using Instructions = std::vector<const IRInstruction*>;
using Types = std::vector<const DexType*>;
using MethodInsns =
    std::map<const DexMethod*, Instructions, dexmethods_comparator>;

} // namespace

class Breadcrumbs {
 public:
  explicit Breadcrumbs(const Scope& scope,
                       const std::string& allowed_violations_file_path,
                       DexStoresVector& stores,
                       bool reject_illegal_refs_root_store,
                       bool only_verify_primary_dex,
                       bool verify_type_hierarchies,
                       bool verify_proto_cross_dex,
                       bool enforce_allowed_violations_file);
  void check_breadcrumbs();
  void report_deleted_types(bool report_only, PassManager& mgr);
  std::string get_methods_with_bad_refs();
  void report_illegal_refs(bool fail_if_illegal_refs, PassManager& mgr);
  bool has_illegal_access(const DexMethod* input_method);

 private:
  const Scope& m_scope;
  Scope m_scope_to_walk;
  std::unordered_set<const DexClass*> m_classes;
  std::map<const DexType*, Fields, dextypes_comparator> m_bad_fields;
  std::map<const DexType*, Methods, dextypes_comparator> m_bad_methods;
  std::map<const DexType*, MethodInsns, dextypes_comparator> m_bad_type_insns;
  std::map<const DexField*, MethodInsns, dexfields_comparator>
      m_bad_field_insns;
  std::map<const DexMethod*, MethodInsns, dexmethods_comparator>
      m_bad_meth_insns;
  std::map<const DexType*, Fields, dextypes_comparator> m_illegal_field;
  std::map<const DexMethod*, Types, dexmethods_comparator> m_illegal_method;
  std::map<const DexMethod*, Fields, dexmethods_comparator> m_bad_fields_refs;
  MethodInsns m_illegal_type;
  MethodInsns m_illegal_field_type;
  MethodInsns m_illegal_field_cls;
  MethodInsns m_illegal_method_call;
  XStoreRefs m_xstores;
  std::unordered_set<const DexType*> m_allow_violations;
  std::unordered_set<std::string> m_allow_violation_type_prefixes;
  std::unordered_set<const DexType*> m_types_with_allowed_violations;
  std::unordered_set<std::string> m_type_prefixes_with_allowed_violations;
  std::vector<std::string> m_unneeded_violations_file_lines;
  bool m_multiple_root_store_dexes;
  bool m_reject_illegal_refs_root_store;
  bool m_verify_type_hierarchies;
  bool m_verify_proto_cross_dex;
  bool m_enforce_allowed_violations_file;

  bool should_allow_violations(const DexType* type);
  size_t process_illegal_elements(const XStoreRefs& xstores,
                                  const MethodInsns& method_to_insns,
                                  const char* desc,
                                  MethodInsns& suppressed,
                                  std::ostream& ss);
  bool is_illegal_cross_store(const DexType* caller, const DexType* callee);
  const DexType* check_type(const DexType* type);
  const DexType* check_method(const DexMethodRef* method);
  const DexType* check_anno(const DexAnnotationSet* anno);

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
