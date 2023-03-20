/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <unordered_set>

#include "ConcurrentContainers.h"
#include "DexClass.h"
#include "KeepReason.h"
#include "MethodOverrideGraph.h"
#include "Pass.h"
#include "SpartaWorkQueue.h"
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

  void mark(const DexClass* cls) { m_marked_classes.insert(cls); }

  void mark(const DexMethodRef* method) { m_marked_methods.insert(method); }

  void mark(const DexFieldRef* field) { m_marked_fields.insert(field); }

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
  friend class TransitiveClosureMarker;
};

struct ConditionallyMarked {
  ConcurrentSet<const DexField*> fields;
  ConcurrentSet<const DexMethod*> methods;
};

struct References {
  std::vector<const DexString*> strings;
  std::vector<DexType*> types;
  std::vector<DexFieldRef*> fields;
  std::vector<DexMethodRef*> methods;
  // Conditional method references. They are already resolved DexMethods
  // conditionally reachable at virtual call sites.
  std::vector<const DexMethod*> cond_methods;
};

// Each thread will have its own instance of Stats, so align it in order to
// avoid false sharing.
struct alignas(CACHE_LINE_SIZE) Stats {
  int num_ignore_check_strings;
};

using MarkWorkerState = sparta::SpartaWorkerState<ReachableObject>;

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
                ConditionallyMarked* cond_marked,
                ReachableObjects* reachable_objects,
                ConcurrentSet<ReachableObject, ReachableObjectHash>* root_set)
      : m_method_override_graph(method_override_graph),
        m_record_reachability(record_reachability),
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

  void push_seed(const DexField* field);

  void push_seed(const DexMethod* method);

  template <class Seed>
  void record_is_seed(Seed* seed);

  /*
   * Mark as seeds all methods that override or implement an external method.
   */
  void mark_external_method_overriders();

  const method_override_graph::Graph& m_method_override_graph;
  bool m_record_reachability;
  ConditionallyMarked* m_cond_marked;
  ReachableObjects* m_reachable_objects;
  ConcurrentSet<ReachableObject, ReachableObjectHash>* m_root_set;
};

class TransitiveClosureMarker {
 public:
  TransitiveClosureMarker(
      const IgnoreSets& ignore_sets,
      const method_override_graph::Graph& method_override_graph,
      bool record_reachability,
      ConditionallyMarked* cond_marked,
      ReachableObjects* reachable_objects,
      MarkWorkerState* worker_state,
      Stats* stats,
      bool remove_no_argument_constructors = false)
      : m_ignore_sets(ignore_sets),
        m_method_override_graph(method_override_graph),
        m_record_reachability(record_reachability),
        m_cond_marked(cond_marked),
        m_reachable_objects(reachable_objects),
        m_worker_state(worker_state),
        m_stats(stats),
        m_remove_no_argument_constructors(remove_no_argument_constructors) {
    if (s_class_forname == nullptr) {
      s_class_forname = DexMethod::get_method(
          "Ljava/lang/Class;.forName:(Ljava/lang/String;)Ljava/lang/Class;");
    }
  }

  virtual ~TransitiveClosureMarker() = default;

  /*
   * Marks :obj and pushes its immediately reachable neighbors onto the local
   * task queue of the current worker.
   */
  virtual void visit(const ReachableObject& obj);

  virtual void visit_cls(const DexClass* cls);

  virtual void visit_method_ref(const DexMethodRef* method);

  void visit_field_ref(const DexFieldRef* field);

  virtual References gather(const DexAnnotation* anno) const;

  virtual References gather(const DexMethod* method) const;

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

  void push_cond(const DexMethod* method);

  bool has_class_forname(DexMethod* meth);

  void gather_and_push(DexMethod* meth);

  template <typename T>
  void gather_and_push(T t);

  template <class Parent>
  void push_typelike_strings(const Parent* parent,
                             const std::vector<const DexString*>& strings);

  template <class Parent, class Object>
  void record_reachability(Parent* parent, Object* object);

  /*
   * Resolve the method reference more conservatively without the context of the
   * call, such as call instruction, target type and the caller method.
   */
  static DexMethod* resolve_without_context(const DexMethodRef* method,
                                            const DexClass* cls);

  const IgnoreSets& m_ignore_sets;
  const method_override_graph::Graph& m_method_override_graph;
  bool m_record_reachability;
  ConditionallyMarked* m_cond_marked;
  ReachableObjects* m_reachable_objects;
  MarkWorkerState* m_worker_state;
  Stats* m_stats;
  bool m_remove_no_argument_constructors;

  static DexMethodRef* s_class_forname;
};

/*
 * Compute all reachable objects from the existing configurations
 * (e.g. proguard rules).
 */
std::unique_ptr<ReachableObjects> compute_reachable_objects(
    const DexStoresVector& stores,
    const IgnoreSets& ignore_sets,
    int* num_ignore_check_strings,
    bool record_reachability = false,
    bool should_mark_all_as_seed = false,
    std::unique_ptr<const method_override_graph::Graph>*
        out_method_override_graph = nullptr,
    bool remove_no_argument_constructors = false);

void sweep(DexStoresVector& stores,
           const ReachableObjects& reachables,
           ConcurrentSet<std::string>* removed_symbols,
           bool output_full_removed_symbols = false);

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

} // namespace reachability
