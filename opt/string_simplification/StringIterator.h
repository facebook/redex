/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <functional>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "FixpointIterators.h"
#include "StringDomain.h"

constexpr const char* STRING_DEF = "Ljava/lang/String;";
constexpr const char* STRINGBUILDER_DEF = "Ljava/lang/StringBuilder;";

class StringIterator : public MonotonicFixpointIterator<cfg::GraphInterface,
                                                        StringProdEnvironment> {
  using NodeId = cfg::Block*;
  using Environment = StringProdEnvironment;

 public:
  StringIterator(IRCode* code, NodeId start_block)
      : MonotonicFixpointIterator(code->cfg()),
        m_code(code),
        m_string_type(DexType::make_type(STRING_DEF)),
        m_builder_type(DexType::make_type(STRINGBUILDER_DEF)),
        m_append_method(DexMethod::make_method(
            STRINGBUILDER_DEF, "append", STRINGBUILDER_DEF, {STRING_DEF})),
        m_to_string_method(DexMethod::make_method(
            STRINGBUILDER_DEF, "toString", STRING_DEF, {})),
        m_strings_added(0),
        m_instructions_added(0),
        m_instructions_removed(0) {}

  size_t get_strings_added() const { return m_strings_added; }
  size_t get_instructions_added() const { return m_instructions_added; }
  size_t get_instructions_removed() const { return m_instructions_removed; }

  Environment analyze_edge(
      const EdgeId&,
      const Environment& exit_state_at_source) const override {
    return exit_state_at_source;
  }

  void analyze_node(const NodeId& block, Environment* env) const override {
    for (auto it = block->begin(); it != block->end(); ++it) {
      if (it->type == MFLOW_OPCODE) {
        analyze_instruction(block, it, env);
      }
    }
  }

  void simplify() {
    for (const auto& block : m_code->cfg().blocks()) {
      auto state = this->get_entry_state_at(block);
      for (auto it = block->begin(); it != block->end(); ++it) {
        if (it->type != MFLOW_OPCODE) {
          continue;
        }
        analyze_instruction(block, it, &state);
        simplify_instruction(block, it, &state);
      }
    }
  }

 private:
  // Performs the abstract interpretation analysis on a per instruction basis.
  // Cases considered:
  // new_instance -> create new object in pool.
  // constructor -> set up initial value.
  // const-string -> new object that is static.
  // append -> handle if possible, otherwise set to top.
  // to_string -> check result.
  // overwritten dest -> clear register pointer (object can exist elsewhere).
  void analyze_instruction(const NodeId blk,
                           IRList::iterator& it,
                           Environment* env) const;

  // Modifies instructions by finding toString method calls and removing all
  // opcodes related to an object in the block, and then either inserting a
  // const-string or building a simpler invocation of the stringbuilder.
  void simplify_instruction(const NodeId block,
                            IRList::iterator& it,
                            const Environment* current_state);

 private:
  // Walks backwards and removes all stringbuilder instructions.  It initially
  // points at the toString method, which will be deleted.
  // In the end, it will point to the valid instruction immediately before,
  // or the same instruction if it is the beginning of the block.
  void remove_stringbuilder_instructions_in_block(IRList::iterator& it,
                                                  const Environment* env,
                                                  NodeId block,
                                                  string_register_t sb_reg);

  bool is_const_string(IRList::iterator it) const;
  bool is_sb_new_instance(IRList::iterator it) const;
  bool is_sb_empty_init(IRList::iterator it) const;
  bool is_sb_string_init(IRList::iterator it) const;
  bool is_sb_append_string(IRList::iterator it) const;
  bool is_sb_to_string(IRList::iterator it) const;

  void insert_const_string(IRList::iterator& it,
                           string_register_t sb,
                           std::string s);

  void insert_sb_init(IRList::iterator& it, string_register_t vreg);

  void insert_sb_append(IRList::iterator& it,
                        string_register_t sb,
                        string_register_t rhs);

  void insert_sb_to_string(IRList::iterator& it,
                           string_register_t sb,
                           string_register_t dest);

  IRCode* m_code;
  const DexType* m_string_type;
  const DexType* m_builder_type;
  DexMethodRef* m_append_method;
  DexMethodRef* m_to_string_method;

  size_t m_strings_added;
  size_t m_instructions_added;
  size_t m_instructions_removed;
};
