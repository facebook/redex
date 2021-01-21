/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "IRInstruction.h"

#include <unordered_map>
#include <unordered_set>

#include <boost/functional/hash.hpp>

namespace cfg {
class Block;
} // namespace cfg

/**
 * This analysis identifies class initializations descended from a base type
 * and tracks their uses across a method identifying per method created in:
 *   Writes to the tracked object's fields,
 *   Reads of the tracked object's fields,
 *   Calls of the tracked object's methods,
 *   Locations and means where the object escapes the scope of the method
 *      whether via return statements
 *      as writes to another object's fields
 *      or as parameters to another method (static vs virtual)
 *   Escapes can be optionally deemed safe via a set of specified fields/methods
 *   Only methods are presently supported
 *
 * To perform this analysis, we have a domain of Tracked values where we have
 * following lattice:
 *                           bottom(nullptr)
 *                          /              \
 *                   NullableTracked    ObjectUses
 *                          \              /
 *                             MergedUses
 *
 * ObjectUses models values created by a unique instruction
 * NullableTracked models values that are null but of type Tracked
 *      this is only discoverable of a value during analysis
 * MergedUses models values created by a non-empty set of instructions
 * Top is modeled by MergedUses with a complete set of instructions that
 *   create Tracked values from the program
 *

 * The analysis further computes whether creation or use happens conditionally
 * However, as this is following a may-use analysis at the moment it is
 * conservative in selection Conditional
 */

// Describe the set of containers used in tracking object uses of a type.
namespace cic {

enum FlowStatus {
  Conditional,
  AllPaths,
};

enum SourceStatus {
  OneReg,
  MultipleReg,
  Unclear,
};

// Todo: switch to a pair of register and instruction
struct FieldSet {
  std::unordered_map<reg_t, std::unordered_set<IRInstruction*>> regs;
  FlowStatus set;
  SourceStatus source;
};

struct MethodCall {
  FlowStatus call;
  std::unordered_set<std::pair<IRInstruction*, reg_t>,
                     boost::hash<std::pair<IRInstruction*, reg_t>>>
      call_sites;
};

/*
 * For all data tracking classes, there are two methods used to combine data
 * Consider a CFG with three blocks, 0 1 and 2. 1 and 2 are the successors to 0
 * combine_paths joins data from different control flow paths
 *    so if block 1 sets field A on the tracked object and block 2 does not
 *    then combine_paths sets that field A is conditionally set
 * merge joins data from all successor blocks
 *    so data from block 0 is merged with the data from blocks 1 and 2
 *    if block 0 did not set field A, then field A is conditionally set
 *    if block 0 does set field A, then field A is set on all paths, potentially
 *    with multiple sources.
 * consistent_with checks whether results of evaluating a basic block in
 *   this domain could produce a different outcome, so null vs Object is not
 *   consistent, but Object(i) consistent_with Merged({i, i'})
 */

using FieldSetMap = std::unordered_map<DexFieldRef*, FieldSet>;
using FieldReadMap = std::unordered_map<DexFieldRef*, FlowStatus>;
using CallMap = std::unordered_map<DexMethodRef*, MethodCall>;
using ArrayWriteMap = std::unordered_map<IRInstruction*, FlowStatus>;

// Tracks a field write either to or using a tracked value
class FieldWriteRegs final {
 public:
  void add_field(DexFieldRef* field, reg_t reg, IRInstruction* instr);
  const FieldSetMap& get_fields() const { return m_fields; }

  bool consistent_with(const FieldWriteRegs& other);
  void combine_paths(const FieldWriteRegs& other);
  void merge(const FieldWriteRegs& other);

 private:
  FieldSetMap m_fields;
};

// Tracks the fields that are read of a tracked object
class FieldReads final {
 public:
  void add_field(DexFieldRef* field);
  const FieldReadMap& get_fields() const { return m_fields; }

  bool consistent_with(const FieldReads& other);
  void combine_paths(const FieldReads& other);
  void merge(const FieldReads& other);

 private:
  FieldReadMap m_fields;
};

// Tracks the method calls made on/with a tracked object
class MethodCalls final {
 public:
  void add_call(DexMethodRef* method, reg_t in_reg, IRInstruction* instr);
  bool consistent_with(const MethodCalls& other);
  void combine_paths(const MethodCalls& other);
  void merge(const MethodCalls& other);

  const CallMap& get_calls() const { return m_calls; }

 private:
  CallMap m_calls;
};

// Tracks the different ways an object escapes the current method
class Escapes final {
 public:
  void add_return(IRInstruction* instr);
  void add_array(IRInstruction* instr);
  void add_field_set(DexFieldRef* field, reg_t reg, IRInstruction* instr);
  void add_dmethod(DexMethodRef* method, reg_t object, IRInstruction* instr);
  void add_smethod(DexMethodRef* method, reg_t object, IRInstruction* instr);

  bool consistent_with(const Escapes& other);
  void combine_paths(const Escapes& other);
  void merge(const Escapes& other);

  boost::optional<FlowStatus> via_return = {};
  std::vector<std::pair<IRInstruction*, reg_t>> get_escape_instructions() const;

  std::unordered_set<IRInstruction*> return_instrs;
  ArrayWriteMap via_array_write;
  FieldSetMap via_field_set;
  CallMap via_vmethod_call;
  CallMap via_smethod_call;
};

// TrackedUses is the domain for the abstract interpretation, where each object
// should be stored in a shared pointer, as they can have multiple owners.

class ObjectUses;
class MergedUses;

// This enum permits differentiating ObjectUses and MergedUses
// without runtime casts (not supported?)
enum Tracked {
  Object,
  Merged,
};

// This structure captures IRInstruction identity that persists across
// builds, using the block ID and instruction order within a block as
// these are consistent across builds.
// Note, they are not necessarily consistent across optimization order
struct InstructionPOIdentity {
  IRInstruction* insn;
  uint32_t block_id;
  uint32_t instruction_count;
  InstructionPOIdentity(IRInstruction* insn, uint32_t b_id, uint32_t i_count)
      : insn(insn), block_id(b_id), instruction_count(i_count) {}
  inline bool operator==(const InstructionPOIdentity& other) const {
    return block_id == other.block_id &&
           instruction_count == other.instruction_count;
  }
};

inline bool operator<(const InstructionPOIdentity& l,
                      const InstructionPOIdentity& r) {
  return l.block_id < r.block_id || (l.block_id == r.block_id &&
                                     l.instruction_count < r.instruction_count);
}

class InstructionPOComparer {
 public:
  bool operator()(const std::shared_ptr<InstructionPOIdentity>& l,
                  const std::shared_ptr<InstructionPOIdentity>& r) const {
    return *l < *r;
  }
};

// TrackedUses is the 'abstract' parent class of the domain representation
class TrackedUses {
 public:
  explicit TrackedUses(Tracked kind);
  virtual ~TrackedUses();

  /*
   * combine_path joins data from different control flow paths
   */
  void combine_paths(const TrackedUses& other);

  /*
   * merge joins data from successor block(s), combined with combine path, to PO
   * earlier blocks
   */
  void merge(const TrackedUses& other);
  /*
   * consistent_with checks if this tracked use can be used in place of other,
   * So ObjectUse(i) is consistent with Merged({i, i'}), but not with
   * ObjectUse(i')
   */
  virtual bool consistent_with(const TrackedUses& other) = 0;
  virtual size_t hash() const = 0;

  const Tracked m_tracked_kind;

  MethodCalls method_calls;
  FieldWriteRegs fields_set;
  FieldReads fields_read;
  Escapes escapes;
  Escapes safe_escapes;
};

class ObjectUses : public TrackedUses {
  // m_id is the instruction creating the Tracked, of class m_class_used
 public:
  explicit ObjectUses(DexType* typ,
                      IRInstruction* instr,
                      uint32_t t_block_id,
                      uint32_t t_instruction_count)
      : TrackedUses(Object),
        m_ir(std::make_shared<InstructionPOIdentity>(
            instr, t_block_id, t_instruction_count)),
        m_class_used(typ) {}

  void combine_paths(const TrackedUses& other);
  void merge(const TrackedUses& other);
  bool consistent_with(const TrackedUses& other) override;

  bool equal(const ObjectUses& other) const { return m_ir == other.m_ir; }
  bool less(const ObjectUses& other) const { return m_ir < other.m_ir; }

  size_t hash() const override { return m_ir->insn->hash(); }
  IRInstruction* get_instr() const { return m_ir->insn; }
  std::shared_ptr<InstructionPOIdentity> get_po_identity() const {
    return m_ir;
  }
  DexType* get_represents_typ() const { return m_class_used; }

  FlowStatus created_flow = AllPaths;

 private:
  std::shared_ptr<InstructionPOIdentity> m_ir;
  DexType* m_class_used;
};

class MergedUses : public TrackedUses {
  // m_instrs is the set of Instructions that created tracked objects
  // m_classes is the set of types, this can be smaller than m_instrs
  // m_includes_nullable indicates if the program could encounter null
  // in the same register
 public:
  MergedUses(const ObjectUses&, const ObjectUses&);
  // Creates a merged object where nullable is true
  explicit MergedUses(const ObjectUses&);

  void combine_paths(const TrackedUses& other);
  void merge(const TrackedUses& other);
  bool consistent_with(const TrackedUses& other) override;
  size_t hash() const override;
  bool equal(const MergedUses& other) const;
  bool less(const MergedUses& other) const;
  void set_is_nullable() { m_includes_nullable = true; }

  const std::unordered_set<IRInstruction*>& get_instrs() const;
  bool contains_instr(const std::shared_ptr<InstructionPOIdentity>&) const;
  const std::unordered_set<DexType*>& get_classes() const { return m_classes; }
  bool contains_type(DexType*) const;
  bool is_nullable() const { return m_includes_nullable; }

 private:
  std::set<std::shared_ptr<InstructionPOIdentity>, InstructionPOComparer>
      m_instrs;
  std::unordered_set<DexType*> m_classes;
  bool m_includes_nullable = false;
};

class TrackedHasher {
 public:
  size_t operator()(const std::shared_ptr<TrackedUses>& o) const {
    return o->hash();
  }
};

class TrackedComparer {
 public:
  bool operator()(const std::shared_ptr<TrackedUses>& l,
                  const std::shared_ptr<TrackedUses>& r) const {
    if (!l || !r) {
      return false;
    }
    // We decree a non-merged object as < a merged one
    if (l->m_tracked_kind != r->m_tracked_kind) {
      return l->m_tracked_kind == Object;
    }
    if (l->m_tracked_kind == Merged) {
      return reinterpret_cast<MergedUses*>(&*l)->less(
          reinterpret_cast<MergedUses&>(*r));
    } else {
      // Has to be ObjectUses
      return reinterpret_cast<ObjectUses*>(&*l)->less(
          reinterpret_cast<ObjectUses&>(*r));
    }
  }
};

using ObjectUsedSet = std::set<std::shared_ptr<ObjectUses>, TrackedComparer>;

using MergedUsedSet = std::set<std::shared_ptr<MergedUses>, TrackedComparer>;

using UsedSet = std::set<std::shared_ptr<TrackedUses>, TrackedComparer>;

// Represents the registers across a method and a set of all Uses encountered
// during the execution, so that over writing a tracked value does not cause us
// to lose track of it for analysis of all potential uses
class RegisterSet {
 public:
  RegisterSet() {}
  RegisterSet(RegisterSet const&);
  RegisterSet(RegisterSet&&) noexcept = default;

  RegisterSet& operator=(const RegisterSet&) = default;
  RegisterSet& operator=(RegisterSet&&) = default;

  // Place Tracked value into register i, remember use
  void insert(reg_t i, const std::shared_ptr<TrackedUses>& uses) {
    m_all_uses.insert(uses);
    m_registers[i] = uses;
  }

  // Set register i back to bottom
  void clear(reg_t i) {
    if (m_registers.count(i) != 0) {
      m_registers[i] = std::shared_ptr<TrackedUses>(nullptr);
    }
  }

  // Extract value for register i or bottom
  std::shared_ptr<TrackedUses> get(reg_t i) const {
    auto val = m_registers.find(i);
    if (val == m_registers.end()) {
      return std::shared_ptr<TrackedUses>(nullptr);
    } else {
      return val->second;
    }
  }

  // Is the value at register i bottom
  bool is_empty(reg_t i) {
    auto value = m_registers.find(i);
    return value == m_registers.end() || !(bool)value->second;
  }

  // Determines if all of the TrackedUses in m_registers of both RegisterSets
  // would produce the same result (i.e. have TrackedUses that are consistent
  // in all non-empty registers for both).
  bool consistent_with(const RegisterSet& other);

  // Equality check on both m_all_uses
  bool same_uses(const RegisterSet& other);

  // Join m_all_uses from different control flow paths,
  void combine_paths(const RegisterSet& other);

  // Turn this m_registers into a register set that is consistent with Other,
  // Potentially lifting ObjectUses into MergedUses, and expanding existing
  // MergedUses to more cover more ObjectUses.
  // Any newly created MergedUses are stored globally
  void merge_registers(const RegisterSet& comes_after,
                       MergedUsedSet& merge_store);

  // Merge m_all_uses from successor(s) to the current, PO earlier uses
  void merge_effects(const RegisterSet& other);

  UsedSet m_all_uses;
  std::unordered_map<reg_t, std::shared_ptr<TrackedUses>> m_registers;
};

/**
 * InitLocation is used within ClassInitCounter to identify and track usage
 * data on where a class is constructed and how the object is subsequently used
 */
class InitLocation final {
  using InitMap = std::unordered_map<
      DexClass*,
      std::unordered_map<
          DexMethod*,
          std::unordered_map<IRInstruction*,
                             std::vector<std::shared_ptr<ObjectUses>>>>>;

 public:
  explicit InitLocation(DexType* typ) : m_typ(typ) {}
  InitLocation() = default;
  uint32_t get_count() const { return m_count; }

  // adds the data structure for this initialization, returning a ref to it
  std::shared_ptr<ObjectUses> add_init(DexClass* container,
                                       DexMethod* caller,
                                       IRInstruction* instr,
                                       uint32_t block_id,
                                       uint32_t instruction_count);
  void update_object(DexClass* container,
                     DexMethod* caller,
                     const ObjectUses& obj);
  const InitMap& get_inits() const { return m_inits; }
  InitMap& get_inits() { return m_inits; }

  // Puts all uses from cls.method into provided set
  void all_uses_from(DexClass* cls,
                     DexMethod* method,
                     ObjectUsedSet& set) const;

  // If this init has data from this `method`, reset to empty.
  // This ensures when a method is going to be re-analyzed that all data
  // is accurate.
  void reset_uses_from(DexClass* cls, DexMethod* method);

  DexType* m_typ = nullptr;

 private:
  InitMap m_inits;
  uint32_t m_count = 0;
};

struct RegistersPerBlock {
  RegisterSet input_registers;
  RegisterSet basic_block_registers;
  boost::optional<RegisterSet> final_result_registers;
};

class ClassInitCounter final {
 public:
  using TypeToInit = std::unordered_map<DexType*, InitLocation>;
  using MergedUsesMap =
      std::unordered_map<DexType*,
                         std::unordered_map<DexMethod*, MergedUsedSet>>;

  ClassInitCounter(
      DexType* parent_class,
      const std::unordered_set<DexMethodRef*>& safe_escapes,
      const std::unordered_set<DexClass*>& classes,
      boost::optional<DexString*> optional_method_name = boost::none);

  const TypeToInit& type_to_inits() const { return m_type_to_inits; }
  const MergedUsesMap& merged_uses() const { return m_stored_mergeds; }

  // Run analysis only for a use at one instruction being tracked
  std::pair<ObjectUsedSet, MergedUsedSet> find_uses_of(IRInstruction* origin,
                                                       DexType* typ,
                                                       DexMethod* method);

  // Calculate the uses for the specified method.
  // If this method has already been analyzed, discard that analysis result
  // and build fresh data.
  void find_uses_within(DexClass* container, DexMethod* method);

  // Reports all object uses and merged uses within the specified method.
  std::pair<ObjectUsedSet, MergedUsedSet> all_uses_from(DexType*, DexMethod*);

  // For debugging
  std::string debug_show_table();

 private:
  void drive_analysis(
      // Class of the method to be analyzed, key for storing data structure
      DexClass* container,
      // Method that will be analyzed, by extracting code and CFG,
      // type_class(method->get_type()) == container
      DexMethod* method,
      // Name of the calling public method, for trace reports
      const std::string& analysis,
      // Potentially empty set of instructions to start tracking from
      // If empty, tracks all new_instance or method calls as set in ctor
      const std::unordered_set<IRInstruction*>& tracking,
      // Reference to data structure to store results in
      TypeToInit& type_to_init);

  // Identifies and stores in type_to_inits all classes that extend parent
  void find_children(DexType* parent,
                     const std::unordered_set<DexClass*>& classes);

  // Walks all of the methods of the apk, updating type_to_inits
  void walk_methods(DexClass* container,
                    const std::vector<DexMethod*>& methods);

  // Walks the instructions of method, populating the relevant init types
  void inits_any_children(DexClass* container, DexMethod* method);

  // Walks block by block the method code that might instantiate a tracked type
  void analyze_block(
      DexClass* container,
      DexMethod* method,
      TypeToInit& populating_inits,
      const std::unordered_set<IRInstruction*>& tracked_instructions,
      cfg::Block* prev_block,
      cfg::Block* block);

  TypeToInit m_type_to_inits;

  MergedUsesMap m_stored_mergeds;

  boost::optional<DexString*> m_optional_method;
  std::unordered_set<DexMethodRef*> m_safe_escapes;

  // These registers are the storage for registers during analysis, they
  // are accessed and modified across recursive calls to analyze_block
  std::unordered_map<cfg::Block*, std::shared_ptr<RegistersPerBlock>>
      visited_blocks;
};

} // namespace cic
