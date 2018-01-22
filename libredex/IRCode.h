/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <boost/intrusive/list.hpp>
#include <boost/range/sub_range.hpp>

#include "DexClass.h"
#include "DexDebugInstruction.h"
#include "IRInstruction.h"

enum TryEntryType {
  TRY_START = 0,
  TRY_END = 1,
};

std::string show(TryEntryType t);

struct TryEntry {
  TryEntryType type;
  MethodItemEntry* catch_start;
  TryEntry(TryEntryType type, MethodItemEntry* catch_start):
      type(type), catch_start(catch_start) {
    always_assert(catch_start != nullptr);
  }
};

struct CatchEntry {
  DexType* catch_type;
  MethodItemEntry* next; // always null for catchall
  CatchEntry(DexType* catch_type): catch_type(catch_type), next(nullptr) {}
};

/**
 * A SwitchIndices represents the set of int values matching a packed switch
 * case. It could be the only value matching one case. There could also be a set
 * of values matching a switch case.
 */
using SwitchIndices = std::set<int>;

/*
 * Multi is where an opcode encodes more than
 * one branch end-point.  This is for packed
 * and sparse switch.  The index is only relevant
 * for multi-branch encodings.  The target is
 * implicit in the flow, where the target is from
 * i.e. what has to be re-written is what is recorded
 * in IRInstruction*.
 */
enum BranchTargetType {
  BRANCH_SIMPLE = 0,
  BRANCH_MULTI = 1,
};

struct MethodItemEntry;
struct BranchTarget {
  BranchTargetType type;
  MethodItemEntry* src;
  int32_t index;
  BranchTarget() = default;
  BranchTarget(MethodItemEntry* src): type(BRANCH_SIMPLE), src(src) {}
  BranchTarget(MethodItemEntry* src, int32_t index)
      : type(BRANCH_MULTI), src(src), index(index) {}
};

enum MethodItemType {
  // Begins or ends a try region. Points to the first associated catch block
  MFLOW_TRY,

  // Found at the beginning of an exception handler block. Points to the next
  // catch block (in case this one does not match)
  MFLOW_CATCH,

  // The actual instructions
  MFLOW_OPCODE,
  MFLOW_DEX_OPCODE,

  // The target of a goto, if, or switch. Also known as a "label"
  MFLOW_TARGET,

  // These hold information about the next MFLOW_(DEX_)OPCODE
  MFLOW_DEBUG,
  MFLOW_POSITION,

  // A no-op
  MFLOW_FALLTHROUGH,
};

/*
 * MethodItemEntry (and the FatMethods that it gets linked into) is a data
 * structure of DEX methods that is easier to modify than DexMethod.
 *
 * For example, when inserting a new instruction into a DexMethod, one needs
 * to recalculate branch offsets, try-catch regions, and debug info. None of
 * that is necessary when inserting into a FatMethod; it gets done when the
 * FatMethod gets translated back into a DexMethod by IRCode::sync().
 */
struct MethodItemEntry {
  boost::intrusive::list_member_hook<> list_hook_;
  MethodItemType type;

  union {
    TryEntry* tentry{nullptr};
    CatchEntry* centry;
    IRInstruction* insn;
    // dex_insn should only ever be used by the instruction lowering / output
    // code. Do NOT use it in passes!
    DexInstruction* dex_insn;
    BranchTarget* target;
    std::unique_ptr<DexDebugInstruction> dbgop;
    std::unique_ptr<DexPosition> pos;
  };
  explicit MethodItemEntry(const MethodItemEntry&);
  MethodItemEntry(DexInstruction* dex_insn) {
    this->type = MFLOW_DEX_OPCODE;
    this->dex_insn = dex_insn;
  }
  MethodItemEntry(IRInstruction* insn) {
    this->type = MFLOW_OPCODE;
    this->insn = insn;
  }
  MethodItemEntry(TryEntryType try_type, MethodItemEntry* catch_start):
    type(MFLOW_TRY), tentry(new TryEntry(try_type, catch_start)) {}
  MethodItemEntry(DexType* catch_type):
    type(MFLOW_CATCH), centry(new CatchEntry(catch_type)) {}
  MethodItemEntry(BranchTarget* bt) {
    this->type = MFLOW_TARGET;
    this->target = bt;
  }
  MethodItemEntry(std::unique_ptr<DexDebugInstruction> dbgop)
      : type(MFLOW_DEBUG), dbgop(std::move(dbgop)) {}
  MethodItemEntry(std::unique_ptr<DexPosition> pos)
      : type(MFLOW_POSITION), pos(std::move(pos)) {}

  MethodItemEntry(): type(MFLOW_FALLTHROUGH) {}
  ~MethodItemEntry();

  /*
   * This should only ever be used by the instruction lowering step. Do NOT use
   * it in passes!
   */
  void replace_ir_with_dex(DexInstruction* dex_insn) {
    always_assert(type == MFLOW_OPCODE);
    this->type = MFLOW_DEX_OPCODE;
    this->dex_insn = dex_insn;
  }

  void gather_strings(std::vector<DexString*>& lstring) const;
  void gather_types(std::vector<DexType*>& ltype) const;
  void gather_fields(std::vector<DexFieldRef*>& lfield) const;
  void gather_methods(std::vector<DexMethodRef*>& lmethod) const;

  opcode::Branchingness branchingness() const;
};

using MethodItemMemberListOption =
    boost::intrusive::member_hook<MethodItemEntry,
                                  boost::intrusive::list_member_hook<>,
                                  &MethodItemEntry::list_hook_>;

using FatMethod =
    boost::intrusive::list<MethodItemEntry, MethodItemMemberListOption>;

struct FatMethodDisposer {
  void operator()(MethodItemEntry* mie) {
    delete mie;
  }
};

std::string show(const FatMethod*);

class Block;

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

  /*
   * This method fixes the goto branches when the instruction is removed or
   * replaced by another instruction.
   */
  void remove_branch_targets(IRInstruction *branch_inst);

  FatMethod* m_fmethod;
  std::unique_ptr<ControlFlowGraph> m_cfg;

  uint16_t m_registers_size {0};
  // TODO(jezng): we shouldn't be storing / exposing the DexDebugItem... just
  // exposing the param names should be enough
  std::unique_ptr<DexDebugItem> m_dbg;

 private:
  FatMethod::iterator main_block();
  FatMethod::iterator insert(FatMethod::iterator cur, IRInstruction* insn);
  FatMethod::iterator make_if_block(FatMethod::iterator cur,
                                    IRInstruction* insn,
                                    FatMethod::iterator* if_block);
  FatMethod::iterator make_if_else_block(FatMethod::iterator cur,
                                         IRInstruction* insn,
                                         FatMethod::iterator* if_block,
                                         FatMethod::iterator* else_block);
  FatMethod::iterator make_switch_block(
      FatMethod::iterator cur,
      IRInstruction* insn,
      FatMethod::iterator* default_block,
      std::map<SwitchIndices, FatMethod::iterator>& cases);

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

  bool structural_equals(const IRCode& other);

  uint16_t get_registers_size() const { return m_registers_size; }

  void set_registers_size(uint16_t sz) { m_registers_size = sz; }

  uint16_t allocate_temp() { return m_registers_size++; }

  /*
   * Find the subrange of load-param instructions. These instructions should
   * always be at the beginning of the method.
   */
  boost::sub_range<FatMethod> get_param_instructions() const;

  void set_debug_item(std::unique_ptr<DexDebugItem> dbg) {
    m_dbg = std::move(dbg);
  }
  const DexDebugItem* get_debug_item() const { return m_dbg.get(); }
  DexDebugItem* get_debug_item() { return m_dbg.get(); }
  std::unique_ptr<DexDebugItem> release_debug_item() {
    return std::move(m_dbg);
  }

  void gather_catch_types(std::vector<DexType*>& ltype) const;
  void gather_strings(std::vector<DexString*>& lstring) const;
  void gather_types(std::vector<DexType*>& ltype) const;
  void gather_fields(std::vector<DexFieldRef*>& lfield) const;
  void gather_methods(std::vector<DexMethodRef*>& lmethod) const;

  /* Return the control flow graph of this method as a vector of blocks. */
  ControlFlowGraph& cfg() { return *m_cfg; }

  // Build a Control Flow Graph
  //  * A non editable CFG's blocks have begin and end pointers into the big
  //    linear FatMethod in IRCode
  //  * An editable CFG's blocks each own a small FatMethod (with
  //    MethodItemEntries taken from IRCode)
  // Changes to an editable CFG are reflected in IRCode after `clear_cfg` is
  // called
  void build_cfg(bool editable = false);

  // if the cfg was editable, linearize it back into m_fmethod
  void clear_cfg();

  /* Generate DexCode from IRCode */
  std::unique_ptr<DexCode> sync(const DexMethod*);

  /* Passes memory ownership of "from" to callee.  It will delete it. */
  void replace_opcode(IRInstruction* from, IRInstruction* to);

  /* Passes memory ownership of "from" to callee.  It will delete it. */
  void replace_opcode(IRInstruction* to_delete,
                      std::vector<IRInstruction*> replacements);

  /*
   * Does exactly what it says and you SHOULD be afraid. This is mainly useful
   * to appease the compiler in various scenarios of unreachable code.
   */
  void replace_opcode_with_infinite_loop(IRInstruction* from);

  /* Like replace_opcode, but both :from and :to must be branch opcodes.
   * :to will end up jumping to the same destination as :from. */
  void replace_branch(IRInstruction* from, IRInstruction* to);

  // remove all debug source code line numbers from this block
  void remove_debug_line_info(Block* block);

  template <class... Args>
  void push_back(Args&&... args) {
    m_fmethod->push_back(*(new MethodItemEntry(std::forward<Args>(args)...)));
  }

  /* Passes memory ownership of "mie" to callee. */
  void push_back(MethodItemEntry& mie) {
    m_fmethod->push_back(mie);
  }

  /*
   * Insert after instruction :position.
   *
   * position = nullptr means we insert at the head
   *
   * If position is an instruction that has a move-result-pseudo suffix, we
   * will do the insertion after the move-result-pseudo.
   */
  void insert_after(IRInstruction* position, const std::vector<IRInstruction*>& opcodes);

  FatMethod::iterator insert_before(const FatMethod::iterator& position,
                                    MethodItemEntry&);

  FatMethod::iterator insert_after(const FatMethod::iterator& position,
                                   MethodItemEntry&);

  template <class... Args>
  FatMethod::iterator insert_before(const FatMethod::iterator& position,
                                    Args&&... args) {
    return m_fmethod->insert(
        position, *(new MethodItemEntry(std::forward<Args>(args)...)));
  }

  template <class... Args>
  FatMethod::iterator insert_after(const FatMethod::iterator& position,
                                   Args&&... args) {
    always_assert(position != m_fmethod->end());
    return m_fmethod->insert(
        std::next(position),
        *(new MethodItemEntry(std::forward<Args>(args)...)));
  }

  /* DEPRECATED! Use the version below that passes in the iterator instead,
   * which is O(1) instead of O(n). */
  /* Memory ownership of "insn" passes to callee, it will delete it. */
  void remove_opcode(IRInstruction* insn);

  /*
   * Remove the instruction that :it points to.
   *
   * If :it points to an instruction that has a move-result-pseudo suffix, we
   * remove both that instruction and the move-result-pseudo that follows.
   */
  void remove_opcode(const FatMethod::iterator& it);

  /* This method will delete the switch case where insn resides. */
  void remove_switch_case(IRInstruction* insn);

  /*
   * Returns an estimated of the number of 2-byte code units needed to encode
   * all the instructions.
   */
  size_t sum_opcode_sizes() const;

  /*
   * Returns the number of instructions.
   */
  size_t count_opcodes() const;

  FatMethod::iterator begin() { return m_fmethod->begin(); }
  FatMethod::iterator end() { return m_fmethod->end(); }
  FatMethod::const_iterator begin() const { return m_fmethod->begin(); }
  FatMethod::const_iterator end() const { return m_fmethod->end(); }
  FatMethod::const_iterator cbegin() const { return m_fmethod->begin(); }
  FatMethod::const_iterator cend() const { return m_fmethod->end(); }
  FatMethod::reverse_iterator rbegin() { return m_fmethod->rbegin(); }
  FatMethod::reverse_iterator rend() { return m_fmethod->rend(); }

  FatMethod::iterator erase(FatMethod::iterator it) {
    return m_fmethod->erase(it);
  }
  FatMethod::iterator erase_and_dispose(FatMethod::iterator it) {
    return m_fmethod->erase_and_dispose(it, FatMethodDisposer());
  }

  FatMethod::iterator iterator_to(MethodItemEntry& mie) {
    return m_fmethod->iterator_to(mie);
  }

  friend std::string show(const IRCode*);

  friend class MethodSplicer;
};

class InstructionIterator {
  FatMethod::iterator m_it;
  FatMethod::iterator m_end;
  /*
   * If m_it doesn't point to an MIE of type MFLOW_OPCODE, increment it until
   * it does. Otherwise do nothing.
   */
  void to_next_instruction() {
    while (m_it != m_end && m_it->type != MFLOW_OPCODE) {
      ++m_it;
    }
  }
 public:
  InstructionIterator() {}
  explicit InstructionIterator(FatMethod::iterator it, FatMethod::iterator end)
      : m_it(it), m_end(end) {}
  InstructionIterator& operator++() {
    ++m_it;
    to_next_instruction();
    return *this;
  }
  InstructionIterator operator++(int) {
    auto rv = *this;
    ++(*this);
    return rv;
  }
  MethodItemEntry& operator*() {
    return *m_it;
  }
  MethodItemEntry* operator->() {
    return &*m_it;
  }
  bool operator==(const InstructionIterator& ii) const {
    return m_it == ii.m_it;
  }
  bool operator!=(const InstructionIterator& ii) const {
    return m_it != ii.m_it;
  }
  FatMethod::iterator unwrap() const {
    return m_it;
  }
  void reset(FatMethod::iterator it) {
    m_it = it;
    to_next_instruction();
  }

  using difference_type = long;
  using value_type = MethodItemEntry&;
  using pointer = MethodItemEntry*;
  using reference = MethodItemEntry&;
  using iterator_category = std::forward_iterator_tag;
};

class InstructionIterable {
  FatMethod::iterator m_begin;
  FatMethod::iterator m_end;
 public:
  // TODO: make this const-correct
  template <typename T>
  explicit InstructionIterable(const T& mentry_list)
      : InstructionIterable(const_cast<T&>(mentry_list)) {}
  template <typename T>
  explicit InstructionIterable(T& mentry_list)
      : m_begin(mentry_list.begin()), m_end(mentry_list.end()) {
    while (m_begin != m_end) {
      if (m_begin->type == MFLOW_OPCODE) {
        break;
      }
      ++m_begin;
    }
  }
  template <typename T>
  explicit InstructionIterable(T* mentry_list)
      : InstructionIterable(*mentry_list) {}

  InstructionIterator begin() const {
    return InstructionIterator(m_begin, m_end);
  }

  InstructionIterator end() const {
    return InstructionIterator(m_end, m_end);
  }

  bool empty() const {
    return begin() == end();
  }

  bool structural_equals(const InstructionIterable& other);
};

IRInstruction* primary_instruction_of_move_result_pseudo(
    FatMethod::iterator it);

IRInstruction* move_result_pseudo_of(FatMethod::iterator it);
