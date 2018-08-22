/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexUtil.h"
#include "IRCode.h"
#include "MethodReference.h"
#include "TypeTags.h"

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
                std::string val_str);

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

  std::vector<ConstantLoad> collect_constant_loads_in(IRCode* code);

  std::vector<IRInstruction*> make_load_const(uint16_t const_reg);

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
