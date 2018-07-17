/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantLifting.h"

#include "AnnoUtils.h"
#include "IRCode.h"
#include "MethodReference.h"
#include "Resolver.h"
#include "TypeReference.h"
#include "TypeTags.h"

using MethodOrderedSet = std::set<DexMethod*, dexmethods_comparator>;

namespace {

constexpr const char* METHOD_META =
    "Lcom/facebook/redex/annotations/MethodMeta;";
constexpr const char* CONST_TYPE_ANNO_ATTR_NAME = "constantTypes";
constexpr const char* CONST_VALUE_ANNO_ATTR_NAME = "constantValues";

std::vector<IRInstruction*> make_string_const(uint16_t dest, std::string val) {
  std::vector<IRInstruction*> res;
  IRInstruction* load = new IRInstruction(OPCODE_CONST_STRING);
  load->set_string(DexString::make_string(val));
  IRInstruction* move_result_pseudo =
      new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
  move_result_pseudo->set_dest(dest);
  res.push_back(load);
  res.push_back(move_result_pseudo);
  return res;
}

bool overlaps_with_an_existing_virtual_scope(DexType* type,
                                             DexString* name,
                                             DexProto* proto) {
  if (DexMethod::get_method(type, name, proto)) {
    return true;
  }
  DexClass* cls = type_class(type);
  while (cls->get_super_class()) {
    type = cls->get_super_class();
    if (DexMethod::get_method(type, name, proto)) {
      return true;
    }
    cls = type_class(type);
  }

  return false;
}

class ConstantValue {

  /////////////////////////////////////////////////////////////////////////
  // The kind of constant value emitted in the @MethodMeta annotation.
  //
  // INT: @MethodMeta(constantTypes = "I", constantValues = "42")
  //       A 64 bit integer constant like an offset or hash code.
  // TYPE: @MethodMeta(constantTypes = "T",
  //                   constantValues = "Lcom/facebook/CommentModels$ModelA;")
  //       The constant in the annotated method is a reference to a mergeable
  //       type. We only process if the refernced type is merged to a Shape.
  //       In that case, the type reference becomes the type tag constant and a
  //       reference to the merger type. As a result, the type tag integer
  //       constant is the only constant value we need to lift.
  //       The Type case can be considered as a special Int case, where type tag
  //       become the only constant making the annotated methods different from
  //       each other after merging.
  //       This is specially useful when the type tags are not accessible from
  //       the code-gen.
  // STRING: @MethodMeta(constantTypes = "S", constantValues = "post_id")
  //         A string constant like the name of a parameter.
  // INVALID:
  //        Whenever the annotated value cannot be processed. For instance, the
  //        emitted Type cannot be found or is not merged.
  enum ConstantKind { INT, TYPE, STRING, INVALID };

  // The insn and the dest. OPCODE_CONST_STRING does not have a dest in itself.
  using ConstantLoad = std::pair<IRInstruction*, uint16_t>;

 public:
  ConstantValue(const TypeTags* type_tags,
                std::string kind_str,
                std::string val_str) {
    if (kind_str == "I") {
      m_kind = ConstantKind::INT;
      m_int_val = std::stoll(val_str);
    } else if (kind_str == "T") {
      auto type_val = DexType::get_type(val_str.c_str());
      if (type_val != nullptr && type_tags->has_type_tag(type_val)) {
        m_kind = ConstantKind::TYPE;
        m_int_val = type_tags->get_type_tag(type_val);
        return;
      } else if (type_val == nullptr) {
        TRACE(TERA, 9, "const lift: unable to find type %s\n", val_str.c_str());
      }
      // Cannot find type or not type tag.
      m_kind = ConstantKind::INVALID;
    } else if (kind_str == "S") {
      m_kind = ConstantKind::STRING;
      m_str_val = val_str;
    } else {
      always_assert_log(false, "Unexpected kind str %s\n", kind_str.c_str());
    }
  }

  bool is_int_value() {
    return m_kind == ConstantKind::INT || m_kind == ConstantKind::TYPE;
  }
  bool is_str_value() { return m_kind == ConstantKind::STRING; }
  int64_t get_int_value() {
    always_assert(is_int_value());
    return m_int_val;
  }
  bool is_invalid() { return m_kind == ConstantKind::INVALID; }
  std::string get_str_value() {
    always_assert(is_str_value());
    return m_str_val;
  }
  DexType* get_constant_type() {
    if (is_int_value()) {
      return get_int_type();
    } else {
      return get_string_type();
    }
  }

  std::vector<ConstantLoad> collect_constant_loads_in(IRCode* code) {
    std::vector<ConstantLoad> res;
    if (is_invalid()) {
      return res;
    }
    auto ii = InstructionIterable(code);
    for (auto it = ii.begin(); it != ii.end(); ++it) {
      auto insn = it->insn;
      if (is_int_value() && is_literal_const(insn->opcode())) {
        int64_t literal = insn->get_literal();
        // Special handling for type tags to avoid sign extensionon on int64_t.
        if (m_kind == ConstantKind::TYPE) {
          literal = static_cast<uint32_t>(literal);
        }
        if (is_int_value() && literal == m_int_val) {
          res.emplace_back(insn, insn->dest());
        }
      } else if (is_str_value() && insn->opcode() == OPCODE_CONST_STRING) {
        if (strcmp(insn->get_string()->c_str(), m_str_val.c_str()) == 0) {
          auto pseudo_move = std::next(it)->insn;
          always_assert(pseudo_move->opcode() ==
                        IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
          res.emplace_back(insn, pseudo_move->dest());
        }
      }
    }

    return res;
  }

  std::vector<IRInstruction*> make_load_const(uint16_t const_reg) {
    always_assert(!is_invalid());

    if (is_int_value()) {
      std::vector<IRInstruction*> res;
      auto load = method_reference::make_load_const(const_reg, m_int_val);
      res.push_back(load);
      return res;
    } else {
      return make_string_const(const_reg, m_str_val);
    }
  }

  std::string to_str() {
    std::ostringstream ss;
    if (is_int_value()) {
      ss << m_int_val;
    } else if (is_str_value()) {
      ss << m_str_val;
    } else {
      ss << "invalid";
    }
    return ss.str();
  }

 private:
  ConstantKind m_kind;
  int64_t m_int_val;
  std::string m_str_val;
};

} // namespace

using MethodToConstant =
    std::map<DexMethod*, ConstantValue, dexmethods_comparator>;

const DexType* s_method_meta_anno;

ConstantLifting::ConstantLifting() : m_num_const_lifted_methods(0) {
  s_method_meta_anno = DexType::get_type(DexString::get_string(METHOD_META));
}

bool ConstantLifting::is_applicable_to_constant_lifting(
    const DexMethod* method) {
  if (is_synthetic(method) || !has_anno(method, s_method_meta_anno)) {
    return false;
  }
  if (!has_attribute(method, s_method_meta_anno, CONST_TYPE_ANNO_ATTR_NAME)) {
    return false;
  }
  return true;
}

void ConstantLifting::lift_constants_from(
    const Scope& scope,
    const TypeTags* type_tags,
    const std::vector<DexMethod*>& methods) {
  MethodOrderedSet lifted;
  MethodToConstant lifted_constants;
  for (auto method : methods) {
    always_assert(has_anno(method, s_method_meta_anno));
    auto const_kind_str = parse_str_anno_value(
        method, s_method_meta_anno, CONST_TYPE_ANNO_ATTR_NAME);
    auto const_str = parse_str_anno_value(
        method, s_method_meta_anno, CONST_VALUE_ANNO_ATTR_NAME);

    ConstantValue const_value(type_tags, const_kind_str, const_str);
    auto const_loads =
        const_value.collect_constant_loads_in(method->get_code());
    if (const_loads.size() == 0) {
      // No matching constant found.
      TRACE(TERA,
            5,
            "  no matching constant %s found in %s\n",
            const_value.to_str().c_str(),
            SHOW(method));
      TRACE(TERA, 9, "%s\n", SHOW(method->get_code()));
      continue;
    }
    lifted.insert(method);
    lifted_constants.emplace(method, const_value);
    TRACE(TERA,
          5,
          "constant lifting: const value %s\n",
          const_value.to_str().c_str());

    // Add constant to arg list.
    auto old_proto = method->get_proto();
    auto const_type = const_value.get_constant_type();
    auto arg_list =
        type_reference::append_and_make(old_proto->get_args(), const_type);
    auto new_proto = DexProto::make_proto(old_proto->get_rtype(), arg_list);

    // Find a non-conflicting name
    auto name = method->get_name();
    std::string suffix = "$r";
    while (overlaps_with_an_existing_virtual_scope(
        method->get_class(), name, new_proto)) {
      name = DexString::make_string(name->c_str() + suffix);
      TRACE(TERA,
            9,
            "constant lifting method name updated to %s\n",
            name->c_str());
    }

    // Update method
    DexMethodSpec spec;
    spec.name = name;
    spec.proto = new_proto;
    method->change(spec, true);

    // Insert param load.
    auto code = method->get_code();
    auto const_val_reg = code->allocate_temp();
    auto params = code->get_param_instructions();
    auto load_type_tag_param =
        const_value.is_int_value()
            ? new IRInstruction(IOPCODE_LOAD_PARAM)
            : new IRInstruction(IOPCODE_LOAD_PARAM_OBJECT);
    load_type_tag_param->set_dest(const_val_reg);
    code->insert_before(params.end(), load_type_tag_param);

    // Replace const loads with moves.
    for (const auto load : const_loads) {
      auto insn = load.first;
      auto dest = load.second;
      auto move_const_arg = const_value.is_int_value()
                                ? new IRInstruction(OPCODE_MOVE)
                                : new IRInstruction(OPCODE_MOVE_OBJECT);
      move_const_arg->set_dest(dest);
      move_const_arg->set_src(0, const_val_reg);
      code->replace_opcode(insn, move_const_arg);
    }
  }
  TRACE(TERA,
        5,
        "constant lifting applied to %ld among %ld\n",
        lifted.size(),
        methods.size());
  m_num_const_lifted_methods += methods.size();

  // Update call sites
  auto call_sites = method_reference::collect_call_refs(scope, lifted);
  for (const auto& pair : call_sites) {
    auto meth = pair.first;
    auto insn = pair.second;
    const auto callee =
        resolve_method(insn->get_method(), opcode_to_search(insn));
    always_assert(callee != nullptr);
    // Make const load
    auto code = meth->get_code();
    auto const_reg = code->allocate_temp();
    auto const_value = lifted_constants.at(callee);
    auto const_load_and_invoke = const_value.make_load_const(const_reg);
    // Insert const load
    std::vector<uint16_t> args;
    for (size_t i = 0; i < insn->srcs_size(); i++) {
      args.push_back(insn->src(i));
    }
    args.push_back(const_reg);
    auto invoke = method_reference::make_invoke(callee, insn->opcode(), args);
    const_load_and_invoke.push_back(invoke);
    code->insert_after(insn, const_load_and_invoke);
    // remove original call.
    code->remove_opcode(insn);
    TRACE(TERA, 9, " patched call site in %s\n%s\n", SHOW(meth), SHOW(code));
  }
}
