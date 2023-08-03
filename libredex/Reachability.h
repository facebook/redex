/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <unordered_set>

#include <sparta/WorkQueue.h>

#include "ConcurrentContainers.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "KeepReason.h"
#include "MethodOverrideGraph.h"
#include "MethodUtil.h"
#include "Pass.h"
#include "RemoveUninstantiablesImpl.h"
#include "Thread.h"

class DexAnnotation;

namespace reachability {

enum class ReachableObjectType : uint8_t {
  ANNO,
  CLASS,
  FIELD,
  METHOD,
  SEED,
};

/**
 * Represents an object (class, method, or field) that's considered reachable
 * by this pass.
 *
 * Used by our mark-sweep algorithm for keeping track of which objects to visit
 * next, as well as for logging what retains what so that we can see what things
 * which should be removed aren't.
 */
struct ReachableObject {
  ReachableObjectType type;
  union {
    const DexAnnotation* anno{nullptr};
    const DexClass* cls;
    const DexFieldRef* field;
    const DexMethodRef* method;
    const keep_reason::Reason* keep_reason;
  };

  explicit ReachableObject(const DexAnnotation* anno)
      : type{ReachableObjectType::ANNO}, anno{anno} {}
  explicit ReachableObject(const DexClass* cls)
      : type{ReachableObjectType::CLASS}, cls{cls} {}
  explicit ReachableObject(const DexMethodRef* method)
      : type{ReachableObjectType::METHOD}, method{method} {}
  explicit ReachableObject(const DexFieldRef* field)
      : type{ReachableObjectType::FIELD}, field{field} {}
  explicit ReachableObject(const keep_reason::Reason* keep_reason)
      : type(ReachableObjectType::SEED), keep_reason(keep_reason) {}
  explicit ReachableObject() : type{ReachableObjectType::SEED} {}

  friend std::ostream& operator<<(std::ostream&, const ReachableObject&);

  friend bool operator==(const ReachableObject& lhs,
                         const ReachableObject& rhs) {
    return lhs.type == rhs.type && lhs.anno == rhs.anno;
  }
};

struct ReachableObjectHash {
  std::size_t operator()(const ReachableObject& obj) const {
    return std::hash<uintptr_t>{}(reinterpret_cast<uintptr_t>(obj.anno));
  }
};

struct IgnoreSets {
  IgnoreSets() = default;
  std::unordered_set<const DexType*> string_literals;
  std::unordered_set<const DexType*> string_literal_annos;
  std::unordered_set<const DexType*> system_annos;
  bool keep_class_in_string{true};
};

// The ReachableObjectSet does not need to be a ConcurrentSet since it is nested
// within the ReachableObjectGraph's ConcurrentMap, which ensures that all
// updates to it are thread-safe. Using a plain unordered_set here is a
// significant performance improvement.
using ReachableObjectSet =
    std::unordered_set<ReachableObject, ReachableObjectHash>;
using ReachableObjectGraph =
    ConcurrentMap<ReachableObject, ReachableObjectSet, ReachableObjectHash>;

class ReachableObjects {
 public:
  const ReachableObjectGraph& retainers_of() const { return m_retainers_of; }

  bool mark(const DexClass* cls) { return m_marked_classes.insert(cls); }

  bool mark(const DexMethodRef* method) {
    return m_marked_methods.insert(method);
  }

  bool mark(const DexFieldRef* field) { return m_marked_fields.insert(field); }

  bool marked(const DexClass* cls) const { return m_marked_classes.count(cls); }

  bool marked(const DexMethodRef* method) const {
    return m_marked_methods.count(method);
  }

  bool marked(const DexFieldRef* field) const {
    return m_marked_fields.count(field);
  }

  bool marked_unsafe(const DexClass* cls) const {
    return m_marked_classes.count_unsafe(cls);
  }

  bool marked_unsafe(const DexMethodRef* method) const {
    return m_marked_methods.count_unsafe(method);
  }

  bool marked_unsafe(const DexFieldRef* field) const {
    return m_marked_fields.count_unsafe(field);
  }

  size_t num_marked_classes() const { return m_marked_classes.size(); }

  size_t num_marked_fields() const { return m_marked_fields.size(); }

  size_t num_marked_methods() const { return m_marked_methods.size(); }

 private:
  template <class Seed>
  void record_is_seed(Seed* seed);

  template <class Parent, class Object>
  void record_reachability(Parent*, Object*);

  template <class Object>
  void record_reachability(Object* parent, Object* object);

  void record_reachability(const DexFieldRef* member, const DexClass* cls);

  void record_reachability(const DexMethodRef* member, const DexClass* cls);

  static constexpr size_t MARK_SLOTS = 127;

  ConcurrentSet<const DexClass*,
                std::hash<const DexClass*>,
                std::equal_to<const DexClass*>,
                MARK_SLOTS>
      m_marked_classes;
  ConcurrentSet<const DexFieldRef*,
                std::hash<const DexFieldRef*>,
                std::equal_to<const DexFieldRef*>,
                MARK_SLOTS>
      m_marked_fields;
  ConcurrentSet<const DexMethodRef*,
                std::hash<const DexMethodRef*>,
                std::equal_to<const DexMethodRef*>,
                MARK_SLOTS>
      m_marked_methods;
  ReachableObjectGraph m_retainers_of;

  friend class RootSetMarker;
  friend class TransitiveClosureMarkerWorker;
};

enum class Condition {
  ClassRetained,
  ClassDynamicallyReferenced,
  ClassInstantiable,
};

struct References;

struct CFGNeedle {
  cfg::Block* block;
  IRList::const_iterator it;
};

using GatherMieFunction =
    std::function<void(const DexMethod*, const MethodItemEntry&, References*)>;

class MethodReferencesGatherer {
 public:
  MethodReferencesGatherer(
      const DexMethod* method,
      bool include_dynamic_references,
      bool check_init_instantiable,
      std::function<bool(const DexClass*)> is_class_instantiable,
      bool consider_code,
      GatherMieFunction gather_mie);

  enum class AdvanceKind {
    Initial,
    Callable,
    InstantiableDependencyResolved, // iff instantiable_cls
  };

  class Advance {
    explicit Advance(AdvanceKind kind) : m_kind(kind) {}
    Advance(AdvanceKind kind, const DexClass* instantiable_cls)
        : m_kind(kind), m_instantiable_cls(instantiable_cls) {}

   public:
    static Advance initial() { return Advance(AdvanceKind::Initial); }
    static Advance callable() { return Advance(AdvanceKind::Callable); }
    static Advance instantiable(const DexClass* instantiable_cls) {
      return Advance(AdvanceKind::InstantiableDependencyResolved,
                     instantiable_cls);
    }
    AdvanceKind kind() const { return m_kind; }
    const DexClass* instantiable_cls() const { return m_instantiable_cls; }

   private:
    AdvanceKind m_kind;
    const DexClass* m_instantiable_cls{nullptr};
  };

  void advance(const Advance& advance, References* refs);

  const DexMethod* get_method() const { return m_method; }

  uint32_t get_instructions_visited() const { return m_instructions_visited; }

  static void default_gather_mie(bool include_dynamic_references,
                                 const DexMethod* method,
                                 const MethodItemEntry& mie,
                                 References* refs,
                                 bool gather_methods = true);

 private:
  struct InstantiableDependency {
    const DexClass* cls{nullptr};
    bool may_continue_normally_if_uninstantiable{true};
    bool may_throw_if_uninstantiable{true};
  };
  std::optional<InstantiableDependency> get_instantiable_dependency(
      const MethodItemEntry& mie) const;

  const DexMethod* m_method;
  bool m_include_dynamic_references;
  bool m_check_init_instantiable;
  std::function<bool(const DexClass*)> m_is_class_instantiable;
  bool m_consider_code;
  GatherMieFunction m_gather_mie;
  std::mutex m_mutex;
  std::unordered_set<cfg::Block*> m_pushed_blocks;
  std::unordered_set<DexType*> m_covered_catch_types;
  std::unordered_map<const DexClass*, std::vector<CFGNeedle>>
      m_instantiable_dependencies;
  uint32_t m_instructions_visited{0};
  AdvanceKind m_next_advance_kind{AdvanceKind::Initial};
};

using MethodReferencesGatherers =
    std::unordered_map<const DexMethod*,
                       std::shared_ptr<MethodReferencesGatherer>>;

struct ConditionallyMarked {
  struct MarkedItems {
    ConcurrentSet<const DexField*> fields;
    ConcurrentSet<const DexMethod*> methods;
    ConcurrentMap<const DexClass*, MethodReferencesGatherers>
        method_references_gatherers;
    ConcurrentSet<DexType*> directly_instantiable_types;
  };

  // If any reference to the class if retained as part of any reachable
  // structure.
  MarkedItems if_class_retained;

  // If the class is referenced in a certain way that makes it discoverable via
  // reflection, using the rules of the DelInitPass.
  MarkedItems if_class_dynamically_referenced;

  // If the class is not abstract and has a constructor, or has a derived class
  // that does.
  MarkedItems if_class_instantiable;

  ConcurrentMap<const DexMethod*, std::shared_ptr<MethodReferencesGatherer>>
      if_instance_method_callable;
};

using CallableInstanceMethods = ConcurrentSet<const DexMethod*>;
using InstantiableTypes = ConcurrentSet<const DexClass*>;
using DynamicallyReferencedClasses = ConcurrentSet<const DexClass*>;

struct ReachableAspects {
  DynamicallyReferencedClasses dynamically_referenced_classes;
  CallableInstanceMethods callable_instance_methods;
  InstantiableTypes instantiable_types;
  InstantiableTypes uninstantiable_dependencies;
  uint64_t instructions_unvisited{0};
  void finish(const ConditionallyMarked& cond_marked);
};

struct References {
  std::vector<const DexString*> strings;
  std::vector<DexType*> types;
  std::vector<DexFieldRef*> fields;
  std::vector<DexMethodRef*> methods;
  // Conditional virtual method references. They are already resolved DexMethods
  // conditionally reachable at virtual call sites.
  std::vector<const DexMethod*> vmethods_if_class_instantiable;
  std::unordered_set<const DexClass*> classes_dynamically_referenced;
  std::vector<const DexClass*>
      method_references_gatherer_dependencies_if_class_instantiable;
  bool method_references_gatherer_dependency_if_instance_method_callable{false};
  std::vector<DexType*> new_instances;
  std::vector<DexMethod*> called_super_methods;
};

void gather_dynamic_references(const DexAnnotation* item,
                               References* references);

void gather_dynamic_references(const MethodItemEntry* mie,
                               References* references);

struct Stats {
  std::atomic<int> num_ignore_check_strings{0};
};

/*
 * These helper classes compute reachable objects by a DFS+marking algorithm.
 *
 * Conceptually we start at roots, which are defined by -keep rules in the
 * config file, and perform a depth-first search to find all references.
 * Elements visited in this manner will be retained, and are found in the
 * m_marked_* sets.
 *
 * -keepclassmembers rules are a bit more complicated, because they require
 * "conditional" marking: these members are kept only if their containing class
 * is determined to be kept. The conditional marking logic is also used to
 * retain (or not) implementations of interface methods. These elements are
 * placed in the cond_marked_* sets; care must be taken to promote
 * conditionally marked elements to fully marked.
 */
class RootSetMarker {
 public:
  RootSetMarker(const method_override_graph::Graph& method_override_graph,
                bool record_reachability,
                bool relaxed_keep_class_members,
                bool remove_no_argument_constructors,
                ConditionallyMarked* cond_marked,
                ReachableObjects* reachable_objects,
                ConcurrentSet<ReachableObject, ReachableObjectHash>* root_set)
      : m_method_override_graph(method_override_graph),
        m_record_reachability(record_reachability),
        m_relaxed_keep_class_members(relaxed_keep_class_members),
        m_remove_no_argument_constructors(remove_no_argument_constructors),
        m_cond_marked(cond_marked),
        m_reachable_objects(reachable_objects),
        m_root_set(root_set) {}

  virtual ~RootSetMarker() = default;
  /*
   * Initializes the root set by marking and pushing nodes onto the work queue.
   * Also conditionally marks class member seeds.
   */
  void mark(const Scope& scope);

  void mark_with_exclusions(
      const Scope& scope,
      const ConcurrentSet<const DexClass*>& excluded_classes,
      const ConcurrentSet<const DexMethod*>& excluded_methods);

  /**
   * Mark everything as seed.
   */
  void mark_all_as_seed(const Scope& scope);

  bool is_canary(const DexClass* cls);

  virtual bool should_mark_cls(const DexClass* cls);

 private:
  void push_seed(const DexClass* cls);

  void push_seed(const DexField* field, Condition condition);

  void push_seed(const DexMethod* metho, Condition condition);

  template <class Seed>
  void record_is_seed(Seed* seed);

  /*
   * Mark as seeds all methods that override or implement an external method.
   */
  void mark_external_method_overriders();

  static bool is_rootlike_clinit(const DexMethod* m);

  bool is_rootlike_init(const DexMethod* m) const;

  const method_override_graph::Graph& m_method_override_graph;
  bool m_record_reachability;
  bool m_relaxed_keep_class_members;
  bool m_remove_no_argument_constructors;
  ConditionallyMarked* m_cond_marked;
  ReachableObjects* m_reachable_objects;
  ConcurrentSet<ReachableObject, ReachableObjectHash>* m_root_set;
};

class TransitiveClosureMarkerWorker;

struct TransitiveClosureMarkerSharedState {
  const IgnoreSets* ignore_sets;
  const method_override_graph::Graph* method_override_graph;
  bool record_reachability;
  bool relaxed_keep_class_members;
  bool cfg_gathering_check_instantiable;
  bool cfg_gathering_check_instance_callable;

  ConditionallyMarked* cond_marked;
  ReachableObjects* reachable_objects;
  ReachableAspects* reachable_aspects;
  Stats* stats;
};

using TransitiveClosureMarkerWorkerState = sparta::WorkerState<ReachableObject>;

/*
 * Resolve the method reference more conservatively without the context of the
 * call, such as call instruction, target type and the caller method.
 */
const DexMethod* resolve_without_context(const DexMethodRef* method,
                                         const DexClass* cls);

class TransitiveClosureMarkerWorker {
 public:
  TransitiveClosureMarkerWorker(
      const TransitiveClosureMarkerSharedState* shared_state,
      TransitiveClosureMarkerWorkerState* worker_state)
      : m_shared_state(shared_state), m_worker_state(worker_state) {}

  virtual ~TransitiveClosureMarkerWorker() = default;

  /*
   * Marks :obj and pushes its immediately reachable neighbors onto the local
   * task queue of the current worker.
   */
  virtual void visit(const ReachableObject& obj);

  virtual void visit_cls(const DexClass* cls);

  virtual void visit_method_ref(const DexMethodRef* method);

  void visit_field_ref(const DexFieldRef* field);

  virtual References gather(const DexAnnotation* anno) const;

  virtual References gather(const DexField* field) const;

  template <class Parent>
  void push(const Parent* parent, const DexType* type);

  void push(const DexMethodRef* parent, const DexType* type);

 protected:
  template <class Parent, class InputIt>
  void push(const Parent* parent, InputIt begin, InputIt end);

  template <class Parent>
  void push(const Parent* parent, const DexClass* cls);

  template <class Parent>
  void push(const Parent* parent, const DexFieldRef* field);

  template <class Parent>
  void push(const Parent* parent, const DexMethodRef* method);

  void push(const DexMethodRef* parent, const DexMethodRef* method);

  void push_if_class_instantiable(const DexField* field);

  void push_if_class_instantiable(const DexMethod* method);

  void push_if_class_instantiable(
      const DexClass* cls,
      std::shared_ptr<MethodReferencesGatherer> method_references_gatherer);

  void push_if_class_retained(const DexField* field);

  void push_if_class_retained(const DexMethod* method);

  void push_directly_instantiable_if_class_dynamically_referenced(
      DexType* type);

  void push_if_instance_method_callable(
      std::shared_ptr<MethodReferencesGatherer> method_references_gatherer);

  bool has_class_forName(const DexMethod* meth);

  void gather_and_push(
      std::shared_ptr<MethodReferencesGatherer> method_references_gatherer,
      const MethodReferencesGatherer::Advance& advance);

  std::shared_ptr<MethodReferencesGatherer> create_method_references_gatherer(
      const DexMethod* method,
      bool consider_code = true,
      GatherMieFunction gather_mie = nullptr);

  virtual void gather_and_push(const DexMethod* meth);

  template <typename T>
  void gather_and_push(T t);

  template <class Parent>
  void push_typelike_strings(const Parent* parent,
                             const std::vector<const DexString*>& strings);

  template <class Parent, class Object>
  void record_reachability(Parent* parent, Object* object);

  void instantiable(DexType* type);

  void directly_instantiable(DexType* type);
  void directly_instantiable(const std::vector<DexType*>& types) {
    for (auto* type : types) {
      directly_instantiable(type);
    }
  }

  void instance_callable(DexMethod* method);
  void instance_callable(const std::vector<DexMethod*>& methods) {
    for (auto* m : methods) {
      instance_callable(m);
    }
  }

  void dynamically_referenced(const DexClass* cls);
  void dynamically_referenced(
      const std::unordered_set<const DexClass*>& classes) {
    for (auto* cls : classes) {
      dynamically_referenced(cls);
    }
  }

  const TransitiveClosureMarkerSharedState* const m_shared_state;
  TransitiveClosureMarkerWorkerState* const m_worker_state;
};

/*
 * Compute all reachable objects from the existing configurations
 * (e.g. proguard rules).
 */
std::unique_ptr<ReachableObjects> compute_reachable_objects(
    const DexStoresVector& stores,
    const IgnoreSets& ignore_sets,
    int* num_ignore_check_strings,
    ReachableAspects* reachable_aspects,
    bool record_reachability = false,
    bool relaxed_keep_class_members = false,
    bool cfg_gathering_check_instantiable = false,
    bool cfg_gathering_check_instance_callable = false,
    bool should_mark_all_as_seed = false,
    std::unique_ptr<const method_override_graph::Graph>*
        out_method_override_graph = nullptr,
    bool remove_no_argument_constructors = false);

void sweep(DexStoresVector& stores,
           const ReachableObjects& reachables,
           ConcurrentSet<std::string>* removed_symbols = nullptr,
           bool output_full_removed_symbols = false);

remove_uninstantiables_impl::Stats sweep_code(
    DexStoresVector& stores,
    bool prune_uncallable_instance_method_bodies,
    bool skip_uncallable_virtual_methods,
    const ReachableAspects& reachable_aspects);

remove_uninstantiables_impl::Stats sweep_uncallable_virtual_methods(
    DexStoresVector& stores, const ReachableAspects& reachable_aspects);

struct ObjectCounts {
  size_t num_classes{0};
  size_t num_fields{0};
  size_t num_methods{0};
};

/*
 * Count the number of objects in scope. Can be used to measure the number of
 * objects removed by a mark-sweep.
 */
ObjectCounts count_objects(const DexStoresVector& stores);

void dump_graph(std::ostream& os, const ReachableObjectGraph& retainers_of);

bool consider_dynamically_referenced(const DexClass* cls);

} // namespace reachability
