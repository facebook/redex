/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "IRInstruction.h"
#include <map>
#include <unordered_set>

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
  std::map<reg_t, std::set<IRInstruction*>> regs;
  FlowStatus set;
  SourceStatus source;
};

struct MethodCall {
  FlowStatus call;
  std::set<std::pair<IRInstruction*, reg_t>> call_sites;
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

typedef std::map<DexFieldRef*, FieldSet, dexfields_comparator> FieldSetMap;
typedef std::map<DexFieldRef*, FlowStatus, dexfields_comparator> FieldReadMap;
typedef std::map<DexMethodRef*, MethodCall, dexmethods_comparator> CallMap;
typedef std::map<IRInstruction*, FlowStatus> ArrayWriteMap;

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
  std::vector<std::pair<IRInstruction*, reg_t>> get_escape_instructions();

  std::set<IRInstruction*> return_instrs;
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

// TrackedUses is the 'abstract' parent class of the domain representation
class TrackedUses {
 public:
  TrackedUses(Tracked kind);
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
  explicit ObjectUses(DexType* typ, IRInstruction* instr)
      : TrackedUses(Object), m_id(instr), m_class_used(typ) {}

  void combine_paths(const TrackedUses& other);
  void merge(const TrackedUses& other);
  bool consistent_with(const TrackedUses& other);

  bool same_instr(const ObjectUses& other) const { return m_id == other.m_id; }

  size_t hash() const { return m_id->hash(); }
  IRInstruction* get_instr() const { return m_id; }
  DexType* get_represents_typ() const { return m_class_used; }

  FlowStatus created_flow = AllPaths;

 private:
  IRInstruction* m_id;
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
  MergedUses(const ObjectUses&);

  void combine_paths(const TrackedUses& other);
  void merge(const TrackedUses& other);
  bool consistent_with(const TrackedUses& other);
  size_t hash() const;
  bool same_instrs(const MergedUses& other) const;
  void set_is_nullable() { m_includes_nullable = true; }

  const std::set<IRInstruction*>& get_instrs() const { return m_instrs; }
  const std::set<DexType*, dextypes_comparator>& get_classes() const {
    return m_classes;
  }
  bool is_nullable() const { return m_includes_nullable; }

 private:
  std::set<IRInstruction*> m_instrs;
  std::set<DexType*, dextypes_comparator> m_classes;
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
    if (!l && !r) {
      return true;
    }
    if (!l || !r) {
      return false;
    }
    if (l->m_tracked_kind != r->m_tracked_kind) {
      return false;
    }
    if (l->m_tracked_kind == Merged) {
      return reinterpret_cast<MergedUses*>(&*l)->same_instrs(
          reinterpret_cast<MergedUses&>(*r));
    } else {
      // Has to be ObjectUses
      return reinterpret_cast<ObjectUses*>(&*l)->same_instr(
          reinterpret_cast<ObjectUses&>(*r));
    }
  }
};

typedef std::
    unordered_set<std::shared_ptr<ObjectUses>, TrackedHasher, TrackedComparer>
        ObjectUsedSet;

typedef std::
    unordered_set<std::shared_ptr<MergedUses>, TrackedHasher, TrackedComparer>
        MergedUsedSet;

typedef std::
    unordered_set<std::shared_ptr<TrackedUses>, TrackedHasher, TrackedComparer>
        UsedSet;

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
  void merge_registers(const RegisterSet& other, MergedUsedSet& stored);

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
  using InitMap =
      std::map<DexClass*,
               std::map<DexMethod*,
                        std::map<IRInstruction*,
                                 std::vector<std::shared_ptr<ObjectUses>>>,
                        dexmethods_comparator>,
               dexclasses_comparator>;

 public:
  InitLocation(DexType* typ) : m_typ(typ) {}
  InitLocation() = default;
  uint32_t get_count() const { return m_count; }

  // adds the data structure for this initialization, returning a ref to it
  std::shared_ptr<ObjectUses> add_init(DexClass* container,
                                       DexMethod* caller,
                                       IRInstruction* instr);
  void update_object(DexClass* container,
                     DexMethod* caller,
                     const ObjectUses& obj);
  const InitMap& get_inits() const { return m_inits; }

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
  using TypeToInit = std::map<DexType*, InitLocation, dextypes_comparator>;
  using MergedUsesMap =
      std::map<DexType*,
               std::map<DexMethod*, MergedUsedSet, dexmethods_comparator>,
               dextypes_comparator>;

  ClassInitCounter(
      DexType* common_parent,
      const std::set<DexMethodRef*, dexmethods_comparator>& safe_escapes,
      const std::unordered_set<DexClass*>& classes,
      boost::optional<DexString*> optional_method_name = boost::none);

  const TypeToInit& type_to_inits() const { return m_type_to_inits; }
  const MergedUsesMap& merged_uses() const { return m_stored_mergeds; }

  // For debugging
  std::string debug_show_table();

 private:
  // Identifies and stores in type_to_inits all classes that extend parent
  void find_children(DexType* parent,
                     const std::unordered_set<DexClass*>& classes);

  // Walks all of the methods of the apk, updating type_to_inits
  void walk_methods(DexClass* container,
                    const std::vector<DexMethod*>& methods);

  // Walks the instructions of method, populating the relevant init types
  void inits_any_children(DexClass* container, DexMethod* method);

  // Walks block by block the method code that might instantiate a tracked type
  void analyze_block(DexClass* container,
                     DexMethod* method,
                     cfg::Block* prev_block,
                     cfg::Block* block);

  TypeToInit m_type_to_inits;

  MergedUsesMap m_stored_mergeds;

  boost::optional<DexString*> m_optional_method;
  std::set<DexMethodRef*, dexmethods_comparator> m_safe_escapes;

  // These registers are the storage for registers during analysis, they
  // are accessed and modified across recursive calls to analyze_block
  std::unordered_map<cfg::Block*, std::shared_ptr<RegistersPerBlock>>
      visited_blocks;
};

} // namespace cic
