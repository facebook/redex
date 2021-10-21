/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/intrusive/list.hpp>
#include <boost/optional.hpp>
#include <boost/range/sub_range.hpp>
#include <functional>
#include <iosfwd>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Debug.h"

class DexCallSite;
class DexDebugInstruction;
class DexFieldRef;
class DexInstruction;
class DexMethodHandle;
class DexMethodRef;
struct DexPosition;
class DexString;
class DexType;
class IRCode;
class IRInstruction;
struct MethodItemEntry;

using reg_t = uint32_t;

namespace opcode {
enum Branchingness : uint8_t;
} // namespace opcode

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

  bool operator==(const TryEntry& other) const;
};

struct CatchEntry {
  DexType* catch_type;
  MethodItemEntry* next; // always null for catchall
  explicit CatchEntry(DexType* catch_type)
      : catch_type(catch_type), next(nullptr) {}

  bool operator==(const CatchEntry& other) const;
};

/**
 * A SwitchIndices represents the set of int values matching a packed switch
 * case. It could be the only value matching one case. There could also be a set
 * of values matching a switch case.
 */
using SwitchIndices = std::set<int>;

using InstructionEquality =
    std::function<bool(const IRInstruction&, const IRInstruction&)>;

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
  MethodItemEntry* src;
  BranchTargetType type;

  // The key that a value must match to take this case in a switch statement.
  int32_t case_key;

  BranchTarget() = default;
  explicit BranchTarget(MethodItemEntry* src) : src(src), type(BRANCH_SIMPLE) {}

  BranchTarget(MethodItemEntry* src, int32_t case_key)
      : src(src), type(BRANCH_MULTI), case_key(case_key) {}

  bool operator==(const BranchTarget& other) const;
};

/**
 * A SourceBlock refers to a method and block ID that the following code came
 * from. It also has a float payload at the moment (though that is in flow),
 * which will be used for profiling information.
 */
struct SourceBlock {
  DexMethodRef* src{nullptr};
  std::unique_ptr<SourceBlock> next;
  // Large methods exist, but a 32-bit integer is safe.
  uint32_t id{0};
  // Float has enough precision.
  class Val {
    static constexpr float kNoneVal = std::numeric_limits<float>::quiet_NaN();

   public:
    constexpr Val(float v, float a) noexcept : m_val({v, a}) {}

    static constexpr Val none() { return Val(kNoneVal, kNoneVal); }

    // NOLINTNEXTLINE
    /* implicit */ operator bool() const { return m_val.val == m_val.val; };

    bool operator==(const Val& other) const {
      return (m_val.val == other.m_val.val &&
              m_val.appear100 == other.m_val.appear100) ||
             (m_val.val != m_val.val && other.m_val.val != other.m_val.val);
    }

    // To access like an `optional`.

    struct ValPair {
      float val;
      float appear100;
    };

    ValPair& operator*() {
      redex_assert(operator bool());
      return m_val;
    }
    const ValPair& operator*() const {
      redex_assert(operator bool());
      return m_val;
    }

    ValPair* operator->() {
      redex_assert(operator bool());
      return &m_val;
    }
    const ValPair* operator->() const {
      redex_assert(operator bool());
      return &m_val;
    }

   private:
    ValPair m_val;
  };
  std::vector<Val> vals;

  SourceBlock() = default;
  SourceBlock(DexMethodRef* src, size_t id) : src(src), id(id) {}
  SourceBlock(DexMethodRef* src, size_t id, std::vector<Val> v)
      : src(src), id(id), vals(std::move(v)) {}
  SourceBlock(const SourceBlock& other)
      : src(other.src),
        next(other.next == nullptr ? nullptr : new SourceBlock(*other.next)),
        id(other.id),
        vals(other.vals) {}

  boost::optional<float> get_val(size_t i) const {
    return vals[i] ? boost::optional<float>(vals[i]->val) : boost::none;
  }
  boost::optional<float> get_appear100(size_t i) const {
    return vals[i] ? boost::optional<float>(vals[i]->appear100) : boost::none;
  }

  template <typename Fn>
  void foreach_val(const Fn& fn) const {
    for (const auto& val : vals) {
      fn(val);
    }
  }

  template <typename Fn>
  bool foreach_val_early(const Fn& fn) const {
    for (const auto& val : vals) {
      if (fn(val)) {
        return true;
      }
    }
    return false;
  }

  bool operator==(const SourceBlock& other) const {
    return src == other.src && id == other.id && vals == other.vals;
  }

  void append(std::unique_ptr<SourceBlock> sb) {
    SourceBlock* last = this;
    while (last->next != nullptr) {
      last = last->next.get();
    }
    last->next = std::move(sb);
  }
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

  // This holds information about the source block.
  MFLOW_SOURCE_BLOCK,

  // A no-op
  MFLOW_FALLTHROUGH,
};

std::ostream& operator<<(std::ostream&, const MethodItemType& type);

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
    std::unique_ptr<SourceBlock> src_block;
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
  explicit MethodItemEntry(DexType* catch_type)
      : type(MFLOW_CATCH), centry(new CatchEntry(catch_type)) {}
  explicit MethodItemEntry(BranchTarget* bt) {
    this->type = MFLOW_TARGET;
    this->target = bt;
  }
  explicit MethodItemEntry(std::unique_ptr<DexDebugInstruction> dbgop);
  explicit MethodItemEntry(std::unique_ptr<DexPosition> pos);
  explicit MethodItemEntry(std::unique_ptr<SourceBlock> src_block);

  bool operator==(const MethodItemEntry&) const;

  bool operator!=(const MethodItemEntry& that) const {
    return !(*this == that);
  }

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

  void gather_strings(std::vector<const DexString*>& lstring) const;
  void gather_types(std::vector<DexType*>& ltype) const;
  void gather_fields(std::vector<DexFieldRef*>& lfield) const;
  void gather_methods(std::vector<DexMethodRef*>& lmethod) const;
  void gather_callsites(std::vector<DexCallSite*>& lcallsite) const;
  void gather_methodhandles(std::vector<DexMethodHandle*>& lmethodhandle) const;

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
  MethodItemEntry* clone(const MethodItemEntry* mie);

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

  static void disposer(MethodItemEntry* mie) { delete mie; }

 public:
  using iterator = IntrusiveList::iterator;
  using const_iterator = IntrusiveList::const_iterator;
  using reverse_iterator = IntrusiveList::reverse_iterator;
  using const_reverse_iterator = IntrusiveList::const_reverse_iterator;
  using difference_type = IntrusiveList::difference_type;

  IRList::iterator main_block();
  IRList::iterator make_if_block(const IRList::iterator& cur,
                                 IRInstruction* insn,
                                 IRList::iterator* false_block);
  IRList::iterator make_if_else_block(const IRList::iterator& cur,
                                      IRInstruction* insn,
                                      IRList::iterator* false_block,
                                      IRList::iterator* true_block);
  IRList::iterator make_switch_block(
      const IRList::iterator& cur,
      IRInstruction* insn,
      IRList::iterator* default_block,
      std::map<SwitchIndices, IRList::iterator>& cases);

  size_t size() const { return m_list.size(); }
  bool empty() const { return m_list.empty(); }

  /*
   * Removes a subset of MFLOW_DEBUG instructions.
   */
  void cleanup_debug();

  /*
   * Removes a subset of MFLOW_DEBUG instructions. valid_regs
   * is an accumulator set of registers used by either DBG_START_LOCAL
   * or DBG_START_LOCAL_EXTENDED. The DBG_END_LOCAL and DBG_RESTART_LOCAL
   * instructions are erased, unless valid_regs contains the registers they use.
   */
  void cleanup_debug(std::unordered_set<reg_t>& valid_regs);

  /* Passes memory ownership of "from" to callee.  It will delete it. */
  void replace_opcode(IRInstruction* from, IRInstruction* to);

  /* Passes memory ownership of "from" to callee.  It will delete it. */
  void replace_opcode(IRInstruction* to_delete,
                      const std::vector<IRInstruction*>& replacements);

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

  bool structural_equals(const IRList& other,
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

  template <typename Predicate>
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
  void gather_strings(std::vector<const DexString*>& lstring) const;
  void gather_types(std::vector<DexType*>& ltype) const;
  void gather_fields(std::vector<DexFieldRef*>& lfield) const;
  void gather_methods(std::vector<DexMethodRef*>& lmethod) const;
  void gather_callsites(std::vector<DexCallSite*>& lcallsite) const;
  void gather_methodhandles(std::vector<DexMethodHandle*>& lmethodhandle) const;

  IRList::iterator erase(const IRList::iterator& it) {
    return m_list.erase(it);
  }
  IRList::iterator erase_and_dispose(const IRList::iterator& it) {
    return m_list.erase_and_dispose(it, disposer);
  }
  void clear_and_dispose() { m_list.clear_and_dispose(disposer); }

  IRList::iterator iterator_to(MethodItemEntry& mie) {
    return m_list.iterator_to(mie);
  }

  IRList::const_iterator iterator_to(const MethodItemEntry& mie) const {
    return m_list.iterator_to(mie);
  }

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
  /*
   * If m_it doesn't point to an MIE of type MFLOW_OPCODE, decrement it until
   * it does. Otherwise do nothing.
   */
  void to_prev_instruction() {
    while (m_it->type != MFLOW_OPCODE) {
      --m_it;
    }
  }

 public:
  using difference_type = long;
  using value_type = Mie&;
  using pointer = Mie*;
  using reference = Mie&;
  using iterator_category = std::bidirectional_iterator_tag;

  InstructionIteratorImpl() {}
  InstructionIteratorImpl(Iterator it, Iterator end) : m_it(it), m_end(end) {
    to_next_instruction();
  }

  InstructionIteratorImpl(const InstructionIteratorImpl<false>& rhs)
      : m_it(rhs.m_it), m_end(rhs.m_end) {}

  InstructionIteratorImpl& operator=(const InstructionIteratorImpl& other) =
      default;

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

  InstructionIteratorImpl& operator--() {
    --m_it;
    to_prev_instruction();
    return *this;
  }

  InstructionIteratorImpl operator--(int) {
    auto rv = *this;
    --(*this);
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

  template <bool kConst>
  friend class InstructionIteratorImpl;
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
  InstructionIterableImpl(const T& mentry_list,
                          // we add this unused parameter so we don't
                          // accidentally resolve to this constructor when we
                          // meant to call the non-const version
                          bool /* unused */)
      : m_begin(mentry_list.begin()), m_end(mentry_list.end()) {}

 public:
  template <class T>
  explicit InstructionIterableImpl(T& mentry_list)
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
};

using InstructionIterator = InstructionIteratorImpl<false>;
using ConstInstructionIterator = InstructionIteratorImpl<true>;

using InstructionIterable = InstructionIterableImpl<false>;
class ConstInstructionIterable : public InstructionIterableImpl<true> {
 public:
  // We extend the Impl so we can add the const versions of the constructors.
  // We can't have the const constructors on the non-const iterables
  template <class T>
  explicit ConstInstructionIterable(const T& mentry_list)
      : InstructionIterableImpl<true>(mentry_list, false) {}

  template <class T>
  explicit ConstInstructionIterable(const T* mentry_list)
      : ConstInstructionIterable(*mentry_list) {}
};

IRInstruction* primary_instruction_of_move_result_pseudo(IRList::iterator it);

IRInstruction* primary_instruction_of_move_result(IRList::iterator it);

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
