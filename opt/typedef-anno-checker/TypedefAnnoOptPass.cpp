/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypedefAnnoOptPass.h"

#include "AnnoUtils.h"
#include "CFGMutation.h"
#include "MethodReference.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Show.h"
#include "StringTreeSet.h"
#include "Trace.h"
#include "TypeUtil.h"
#include "Walkers.h"

constexpr const char* VALUE_OF = "valueOf";
constexpr const char* VALUE_OF_OPT = "valueOfOpt";

// derive the util class from the typedef class and find the valueOfOpt method
DexMethod* get_util_method(DexClass* cls, const char* name) {
  auto util_cls_str =
      cls->get_type()->str().substr(0, cls->get_type()->str().size() - 1) +
      "$Util;";
  auto util_cls = type_class(DexType::make_type(util_cls_str));
  cls = util_cls;
  if (!cls) {
    return nullptr;
  }
  return cls->find_method_from_simple_deobfuscated_name(name);
}

// fill the const string in valueOfOpt with the encoded map
void fill_encoded_string(DexMethod* m, const DexString* encoded_dex_str) {
  auto& cfg = m->get_code()->cfg();
  cfg::Block* const_str_block = nullptr;
  for (auto* block : cfg.blocks()) {
    for (const auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      if (insn->opcode() == OPCODE_CONST_STRING &&
          insn->get_string()->str().empty()) {
        insn->set_string(encoded_dex_str);
        const_str_block = block;
      }
    }
  }
  always_assert_log(const_str_block,
                    "could not find a block containing an empty str for the "
                    "encoded map in valueOfOpt");

  set_public(m);
}

void TypedefAnnoOptPass::populate_value_of_opt_str(DexClass* cls) {
  const std::vector<DexField*>& fields = cls->get_sfields();
  if (get_annotation(cls, m_config.str_typedef)) {

    auto m = get_util_method(cls, VALUE_OF_OPT);
    if (m == nullptr) {
      return;
    }

    std::map<std::string, std::string> string_tree_items;
    for (auto* field : fields) {
      auto field_value =
          static_cast<DexEncodedValueString*>(field->get_static_value())
              ->string();
      string_tree_items.emplace(field->get_simple_deobfuscated_name(),
                                field_value->str_copy());
    }

    auto encoded_str =
        StringTreeStringMap::encode_string_tree_map(string_tree_items);
    auto encoded_dex_str = DexString::make_string(encoded_str);
    fill_encoded_string(m, encoded_dex_str);
    old_to_new_callee.emplace(get_util_method(cls, VALUE_OF), m);

  } else if (get_annotation(cls, m_config.int_typedef)) {

    auto m = get_util_method(cls, VALUE_OF_OPT);
    if (m == nullptr) {
      return;
    }

    std::map<std::string, int32_t> string_tree_items;
    for (auto* field : fields) {
      int32_t field_value = field->get_static_value()->value();
      string_tree_items.emplace(field->get_simple_deobfuscated_name(),
                                field_value);
    }

    auto encoded_str =
        StringTreeMap<int32_t>::encode_string_tree_map(string_tree_items);
    auto encoded_dex_str = DexString::make_string(encoded_str);
    fill_encoded_string(m, encoded_dex_str);
    old_to_new_callee.emplace(get_util_method(cls, VALUE_OF), m);
  }
}

void TypedefAnnoOptPass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& /* unused */,
                                  PassManager& /* unused */) {
  always_assert(m_config.int_typedef != nullptr);
  always_assert(m_config.str_typedef != nullptr);
  auto scope = build_class_scope(stores);

  walk::parallel::classes(
      scope, [&](DexClass* cls) { populate_value_of_opt_str(cls); });

  method_reference::update_call_refs_simple(scope, old_to_new_callee);
}

static TypedefAnnoOptPass s_pass;
