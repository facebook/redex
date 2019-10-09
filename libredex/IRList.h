/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/intrusive/list.hpp>
#include <boost/range/sub_range.hpp>
#include <type_traits>

#include "DexClass.h"
#include "DexDebugInstruction.h"
#include "IRInstruction.h"

struct MethodItemEntry;

enum TryEntryType {
  TRY_START = 0,
  TRY_END = 1,
};

std::string show(TryEntryType t);

struct TryEntry {
  TryEntryType type;
  MethodItemEntry* catch_start;
  TryEntry(TryEntryType type, MethodItemEntry* catch_start)
      : type(type), catch_start(catch_start) {
    always_assert(catch_start != nullptr);
  }
};

struct CatchEntry {
  DexType* catch_type;
  MethodItemEntry* next; // always null for catchall
  CatchEntry(DexType* catch_type) : catch_type(catch_type), next(nullptr) {}
};

/**
 * A SwitchIndices represents the set of int values matching a packed switch
 * case. It could be the only value matching one case. There could also be a set
 * of values matching a switch case.
 */
using SwitchIndices = std::set<int>;

using InstructionEquality = std::function<bool(const IRInstruction&, const IRInstruction&)>;

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

struct BranchTarget {
  BranchTargetType type;
  MethodItemEntry* src;

  // The key that a value must match to take this case in a switch statement.
  int32_t case_key;

  BranchTarget() = default;
  BranchTarget(MethodItemEntry* src) : type(BRANCH_SIMPLE), src(src) {}

  BranchTarget(MethodItemEntry* src, int32_t case_key)
      : type(BRANCH_MULTI), src(src), case_key(case_key) {}
};

/*
 * MethodItemEntry (and the IRLists that it gets linked into) is a data
 * structure of DEX methods that is easier to modify than DexMethod.
 *
 * For example, when inserting a new instruction into a DexMethod, one needs
 * to recalculate branch offsets, try-catch regions, and debug info. None of
 * that is necessary when inserting into an IRList; it gets done when the
 * IRList gets translated back into a DexMethod by IRCode::sync().
 */

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

  // These hold information about the following `MFLOW_(DEX_)OPCODE`s
  MFLOW_DEBUG,
  MFLOW_POSITION,

  // A no-op
  MFLOW_FALLTHROUGH,
};

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
  MethodItemEntry(const MethodItemEntry&);
  explicit MethodItemEntry(DexInstruction* dex_insn) {
    this->type = MFLOW_DEX_OPCODE;
    this->dex_insn = dex_insn;
  }
  explicit MethodItemEntry(IRInstruction* insn) {
    this->type = MFLOW_OPCODE;
    this->insn = insn;
  }
  MethodItemEntry(TryEntryType try_type, MethodItemEntry* catch_start)
      : type(MFLOW_TRY), tentry(new TryEntry(try_type, catch_start)) {}
  MethodItemEntry(DexType* catch_type)
      : type(MFLOW_CATCH), centry(new CatchEntry(catch_type)) {}
  MethodItemEntry(BranchTarget* bt) {
    this->type = MFLOW_TARGET;
    this->target = bt;
  }
  MethodItemEntry(std::unique_ptr<DexDebugInstruction> dbgop)
      : type(MFLOW_DEBUG), dbgop(std::move(dbgop)) {}
  MethodItemEntry(std::unique_ptr<DexPosition> pos)
      : type(MFLOW_POSITION), pos(std::move(pos)) {}

  MethodItemEntry() : type(MFLOW_FALLTHROUGH) {}
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

class MethodItemEntryCloner {
  // We need a map of MethodItemEntry we have created because a branch
  // points to another MethodItemEntry which may have been created or not
  std::unordered_map<const MethodItemEntry*, MethodItemEntry*> m_entry_map;
  // for remapping the parent position pointers
  std::unordered_map<DexPosition*, DexPosition*> m_pos_map;
  std::vector<DexPosition*> m_positions_to_fix;

 public:
  MethodItemEntryCloner();
  MethodItemEntry* clone(const MethodItemEntry* mei);

  /*
   * This should to be called after the whole method is already cloned so that
   * m_pos_map has all the positions in the method.
   *
   * Don't change any parent pointers that point to `ignore_pos`. This is used
   * for inlining because the invoke position is the parent but it isn't in the
   * callee. If you don't have any positions to ignore, nullptr is a safe
   * default.
   */
  void fix_parent_positions(const DexPosition* ignore_pos = nullptr);
};

using MethodItemMemberListOption =
    boost::intrusive::member_hook<MethodItemEntry,
                                  boost::intrusive::list_member_hook<>,
                                  &MethodItemEntry::list_hook_>;

class IRList {
 private:
  using IntrusiveList =
      boost::intrusive::list<MethodItemEntry, MethodItemMemberListOption>;

  IntrusiveList m_list;
  void remove_branch_targets(IRInstruction* branch_inst);

  static void disposer(MethodItemEntry* mie) {
    delete mie;
  }

 public:
  using iterator = IntrusiveList::iterator;
  using const_iterator = IntrusiveList::const_iterator;
  using reverse_iterator = IntrusiveList::reverse_iterator;
  using const_reverse_iterator = IntrusiveList::const_reverse_iterator;
  using difference_type = IntrusiveList::difference_type;

  IRList::iterator main_block();
  IRList::iterator make_if_block(IRList::iterator cur,
                                 IRInstruction* insn,
                                 IRList::iterator* if_block);
  IRList::iterator make_if_else_block(IRList::iterator cur,
                                      IRInstruction* insn,
                                      IRList::iterator* if_block,
                                      IRList::iterator* else_block);
  IRList::iterator make_switch_block(
      IRList::iterator cur,
      IRInstruction* insn,
      IRList::iterator* default_block,
      std::map<SwitchIndices, IRList::iterator>& cases);

  size_t size() const { return m_list.size(); }
  bool empty() const { return m_list.empty(); }

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

  /*
   * Find the subrange of load-param instructions. These instructions should
   * always be at the beginning of the method.
   */
  boost::sub_range<IRList> get_param_instructions();

  bool structural_equals(
      const IRList& other,
      const InstructionEquality& instruction_equals) const;

  /* Passes memory ownership of "mie" to callee. */
  void push_back(MethodItemEntry& mie) { m_list.push_back(mie); }

  /* Passes memory ownership of "mie" to callee. */
  void push_front(MethodItemEntry& mie) { m_list.push_front(mie); }

  /*
   * Insert after instruction :position.
   *
   * position = nullptr means we insert at the head
   *
   * If position is an instruction that has a move-result-pseudo suffix, we
   * will do the insertion after the move-result-pseudo.
   */
  void insert_after(IRInstruction* position,
                    const std::vector<IRInstruction*>& opcodes);

  IRList::iterator insert_before(const IRList::iterator& position,
                                 MethodItemEntry&);

  IRList::iterator insert_after(const IRList::iterator& position,
                                MethodItemEntry&);

  template <class... Args>
  IRList::iterator insert_before(const IRList::iterator& position,
                                 Args&&... args) {
    return insert_before(position,
                         *(new MethodItemEntry(std::forward<Args>(args)...)));
  }

  template <class... Args>
  IRList::iterator insert_after(const IRList::iterator& position,
                                Args&&... args) {
    always_assert(position != this->end());
    return insert_after(position,
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
  void remove_opcode(const IRList::iterator& it);

  /*
   * Returns an estimated of the number of 2-byte code units needed to encode
   * all the instructions.
   */
  size_t sum_opcode_sizes() const;
  size_t sum_non_internal_opcode_sizes() const;
  size_t sum_dex_opcode_sizes() const;

  /*
   * Returns the number of instructions.
   */
  size_t count_opcodes() const;

  // transfer all of `other` into `this` starting at `pos`
  // memory ownership is also transferred
  void splice(IRList::const_iterator pos, IRList& other) {
    m_list.splice(pos, other.m_list);
  }

  // transfer `other[begin]` to `other[end]` into `this` starting at `pos`
  // memory ownership is also transferred
  void splice_selection(IRList::const_iterator pos,
                        IRList& other,
                        IRList::const_iterator begin,
                        IRList::const_iterator end) {
    m_list.splice(pos, other.m_list, begin, end);
  }

  template<typename Predicate>
  void remove_and_dispose_if(Predicate predicate) {
    m_list.remove_and_dispose_if(predicate, disposer);
  }

  void sanity_check() const;

  IRList::iterator begin() { return m_list.begin(); }
  IRList::iterator end() { return m_list.end(); }
  IRList::const_iterator begin() const { return m_list.begin(); }
  IRList::const_iterator end() const { return m_list.end(); }
  IRList::const_iterator cbegin() const { return m_list.cbegin(); }
  IRList::const_iterator cend() const { return m_list.cend(); }
  IRList::reverse_iterator rbegin() { return m_list.rbegin(); }
  IRList::reverse_iterator rend() { return m_list.rend(); }
  IRList::const_reverse_iterator rbegin() const { return m_list.rbegin(); }
  IRList::const_reverse_iterator rend() const { return m_list.rend(); }

  void gather_catch_types(std::vector<DexType*>& ltype) const;
  void gather_strings(std::vector<DexString*>& lstring) const;
  void gather_types(std::vector<DexType*>& ltype) const;
  void gather_fields(std::vector<DexFieldRef*>& lfield) const;
  void gather_methods(std::vector<DexMethodRef*>& lmethod) const;

  IRList::iterator erase(IRList::iterator it) { return m_list.erase(it); }
  IRList::iterator erase_and_dispose(IRList::iterator it) {
    return m_list.erase_and_dispose(it, disposer);
  }
  void clear_and_dispose() { m_list.clear_and_dispose(disposer); }

  IRList::iterator iterator_to(MethodItemEntry& mie) {
    return m_list.iterator_to(mie);
  }

  IRList::const_iterator iterator_to(const MethodItemEntry& mie) const {
    return m_list.iterator_to(mie);
  }

  IRList::difference_type index_of(const MethodItemEntry& mie) const;

  friend std::string show(const IRCode*);
};

std::string show(const IRList*);

namespace ir_list {

template <bool is_const>
class InstructionIteratorImpl {
  using Iterator = typename std::
      conditional<is_const, IRList::const_iterator, IRList::iterator>::type;
  using Mie = typename std::
      conditional<is_const, const MethodItemEntry, MethodItemEntry>::type;

 private:

  Iterator m_it;
  Iterator m_end;
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
  using difference_type = long;
  using value_type = Mie&;
  using pointer = Mie*;
  using reference = Mie&;
  using iterator_category = std::forward_iterator_tag;

  InstructionIteratorImpl() {}
  InstructionIteratorImpl(Iterator it, Iterator end)
      : m_it(it), m_end(end) {
    to_next_instruction();
  }

  InstructionIteratorImpl& operator++() {
    ++m_it;
    to_next_instruction();
    return *this;
  }

  InstructionIteratorImpl operator++(int) {
    auto rv = *this;
    ++(*this);
    return rv;
  }

  reference operator*() const { return *m_it; }

  pointer operator->() const { return &*m_it; }

  bool operator==(const InstructionIteratorImpl& ii) const {
    return m_it == ii.m_it;
  }

  bool operator!=(const InstructionIteratorImpl& ii) const {
    return !(m_it == ii.m_it);
  }

  Iterator unwrap() const { return m_it; }

  void reset(Iterator it) {
    m_it = it;
    to_next_instruction();
  }
};

template <bool is_const>
class InstructionIterableImpl {
 protected:
  using Iterator = typename std::
      conditional<is_const, IRList::const_iterator, IRList::iterator>::type;
  Iterator m_begin;
  Iterator m_end;

  // Only callable by ConstInstructionIterable. If this were public, we may try
  // to bind const iterators to non-const members
  template <class T>
  InstructionIterableImpl(
      const T& mentry_list,
      // we add this unused parameter so we don't accidentally resolve to this
      // constructor when we meant to call the non-const version
      bool /* unused */)
      : m_begin(mentry_list.begin()), m_end(mentry_list.end()) {}

 public:
  template <class T>
  explicit InstructionIterableImpl(
      T& mentry_list)
      : m_begin(mentry_list.begin()), m_end(mentry_list.end()) {}

  template <typename T>
  explicit InstructionIterableImpl(T* mentry_list)
      : InstructionIterableImpl(*mentry_list) {}

  InstructionIteratorImpl<is_const> begin() const {
    return InstructionIteratorImpl<is_const>(m_begin, m_end);
  }

  InstructionIteratorImpl<is_const> end() const {
    return InstructionIteratorImpl<is_const>(m_end, m_end);
  }

  bool empty() const { return begin() == end(); }

  bool structural_equals(const InstructionIterableImpl& other) {
    auto it1 = this->begin();
    auto it2 = other.begin();

    for (; it1 != this->end() && it2 != other.end(); ++it1, ++it2) {
      auto& mie1 = *it1;
      auto& mie2 = *it2;

      if (*mie1.insn != *mie2.insn) {
        return false;
      }
    }

    return it1 == this->end() && it2 == other.end();
  }
};

using InstructionIterator = InstructionIteratorImpl<false>;
using ConstInstructionIterator = InstructionIteratorImpl<true>;

using InstructionIterable = InstructionIterableImpl<false>;
class ConstInstructionIterable : public InstructionIterableImpl<true> {
 public:
  // We extend the Impl so we can add the const versions of the constructors.
  // We can't have the const constructors on the non-const iterables
  template <class T>
  explicit ConstInstructionIterable(
      const T& mentry_list)
      : InstructionIterableImpl<true>(mentry_list, false) {}

  template <class T>
  explicit ConstInstructionIterable(
      const T* mentry_list)
      : ConstInstructionIterable(*mentry_list) {}
};

IRInstruction* primary_instruction_of_move_result_pseudo(IRList::iterator it);

IRInstruction* move_result_pseudo_of(IRList::iterator it);

} // namespace ir_list

template <class T>
inline ir_list::InstructionIterable InstructionIterable(T& t) {
  return ir_list::InstructionIterable(t);
}

template <class T>
inline ir_list::InstructionIterable InstructionIterable(T* t) {
  return ir_list::InstructionIterable(*t);
}

template <class T>
inline ir_list::ConstInstructionIterable InstructionIterable(const T& t) {
  return ir_list::ConstInstructionIterable(t);
}

template <class T>
inline ir_list::ConstInstructionIterable InstructionIterable(const T* t) {
  return ir_list::ConstInstructionIterable(*t);
}
