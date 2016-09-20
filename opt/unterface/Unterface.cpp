/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Unterface.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <functional>
#include <unordered_set>
#include <unordered_map>
#include <cctype>
#include <utility>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "Walkers.h"
#include "UnterfaceOpt.h"

namespace {

//
// Utilities
//
bool is_anonymous(DexClass* cls) {
  const auto name = cls->get_type()->get_name()->c_str();
  auto end = strlen(name) - 1; // skip ';' at end of type name
  while (end && isdigit(name[--end]))
    ;
  return end && name[end] == '$';
}

/**
 * Collect traits of interest for an interface or an implementation.
 * Not all flags apply to interfaces but we have a single common enum.
 */
enum Trait : uint32_t {
  NO_TRAIT = 0x0,
  HAS_SUPER = 0x1,
  HAS_CHILDREN = 0x2,
  IS_ABSTRACT = 0x4,
  IMPL_MULTIPLE_INTERFACES = 0x8, // impl implements 2 interfaces or more
  HAS_INIT = 0x10,
  HAS_CLINIT = 0x20,
  HAS_DIRECT_METHODS = 0x40, // has direct methods other than init/clinit
  HAS_STATIC_FIELDS = 0x80, // impl or interface has static fields
  NO_VMETHODS = 0x100, // empty interface or impl
  HAS_MULTIPLE_INSTANCE_FIELDS = 0x200, // 2 or more instance fields
  HAS_SIMPLE_INIT = 0x400, // init matches a set field pattern
  MULTIPLE_ARGS_CTOR = 0x800, // ctor signatures take 2 or more arguments
  IS_ANONYMOUS = 0x1000,
  // lazy trait, computed on demand. 0 means lazy traits have not been computed
  LAZY_TRAITS = 0xFFFF0000,
  MATCHES_INTERFACE_METHODS = 0x10000,
  NO_MATCH_INTERFACE_METHODS = 0x20000,
};

inline Trait operator|=(Trait& a, const Trait b) {
  return (a = static_cast<Trait>(static_cast<uint32_t>(a) |
                                 static_cast<uint32_t>(b)));
}

inline Trait DEBUG_ONLY operator&=(Trait& a, const Trait b) {
  return (a = static_cast<Trait>(static_cast<uint32_t>(a) &
                                 static_cast<uint32_t>(b)));
}

inline Trait operator|(const Trait& a, const Trait b) {
  return static_cast<Trait>(static_cast<uint32_t>(a) |
                            static_cast<uint32_t>(b));
}

inline Trait operator&(const Trait& a, const Trait b) {
  return static_cast<Trait>(static_cast<uint32_t>(a) &
                            static_cast<uint32_t>(b));
}

Trait check_init(DexMethod* meth) {
  Trait trait = NO_TRAIT;
  auto& code = meth->get_code();
  if (meth->get_proto()->get_args()->get_type_list().size() > 1) {
    trait |= MULTIPLE_ARGS_CTOR;
  }
  if (code == nullptr) return trait;
  if (code->get_ins_size() != 2) return trait;
  auto insns = code->get_instructions();
  if (insns.size() != 3) return trait;
  if (insns[0]->opcode() == OPCODE_IPUT_OBJECT &&
      insns[1]->opcode() == OPCODE_INVOKE_DIRECT &&
      insns[2]->opcode() == OPCODE_RETURN_VOID) {
    trait |= HAS_SIMPLE_INIT;
  }
  return trait;
}

Trait check_dmethods(std::list<DexMethod*>& dmethods) {
  Trait trait = NO_TRAIT;
  for (auto meth : dmethods) {
    if (is_init(meth)) {
      trait |= HAS_INIT;
      trait |= check_init(meth);
      continue;
    }
    if (is_clinit(meth)) {
      trait |= HAS_CLINIT;
      continue;
    }
    trait |= HAS_DIRECT_METHODS;
  }
  return trait;
}

Trait check_vmethods(std::list<DexMethod*>& vmethods) {
  if (vmethods.size() == 0) return NO_VMETHODS;
  return NO_TRAIT;
}

Trait check_sfields(std::list<DexField*>& sfields) {
  if (sfields.size() > 0) return HAS_STATIC_FIELDS;
  return NO_TRAIT;
}

Trait check_ifields(std::list<DexField*>& ifields) {
  if (ifields.size() > 1) return HAS_MULTIPLE_INSTANCE_FIELDS;
  return NO_TRAIT;
}

/**
 * Check whether the methods in the class match exactly and only
 * those in the interface.
 */
Trait match_interfaces(DexClass* cls, std::unordered_set<DexClass*> intfs) {
  size_t intf_mcount = 0;
  for (auto intf : intfs) {
    intf_mcount = intf->get_vmethods().size();
  }
  auto vmethods = cls->get_vmethods();
  if (vmethods.size() != intf_mcount) return NO_TRAIT;
  for (auto meth : vmethods) {
    for (auto intf : intfs) {
      for (auto imeth : intf->get_vmethods()) {
        if (imeth->get_name() == meth->get_name() &&
            imeth->get_proto() == meth->get_proto()) {
          goto next;
        }
      }
    }
    return NO_MATCH_INTERFACE_METHODS;
  next:
    ;
  }
  return MATCHES_INTERFACE_METHODS;
}

//
// Data set
//

using ClassSet = std::unordered_set<DexClass*>;
using ClassTraits = std::unordered_map<DexClass*, Trait>;

using IntfFilter = std::function<bool (DexClass*)>;
using ImplsFilter = std::function<bool (std::unordered_set<DexClass*>&)>;

/**
 * Info about interfaces and their implementations.
 */
class InterfaceImplementations {
public:
  InterfaceImplementations(Scope& scope);
  TypeRelationship match(IntfFilter intf_filter, ImplsFilter impls_filter);

  Trait get_intf_traits(DexClass* intf) {
    return intf_traits[intf];
  }

  Trait get_impl_traits(DexClass* intf) {
    return impl_traits[intf];
  }

  // debug and tracing helper
  void analyze_candidates(TypeRelationship& candidates, const char* name);
  bool print_all() {
    analyze_candidates(intf_to_impls, "ALL");
    return true;
  }

private:
  Scope& scope;
  ClassSet ifset;
  ClassTraits intf_traits;
  TypeRelationship intf_to_impls;
  TypeRelationship impl_to_intfs;
  ClassTraits impl_traits;

private:
  // initializers...
  void load_interfaces();
  void compute_interface_traits();
  void load_implementors();
  void compute_implementor_traits();
  // lazy initializers...
  void compute_lazy_traits(
      DexClass* intf, std::unordered_set<DexClass*>& impls);
  // helpers...
  void find_implementor(DexClass* clazz, DexClass* intf);
};

TypeRelationship InterfaceImplementations::match(
    IntfFilter intf_filter, ImplsFilter impls_filter) {
  TypeRelationship intf_impls;
  for (auto intf_it : intf_traits) {
    if (!intf_filter(intf_it.first)) continue;
    compute_lazy_traits(intf_it.first, intf_to_impls[intf_it.first]);
    if (!impls_filter(intf_to_impls[intf_it.first])) continue;
    intf_impls[intf_it.first] = intf_to_impls[intf_it.first];
  }
  return intf_impls;
}

InterfaceImplementations::InterfaceImplementations(Scope& scope)
    : scope(scope) {
  load_interfaces();
  for (auto intf : ifset) {
    intf_traits[intf] = NO_TRAIT;
  }
  compute_interface_traits();
  load_implementors();
  compute_implementor_traits();
}

void InterfaceImplementations::load_interfaces() {
  for (auto clazz : scope) {
    if ((clazz->get_access() & DexAccessFlags::ACC_INTERFACE) &&
        !(clazz->get_access() & DexAccessFlags::ACC_ANNOTATION)) {
      ifset.insert(clazz);
    }
  }
}

void InterfaceImplementations::compute_interface_traits() {
  for (auto& intf_it : intf_traits) {
    auto intf = intf_it.first;
    if (intf->get_interfaces()->get_type_list().size() != 0) {
      intf_it.second |= HAS_SUPER;
      for (auto super : intf->get_interfaces()->get_type_list()) {
        auto super_cls = type_class(super);
        if (super_cls == nullptr) continue;
        intf_traits[super_cls] |= HAS_CHILDREN;
      }
    }
    intf_it.second |= check_dmethods(intf->get_dmethods());
    intf_it.second |= check_vmethods(intf->get_vmethods());
    intf_it.second |= check_sfields(intf->get_sfields());
    always_assert(intf->get_ifields().size() == 0);
    always_assert((intf_it.second & (HAS_INIT | HAS_DIRECT_METHODS)) == 0);
  }
}

void InterfaceImplementations::load_implementors() {
  for (auto clazz : scope) {
    if (clazz->get_access() & DexAccessFlags::ACC_INTERFACE) continue;
    auto intfs = clazz->get_interfaces()->get_type_list();
    for (auto type : intfs) {
      auto intf = type_class(type);
      if (intf == nullptr) continue;
      find_implementor(clazz, intf);
    }
  }
}

void InterfaceImplementations::find_implementor(
    DexClass* clazz, DexClass* intf) {
  auto parents = intf->get_interfaces()->get_type_list();
  for (auto parent_type : parents) {
    auto parent = type_class(parent_type);
    if (parent == nullptr) continue;
    find_implementor(clazz, parent);
  }
  if (intf_traits.count(intf)) {
    intf_to_impls[intf].insert(clazz);
    impl_to_intfs[clazz].insert(intf);
  }
}

/**
 * Compute traits for the set of implementors in the specific analysis.
 */
void InterfaceImplementations::compute_implementor_traits() {
  for (auto& impl_it : impl_to_intfs) {
    auto impl = impl_it.first;
    auto trait =
        impl_it.second.size() > 1 ? IMPL_MULTIPLE_INTERFACES : NO_TRAIT;
    if (is_anonymous(impl)) trait |= IS_ANONYMOUS;
    if (impl->get_access() & DexAccessFlags::ACC_ABSTRACT) {
      trait |= IS_ABSTRACT;
    }
    if (get_children(impl->get_type()).size() > 0) trait |= HAS_CHILDREN;
    if (impl->get_super_class() != get_object_type()) trait |= HAS_SUPER;
    trait |= check_dmethods(impl->get_dmethods());
    trait |= check_vmethods(impl->get_vmethods());
    trait |= check_sfields(impl->get_sfields());
    trait |= check_ifields(impl->get_ifields());
    impl_traits[impl] = trait;
  }
}

void InterfaceImplementations::compute_lazy_traits(
    DexClass* intf, std::unordered_set<DexClass*>& impls) {
  for (auto impl : impls) {
    if ((impl_traits[impl] & LAZY_TRAITS) == 0) {
      impl_traits[impl] |= match_interfaces(intf, impl_to_intfs[impl]);
    }
  }
}

//
// Debug and tracing utilities
//

void InterfaceImplementations::analyze_candidates(
    TypeRelationship& candidates, const char* name) {
  std::vector<DexClass*> intfs;
  size_t impl_count = 0;
  for (auto& impls : candidates) {
    impl_count += impls.second.size();
    intfs.push_back(impls.first);
  }
  std::sort(intfs.begin(), intfs.end(),
      [&](DexClass* first, DexClass* second) {
        return candidates[first].size() > candidates[second].size();
      });

  // implementations
  TRACE(UNTF, 5,
      "**** %s\n** Interfaces: %ld, Implementors: %ld\n",
      name, candidates.size(), impl_count);
  TRACE(UNTF, 6, "[impls] interface (meths)\n");
  for (int i = 0; i < 20; i++) {
    TRACE(UNTF, 6, "[%ld] %s (%ld)\n", intf_to_impls[intfs[i]].size(),
        SHOW(intfs[i]->get_type()), intfs[i]->get_vmethods().size());
  }

  // signatures
  std::unordered_map<DexProto*, size_t> unique_sig;
  std::unordered_map<DexProto*, size_t> multiple_sig;
  for (auto intf : intfs) {
    std::unordered_map<DexProto*, size_t> protos;
    for (auto meth : intf->get_vmethods()) {
      auto proto = meth->get_proto();
      protos[proto]++;
    }
    for (auto proto : protos) {
      unique_sig[proto.first]++;
      if (proto.second > 1) {
        multiple_sig[proto.first] = std::max(multiple_sig[proto.first],
            proto.second);
      }
    }
  }

  std::vector<DexProto*> sigs;
  for (auto& proto : unique_sig) {
    sigs.push_back(proto.first);
  }
  std::sort(sigs.begin(), sigs.end(),
      [&](DexProto* first, DexProto* second) {
        return unique_sig[first] > unique_sig[second];
      });

  TRACE(UNTF, 5,
      "** Unique signatures %ld\nsignature [count]\n", unique_sig.size());
  for (int i = 0; i < 20; i++) {
    TRACE(UNTF, 6, "%s [%ld]\n", SHOW(sigs[i]), unique_sig[sigs[i]]);
  }

  sigs.clear();
  for (auto& proto : multiple_sig) {
    sigs.push_back(proto.first);
  }
  std::sort(sigs.begin(), sigs.end(),
      [&](DexProto* first, DexProto* second) {
        return multiple_sig[first] > multiple_sig[second];
      });
  TRACE(UNTF, 5, "** Multiple signature needed %ld\nsignature [count]\n",
      multiple_sig.size());
  for (int i = 0; i < 10; i++) {
    TRACE(UNTF, 6, "%s [%ld]\n", SHOW(sigs[i]), multiple_sig[sigs[i]]);
  }
}

/**
 * Helper to match and trace possible optimizations.
 * The exclude interface trait is applied over the interface.
 * If interface is not in the exclude filter every impl is matched against
 * the exclude impl traits.
 */
TypeRelationship exclude(InterfaceImplementations& interfaces,
                         Trait exclude_intf_trait,
                         Trait exclude_impls_trait) {
  return interfaces.match(
      [&](DexClass* intf) {
        auto trait = interfaces.get_intf_traits(intf);
        return (trait & exclude_intf_trait) == 0;
      },
      [&](std::unordered_set<DexClass*>& impls) {
        if (impls.size() < 2) return false;
        for (auto impl : impls) {
          auto traits = interfaces.get_impl_traits(impl);
          if ((traits & exclude_impls_trait) != 0) {
            return false;
          }
        }
        return true;
      });
}

}

void UnterfacePass::run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  Scope scope = build_class_scope(stores);

  InterfaceImplementations interfaces(scope);
  assert(interfaces.print_all());

  auto one_level = exclude(interfaces,
      HAS_SUPER | HAS_CHILDREN | NO_VMETHODS,
      IMPL_MULTIPLE_INTERFACES |
          HAS_SUPER |
          HAS_CHILDREN |
          IS_ABSTRACT |
          NO_MATCH_INTERFACE_METHODS |
          HAS_MULTIPLE_INSTANCE_FIELDS |
          MULTIPLE_ARGS_CTOR |
          HAS_CLINIT |
          NO_VMETHODS |
          HAS_STATIC_FIELDS |
          HAS_DIRECT_METHODS);
  interfaces.analyze_candidates(one_level, "No hierarchy, perfect match");

  // optimize
  std::vector<DexClass*> untfs;
  std::unordered_set<DexClass*> removed;
  //optimize(scope, candidates, untfs, removed);

  // write back
  DexClassesVector outdex;
  DexClasses& orig_classes = stores[0].get_dexen()[0];
  DexClasses classes((size_t)(orig_classes.size() + untfs.size() - removed.size()));
  int pos = 0;
  for (size_t i = 0; i < orig_classes.size(); ++i) {
    auto cls = orig_classes.get(i);
    if (removed.find(cls) == removed.end()) {
      classes.insert_at(cls, pos++);
    }
  }
  for (auto untf : untfs) {
    classes.insert_at(untf, pos++);
  }
  outdex.emplace_back(std::move(classes));
  for (size_t i = 1; i < stores[0].get_dexen().size(); i++) {
    outdex.emplace_back(std::move(stores[0].get_dexen()[i]));
  }
  stores[0].get_dexen() = std::move(outdex);
}

static UnterfacePass s_pass;
