/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

#include <set>
#include <string>
#include <unordered_map>

class CreateReferenceGraphPass : public Pass {
 public:
  CreateReferenceGraphPass() : Pass("CreateReferenceGraphPass") {}

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("gather_all", false, config.gather_all);

    jw.get("refs_in_annotations", true, config.refs_in_annotations);
    jw.get("refs_in_class_structure", true, config.refs_in_class_structure);
    jw.get("refs_in_code", true, config.refs_in_code);

    jw.get("resolve_fields", false, config.resolve_fields);
    jw.get("resolve_methods", false, config.resolve_methods);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  struct Config {
    std::string ref_output_filename;

    bool gather_all;

    bool refs_in_annotations;
    bool refs_in_class_structure;
    bool refs_in_code;

    bool resolve_fields;
    bool resolve_methods;
  };
  Config config;

  // refs_t is the "graph" type. It is a map from every class in the scope
  // to the types each refer to
  //
  // Use the config file to decide which types of references to collect
  using refs_t = std::unordered_map<const DexClass*, std::set<const DexType*, dextypes_comparator>>;
  using type_to_store_map_t = std::unordered_map<const DexType*, DexStore*>;

  using MethodWalkerFn = std::function<void(DexMethod*)>;
  using FieldWalkerFn = std::function<void(DexField*)>;
  using AnnotationWalkerFn = std::function<void(DexAnnotation*)>;
  using InstructionWalkerFn = std::function<void(DexMethod*, IRInstruction*)>;

  void build_super_and_interface_refs(
      const Scope& scope,
      refs_t& class_refs);

  template <class T>
  void get_annots(
      const T* thing_with_annots,
      const DexClass* enclosing_class,
      refs_t& class_refs);

  MethodWalkerFn method_ref_builder(
      const Scope& scope,
      refs_t& class_refs);

  FieldWalkerFn field_ref_builder(
      const Scope& scope,
      refs_t& class_refs);

  void build_class_annot_refs(
      const Scope& scope,
      refs_t& class_refs);

  MethodWalkerFn method_annot_ref_builder(
      const Scope& scope,
      refs_t& class_refs);

  FieldWalkerFn field_annot_ref_builder(
      const Scope& scope,
      refs_t& class_refs);

  MethodWalkerFn exception_ref_builder(
      const Scope& scope,
      refs_t& class_refs);

  InstructionWalkerFn instruction_ref_builder(
      const Scope& scope,
      refs_t& class_refs);

  void gather_all(const Scope& scope, refs_t& class_refs);

  void build_refs(const Scope& scope, refs_t& class_refs);

  void createAndOutputRefGraph(
      DexStore& store,
      type_to_store_map_t type_to_store);
};
