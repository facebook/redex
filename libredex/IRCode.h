/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "DexClass.h"
#include "DexDebugInstruction.h"
#include "IRInstruction.h"
#include "IRList.h"

namespace cfg {
class ControlFlowGraph;
}

// TODO(jezng): IRCode currently contains too many methods that shouldn't
// belong there... I'm going to move them out soon
class IRCode {
 private:
  /* try_sync() is the work-horse of sync.  It's intended such that it can fail
   * in the event that an opcode needs to be resized.  In that instance, it
   * changes the opcode in question, and returns false.  It's intended to be
   * called multiple times until it returns true.
   */
  bool try_sync(DexCode*);

  void split_and_insert_try_regions(
      uint32_t start,
      uint32_t end,
      const DexCatches& catches,
      std::vector<std::unique_ptr<DexTryItem>>* tries);

  IRList* m_ir_list;
  std::unique_ptr<cfg::ControlFlowGraph> m_cfg;

  uint16_t m_registers_size{0};
  // TODO(jezng): we shouldn't be storing / exposing the DexDebugItem... just
  // exposing the param names should be enough
  std::unique_ptr<DexDebugItem> m_dbg;

  IRList::iterator main_block() { return m_ir_list->main_block(); }
  IRList::iterator make_if_block(IRList::iterator cur,
                                 IRInstruction* insn,
                                 IRList::iterator* if_block) {
    return m_ir_list->make_if_block(cur, insn, if_block);
  }
  IRList::iterator make_if_else_block(IRList::iterator cur,
                                      IRInstruction* insn,
                                      IRList::iterator* if_block,
                                      IRList::iterator* else_block) {
    return m_ir_list->make_if_else_block(cur, insn, if_block, else_block);
  }
  IRList::iterator make_switch_block(
      IRList::iterator cur,
      IRInstruction* insn,
      IRList::iterator* default_block,
      std::map<SwitchIndices, IRList::iterator>& cases) {
    return m_ir_list->make_switch_block(cur, insn, default_block, cases);
  }

  friend struct MethodCreator;

 public:
  // This creates an "empty" IRCode, one that contains no load-param opcodes or
  // debug info. If you attach it to a method, you need to insert the
  // appropriate load-param opcodes yourself. Mostly used for testing purposes.
  IRCode();

  explicit IRCode(DexMethod*);
  /*
   * Construct an IRCode for a DexMethod that has no DexCode (that is, a new
   * method that we are creating instead of something from an input dex file.)
   * In the absence of a register allocator, we need the parameters to be
   * loaded at the end of the register frame. Since we don't have a DexCode
   * object, we need to specify the frame size ourselves, hence the extra
   * parameter.
   *
   * Once we have an allocator we should be able to deprecate this.
   */
  explicit IRCode(DexMethod*, size_t temp_regs);

  IRCode(const IRCode& code);

  ~IRCode();

  bool structural_equals(const IRCode& other) {
    return m_ir_list->structural_equals(*other.m_ir_list, std::equal_to<const IRInstruction&>());
  }

  bool structural_equals(const IRCode& other,
                         const InstructionEquality& instruction_equals) {
    return m_ir_list->structural_equals(*other.m_ir_list, instruction_equals);
  }

  uint16_t get_registers_size() const { return m_registers_size; }

  void set_registers_size(uint16_t sz) { m_registers_size = sz; }

  uint16_t allocate_temp() { return m_registers_size++; }

  uint16_t allocate_wide_temp() {
    uint16_t new_reg = m_registers_size;
    m_registers_size += 2;
    return new_reg;
  }

  /*
   * Find the subrange of load-param instructions. These instructions should
   * always be at the beginning of the method.
   */
  boost::sub_range<IRList> get_param_instructions() const {
    return m_ir_list->get_param_instructions();
  }

  void set_debug_item(std::unique_ptr<DexDebugItem> dbg) {
    m_dbg = std::move(dbg);
  }
  const DexDebugItem* get_debug_item() const { return m_dbg.get(); }
  DexDebugItem* get_debug_item() { return m_dbg.get(); }
  std::unique_ptr<DexDebugItem> release_debug_item() {
    return std::move(m_dbg);
  }

  void gather_catch_types(std::vector<DexType*>& ltype) const {
    m_ir_list->gather_catch_types(ltype);
    if (m_dbg) m_dbg->gather_types(ltype);
  }
  void gather_strings(std::vector<DexString*>& lstring) const {
    m_ir_list->gather_strings(lstring);
    if (m_dbg) m_dbg->gather_strings(lstring);
  }
  void gather_types(std::vector<DexType*>& ltype) const {
    m_ir_list->gather_types(ltype);
  }
  void gather_fields(std::vector<DexFieldRef*>& lfield) const {
    m_ir_list->gather_fields(lfield);
  }
  void gather_methods(std::vector<DexMethodRef*>& lmethod) const {
    m_ir_list->gather_methods(lmethod);
  }

  /* Return the control flow graph of this method as a vector of blocks. */
  cfg::ControlFlowGraph& cfg() { return *m_cfg; }

  const cfg::ControlFlowGraph& cfg() const { return *m_cfg; }

  // Build a Control Flow Graph
  //  * A non editable CFG's blocks have begin and end pointers into the big
  //    linear IRList in IRCode
  //  * An editable CFG's blocks each own a small IRList (with
  //    MethodItemEntries taken from IRCode)
  // Changes to an editable CFG are reflected in IRCode after `clear_cfg` is
  // called
  void build_cfg(bool editable = true);

  // if the cfg was editable, linearize it back into m_ir_list
  void clear_cfg();

  bool editable_cfg_built() const;

  /* Generate DexCode from IRCode */
  std::unique_ptr<DexCode> sync(const DexMethod*);

  /* Passes memory ownership of "from" to callee.  It will delete it. */
  void replace_opcode(IRInstruction* from, IRInstruction* to) {
    m_ir_list->replace_opcode(from, to);
  }

  /* Passes memory ownership of "from" to callee.  It will delete it. */
  void replace_opcode(IRInstruction* to_delete,
                      std::vector<IRInstruction*> replacements) {
    m_ir_list->replace_opcode(to_delete, replacements);
  }

  /*
   * Does exactly what it says and you SHOULD be afraid. This is mainly useful
   * to appease the compiler in various scenarios of unreachable code.
   */
  void replace_opcode_with_infinite_loop(IRInstruction* from) {
    m_ir_list->replace_opcode_with_infinite_loop(from);
  }

  /* Like replace_opcode, but both :from and :to must be branch opcodes.
   * :to will end up jumping to the same destination as :from. */
  void replace_branch(IRInstruction* from, IRInstruction* to) {
    m_ir_list->replace_branch(from, to);
  }

  template <class... Args>
  void push_back(Args&&... args) {
    m_ir_list->push_back(*(new MethodItemEntry(std::forward<Args>(args)...)));
  }

  /* Passes memory ownership of "mie" to callee. */
  void push_back(MethodItemEntry& mie) { m_ir_list->push_back(mie); }

  /*
   * Insert after instruction :position.
   *
   * position = nullptr means we insert at the head
   *
   * If position is an instruction that has a move-result-pseudo suffix, we
   * will do the insertion after the move-result-pseudo.
   */
  void insert_after(IRInstruction* position,
                    const std::vector<IRInstruction*>& opcodes) {
    m_ir_list->insert_after(position, opcodes);
  }

  IRList::iterator insert_before(const IRList::iterator& position,
                                 MethodItemEntry& mie) {
    return m_ir_list->insert_before(position, mie);
  }

  IRList::iterator insert_after(const IRList::iterator& position,
                                MethodItemEntry& mie) {
    return m_ir_list->insert_after(position, mie);
  }

  template <class... Args>
  IRList::iterator insert_before(const IRList::iterator& position,
                                 Args&&... args) {
    return m_ir_list->insert_before(
        position, *(new MethodItemEntry(std::forward<Args>(args)...)));
  }

  template <class... Args>
  IRList::iterator insert_after(const IRList::iterator& position,
                                Args&&... args) {
    always_assert(position != m_ir_list->end());
    return m_ir_list->insert_after(
        position, *(new MethodItemEntry(std::forward<Args>(args)...)));
  }

  /* DEPRECATED! Use the version below that passes in the iterator instead,
   * which is O(1) instead of O(n). */
  /* Memory ownership of "insn" passes to callee, it will delete it. */
  void remove_opcode(IRInstruction* insn) { m_ir_list->remove_opcode(insn); }

  /*
   * Remove the instruction that :it points to.
   *
   * If :it points to an instruction that has a move-result-pseudo suffix, we
   * remove both that instruction and the move-result-pseudo that follows.
   */
  void remove_opcode(const IRList::iterator& it) {
    m_ir_list->remove_opcode(it);
  }

  /*
   * Returns an estimated of the number of 2-byte code units needed to encode
   * all the instructions.
   */
  size_t sum_opcode_sizes() const { return m_ir_list->sum_opcode_sizes(); }
  size_t sum_non_internal_opcode_sizes() const {
    return m_ir_list->sum_non_internal_opcode_sizes();
  }
  size_t sum_dex_opcode_sizes() const {
    return m_ir_list->sum_dex_opcode_sizes();
  }

  /*
   * Returns the number of instructions.
   */
  size_t count_opcodes() const { return m_ir_list->count_opcodes(); }

  void sanity_check() const { m_ir_list->sanity_check(); }

  IRList::iterator begin() { return m_ir_list->begin(); }
  IRList::iterator end() { return m_ir_list->end(); }
  IRList::const_iterator begin() const { return m_ir_list->begin(); }
  IRList::const_iterator end() const { return m_ir_list->end(); }
  IRList::const_iterator cbegin() const { return m_ir_list->cbegin(); }
  IRList::const_iterator cend() const { return m_ir_list->cend(); }
  IRList::reverse_iterator rbegin() { return m_ir_list->rbegin(); }
  IRList::reverse_iterator rend() { return m_ir_list->rend(); }
  IRList::const_reverse_iterator rbegin() const { return m_ir_list->rbegin(); }
  IRList::const_reverse_iterator rend() const { return m_ir_list->rend(); }

  IRList::iterator erase(IRList::iterator it) { return m_ir_list->erase(it); }
  IRList::iterator erase_and_dispose(IRList::iterator it) {
    return m_ir_list->erase_and_dispose(it);
  }

  IRList::iterator iterator_to(MethodItemEntry& mie) {
    return m_ir_list->iterator_to(mie);
  }

  friend std::string show(const IRCode*);

  friend class MethodSplicer;
};
