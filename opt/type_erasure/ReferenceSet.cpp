/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ReferenceSet.h"

ReferenceSet::ReferenceSet(
  const Scope& scope,
  const TypeSet& ref_set) {
  // get field references
  const auto& find_field_refs = [&](const std::vector<DexField*>& fields) {
   for (const auto& field : fields) {
    if (ref_set.count(field->get_type()) == 0) continue;
    field_refs[field->get_type()].push_back(field);
   }
  };

  for (const auto cls : scope) {
   find_field_refs(cls->get_ifields());
   find_field_refs(cls->get_sfields());
  }

  // collect all vmethods in reference set
  std::unordered_set<DexMethod*> methods;
  for (const auto ref : ref_set) {
    const auto& cls = type_class(ref);
    for (const auto& meth : cls->get_vmethods()) {
      methods.insert(meth);
    }
  }
  // walk opcodes and get all the type and method references to ref_set
  walk_opcodes(scope,
    [&](DexMethod* meth) {
      auto proto = meth->get_proto();
      auto rtype = proto->get_rtype();
      if (ref_set.count(rtype) > 0) {
        sig_refs[rtype].emplace_back(proto);
      }
      for (const auto type : proto->get_args()->get_type_list()) {
        if (ref_set.count(type) > 0) {
          sig_refs[type].emplace_back(proto);
        }
      }
      return true;
    },
    [&](DexMethod* meth, IRInstruction* insn) {
      if (insn->has_type()) {
        const auto& type = static_cast<IRInstruction*>(insn)->get_type();
        if (ref_set.count(type) > 0) {
          code_refs[type].emplace_back(meth, insn);
        }
        return;
      }
      if (insn->has_method()) {
        auto method = static_cast<IRInstruction*>(insn)->get_method();
        DexMethod* def = resolve_method(method, opcode_to_search(insn));
        if (def == nullptr) def = method;
        if (ref_set.count(method->get_class()) > 0) {
          code_refs[method->get_class()].emplace_back(meth, insn);
        }
        if (methods.count(def) > 0) {
          code_refs[def->get_class()].emplace_back(meth, insn);
        }
        return;
      }
    });

  // build all references and callers map
  for (const auto& ref : field_refs) {
    all_refs.insert(ref.first);
  }
  for (const auto& ref : sig_refs) {
    all_refs.insert(ref.first);
  }
  for (const auto& ref : code_refs) {
    all_refs.insert(ref.first);
  }

  // type unferenced
  for (const auto& ref : ref_set) {
  if (all_refs.count(ref) > 0) continue;
    unrfs.insert(ref);
  }
}

void ReferenceSet::print() const {
  TRACE(TERA, 3, "- Total References %ld\n", all_refs.size());
  TRACE(TERA, 3, "- Field References %ld\n", field_refs.size());
  size_t ref_count = 0;
  for (const auto& ref_it : code_refs) {
    for (const auto& ref : ref_it.second) {
      if (ref.is_type_ref()) {
        ref_count++;
        break;
      }
    }
  }
  TRACE(TERA, 3, "- Type References %ld\n", ref_count);
  ref_count = 0;
  for (const auto& ref_it : code_refs) {
    for (const auto& ref : ref_it.second) {
      if (ref.is_method_ref()) {
        ref_count++;
        break;
      }
    }
  }
  TRACE(TERA, 3, "- Method References %ld\n", ref_count);
  TRACE(TERA, 3, "- Unreferenced %ld\n", unrfs.size());
  for (const auto& type : unrfs) {
    TRACE(TERA, 3, "\t%s\n", SHOW(type));
  }
}
