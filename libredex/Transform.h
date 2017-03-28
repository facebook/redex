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
#include <unordered_map>
#include <unordered_set>

#include <boost/intrusive/list.hpp>

#include "DexClass.h"
#include "DexDebugInstruction.h"
#include "IRInstruction.h"
#include "Liveness.h"
#include "Pass.h"

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
};

enum MethodItemType {
  MFLOW_TRY,
  MFLOW_CATCH,
  MFLOW_OPCODE,
  MFLOW_TARGET,
  MFLOW_DEBUG,
  MFLOW_POSITION,

  /*
   * We insert one MFLOW_FALLTHROUGH before every MFLOW_OPCODE that
   * could potentially throw, and set the throwing_mie field to point to that
   * opcode. The MFLOW_FALLTHROUGH will then be at the end of its basic block
   * and the MFLOW_OPCODE will be at the start of the next one. build_cfg
   * will treat the MFLOW_FALLTHROUGH as potentially throwing and add edges
   * from its BB to the BB of any catch handler, but it will treat the
   * MFLOW_OPCODE is non-throwing. This is desirable for dataflow analysis since
   * we do not want to e.g. consider a register to be defined if the opcode
   * that is supposed to define it ends up throwing an exception. E.g. suppose
   * we have the opcodes
   *
   *   const v0, 123
   *   new-array v0, v1
   *
   * If new-array throws an OutOfMemoryError and control flow jumps to some
   * handler within the same method, then v0 will still contain the value 123
   * instead of an array reference. So we want the control flow edge to be
   * placed before the new-array instruction. Placing that edge right at the
   * const instruction would be strange -- `const` doesn't throw -- so we
   * insert the MFLOW_FALLTHROUGH entry to make it clearer.
   */
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
  uint32_t addr;
  union {
    TryEntry* tentry;
    CatchEntry* centry;
    IRInstruction* insn;
    BranchTarget* target;
    std::unique_ptr<DexDebugInstruction> dbgop;
    std::unique_ptr<DexPosition> pos;
    MethodItemEntry* throwing_mie;
  };
  explicit MethodItemEntry(const MethodItemEntry&);
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

  MethodItemEntry(): type(MFLOW_FALLTHROUGH), throwing_mie(nullptr) {}
  static MethodItemEntry* make_throwing_fallthrough(
      MethodItemEntry* throwing_mie) {
    auto ret = new MethodItemEntry();
    ret->throwing_mie = throwing_mie;
    return ret;
  }

  ~MethodItemEntry();

  void gather_strings(std::vector<DexString*>& lstring) const;
  void gather_types(std::vector<DexType*>& ltype) const;
  void gather_fields(std::vector<DexField*>& lfield) const;
  void gather_methods(std::vector<DexMethod*>& lmethod) const;
};

using MethodItemMemberListOption =
    boost::intrusive::member_hook<MethodItemEntry,
                                  boost::intrusive::list_member_hook<>,
                                  &MethodItemEntry::list_hook_>;

using FatMethod =
    boost::intrusive::list<MethodItemEntry, MethodItemMemberListOption>;

struct FatMethodDisposer {
  void operator()(MethodItemEntry* mei) {
    delete mei;
  }
};

std::string show(const FatMethod*);

class InlineContext;

namespace {
  typedef std::unordered_map<uint32_t, MethodItemEntry*> addr_mei_t;
}

class MethodSplicer;

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
  void remove_branch_target(IRInstruction *branch_inst);

  void clear_cfg();

  FatMethod* m_fmethod;
  // mapping from fill-array-data opcodes to the pseudo opcodes containing the
  // array contents
  std::unordered_map<IRInstruction*, DexOpcodeData*> m_array_data;
  std::unique_ptr<ControlFlowGraph> m_cfg;

  uint16_t m_registers_size {0};
  uint16_t m_ins_size {0};
  // TODO calculate this automatically when syncing
  uint16_t m_outs_size {0};
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
      std::map<int, FatMethod::iterator>& cases);

  friend struct MethodCreator;

 public:
  explicit IRCode();
  explicit IRCode(DexMethod*);

  ~IRCode();

  uint16_t get_registers_size() const { return m_registers_size; }
  uint16_t get_ins_size() const { return m_ins_size; }
  uint16_t get_outs_size() const { return m_outs_size; }

  void set_registers_size(uint16_t sz) { m_registers_size = sz; }
  void set_ins_size(uint16_t sz) { m_ins_size = sz; }
  void set_outs_size(uint16_t sz) { m_outs_size = sz; }

  const DexDebugItem* get_debug_item() const { return m_dbg.get(); }
  DexDebugItem* get_debug_item() { return m_dbg.get(); }
  std::unique_ptr<DexDebugItem> release_debug_item() {
    return std::move(m_dbg);
  }

  void gather_catch_types(std::vector<DexType*>& ltype) const;
  void gather_strings(std::vector<DexString*>& lstring) const;
  void gather_types(std::vector<DexType*>& ltype) const;
  void gather_fields(std::vector<DexField*>& lfield) const;
  void gather_methods(std::vector<DexMethod*>& lmethod) const;

  /* Create a FatMethod from a DexMethod. FatMethods are easier to manipulate.
   * E.g. they don't require manual updating of address offsets, and they don't
   * contain pseudo-opcodes. */
  FatMethod* balloon(DexMethod*);

  /*
   * Inline tail-called `callee` into `caller` at instruction `invoke`.
   *
   * NB: This is NOT a general-purpose inliner; it assumes that the caller does
   * not do any work after the call, so the only live registers are the
   * parameters to the callee.
   */
  static void inline_tail_call(DexMethod* caller,
                               DexMethod* callee,
                               IRInstruction* invoke);

  static bool inline_16regs(
      InlineContext& context,
      DexMethod *callee,
      IRMethodInstruction *invoke);

  /*
   * Simple register allocator.
   * Example:
   * - before: 4 registers, 2 ins -> [v0, v1, p0, p1]
   * - after: 7 registers -> [v0, v1, v2, v3, v4, p0, p1] where v2 -> v4 are new.
   */
  static void enlarge_regs(DexMethod* method, uint16_t newregs);

  /* Return the control flow graph of this method as a vector of blocks. */
  ControlFlowGraph& cfg() { return *m_cfg; }

  /*
   * If end_block_before_throw is false, opcodes that may throw (e.g. invokes,
   * {get|put}-object, etc) will terminate their basic blocks. If it is true,
   * they will instead be at the start of the next basic block. As of right
   * now the only pass that uses the `false` behavior is SimpleInline, and I
   * would like to remove it eventually.
   */
  void build_cfg(bool end_block_before_throw = true);

  /* Generate DexCode from IRCode */
  std::unique_ptr<DexCode> sync(const DexMethod*);

  /* Passes memory ownership of "from" to callee.  It will delete it. */
  void replace_opcode(IRInstruction* from, IRInstruction* to);

  /*
   * Does exactly what it says and you SHOULD be afraid. This is mainly useful
   * to appease the compiler in various scenarios of unreachable code.
   */
  void replace_opcode_with_infinite_loop(IRInstruction* from);

  /* Like replace_opcode, but both :from and :to must be branch opcodes.
   * :to will end up jumping to the same destination as :from. */
  void replace_branch(IRInstruction* from, IRInstruction* to);

  template <class... Args>
  void push_back(Args&&... args) {
    m_fmethod->push_back(*(new MethodItemEntry(std::forward<Args>(args)...)));
  }

  /* Passes memory ownership of "mie" to callee. */
  void push_back(MethodItemEntry& mie) {
    m_fmethod->push_back(mie);
  }

  /* position = nullptr means at the head */
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

  void remove_opcode(const FatMethod::iterator&);

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
  FatMethod::iterator erase(FatMethod::iterator it) {
    return m_fmethod->erase(it);
  }
  friend std::string show(const IRCode*);

  friend class MethodSplicer;
};

/**
 * Carry context for multiple inline into a single caller.
 * In particular, it caches the liveness analysis so that we can reuse it when
 * multiple callees into the same caller.
 */
class InlineContext {
  std::unique_ptr<LivenessMap> m_liveness;
 public:
  uint64_t estimated_insn_size {0};
  uint16_t original_regs;
  IRCode* caller_code;
  InlineContext(DexMethod* caller, bool use_liveness);
  Liveness live_out(IRInstruction*);
};

class InstructionIterator {
  FatMethod::iterator m_it;
  FatMethod::iterator m_end;
 public:
  explicit InstructionIterator(FatMethod::iterator it, FatMethod::iterator end)
      : m_it(it), m_end(end) {}
  InstructionIterator& operator++() {
    ++m_it;
    if (m_it != m_end) {
      while (m_it->type != MFLOW_OPCODE && ++m_it != m_end);
    }
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
  explicit InstructionIterable(const IRCode* ir_code)
      : InstructionIterable(const_cast<IRCode*>(ir_code)) {}
  explicit InstructionIterable(IRCode* mt)
      : m_begin(mt->begin()), m_end(mt->end()) {
    while (m_begin != m_end) {
      if (m_begin->type == MFLOW_OPCODE) {
        break;
      }
      ++m_begin;
    }
  }

  InstructionIterator begin() {
    return InstructionIterator(m_begin, m_end);
  }

  InstructionIterator end() {
    return InstructionIterator(m_end, m_end);
  }
};
