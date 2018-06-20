/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "TypeReference.h"

#include "MethodReference.h"
#include "Resolver.h"
#include "VirtualScope.h"
#include "Walkers.h"

using CallSites = std::vector<std::pair<DexMethod*, IRInstruction*>>;

namespace {

void fix_colliding_method(
    const Scope& scope,
    const std::unordered_map<DexMethod*, DexProto*>& colliding_methods) {
  // Fix colliding methods by appending an additional param.
  TRACE(REFU, 9, "sig: colliding_methods %d\n", colliding_methods.size());
  for (auto it : colliding_methods) {
    auto meth = it.first;
    auto new_proto = it.second;
    auto new_arg_list =
        type_reference::append_and_make(new_proto->get_args(), get_int_type());
    new_proto = DexProto::make_proto(new_proto->get_rtype(), new_arg_list);
    DexMethodSpec spec;
    spec.proto = new_proto;
    meth->change(spec, false);

    auto code = meth->get_code();
    auto new_param_reg = code->allocate_temp();
    auto& last_param = code->get_param_instructions().back().insn;
    auto new_param_load = new IRInstruction(IOPCODE_LOAD_PARAM);
    new_param_load->set_dest(new_param_reg);
    code->insert_after(last_param, std::vector<IRInstruction*>{new_param_load});
    TRACE(REFU, 9, "sig: patching colliding method %s\n", SHOW(meth));
  }

  walk::parallel::code(scope, [&](DexMethod* meth, IRCode& code) {
    CallSites callsites;
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (!insn->has_method()) {
        continue;
      }
      const auto callee =
          resolve_method(insn->get_method(),
                         opcode_to_search(const_cast<IRInstruction*>(insn)));
      if (callee == nullptr ||
          colliding_methods.find(callee) == colliding_methods.end()) {
        continue;
      }
      callsites.emplace_back(callee, insn);
    }

    for (const auto& pair : callsites) {
      auto callee = pair.first;
      auto insn = pair.second;
      always_assert(callee != nullptr);
      TRACE(REFU,
            9,
            "sig: patching colliding method callsite to %s in %s\n",
            SHOW(callee),
            SHOW(meth));
      // 42 is a dummy int val as the additional argument to the patched
      // colliding method.
      method_reference::CallSiteSpec spec{meth, insn, callee};
      method_reference::patch_callsite(spec, boost::optional<uint32_t>(42));
    }
  });
}
} // namespace

namespace type_reference {

bool proto_has_reference_to(const DexProto* proto, const TypeSet& targets) {
  auto rtype = get_array_type_or_self(proto->get_rtype());
  if (targets.count(rtype) > 0) {
    return true;
  }
  std::deque<DexType*> lst;
  for (const auto arg_type : proto->get_args()->get_type_list()) {
    auto extracted_arg_type = get_array_type_or_self(arg_type);
    if (targets.count(extracted_arg_type) > 0) {
      return true;
    }
  }
  return false;
}

DexProto* update_proto_reference(
    const DexProto* proto,
    const std::unordered_map<const DexType*, DexType*>& old_to_new) {
  auto rtype = get_array_type_or_self(proto->get_rtype());
  if (old_to_new.count(rtype) > 0) {
    auto merger_type = old_to_new.at(rtype);
    rtype = is_array(proto->get_rtype()) ? make_array_type(merger_type)
                                         : const_cast<DexType*>(merger_type);
  } else {
    rtype = proto->get_rtype();
  }
  std::deque<DexType*> lst;
  for (const auto arg_type : proto->get_args()->get_type_list()) {
    auto extracted_arg_type = get_array_type_or_self(arg_type);
    if (old_to_new.count(extracted_arg_type) > 0) {
      auto merger_type = old_to_new.at(extracted_arg_type);
      auto new_arg_type = is_array(arg_type)
                              ? make_array_type(merger_type)
                              : const_cast<DexType*>(merger_type);
      lst.push_back(new_arg_type);
    } else {
      lst.push_back(arg_type);
    }
  }

  return DexProto::make_proto(const_cast<DexType*>(rtype),
                              DexTypeList::make_type_list(std::move(lst)));
}

DexTypeList* prepend_and_make(const DexTypeList* list, DexType* new_type) {
  auto old_list = list->get_type_list();
  auto prepended = std::deque<DexType*>(old_list.begin(), old_list.end());
  prepended.push_front(new_type);
  return DexTypeList::make_type_list(std::move(prepended));
}

DexTypeList* append_and_make(const DexTypeList* list, DexType* new_type) {
  auto old_list = list->get_type_list();
  auto appended = std::deque<DexType*>(old_list.begin(), old_list.end());
  appended.push_back(new_type);
  return DexTypeList::make_type_list(std::move(appended));
}

DexTypeList* append_and_make(const DexTypeList* list,
                             const std::vector<DexType*>& new_types) {
  auto old_list = list->get_type_list();
  auto appended = std::deque<DexType*>(old_list.begin(), old_list.end());
  appended.insert(appended.end(), new_types.begin(), new_types.end());
  return DexTypeList::make_type_list(std::move(appended));
}

DexTypeList* replace_head_and_make(const DexTypeList* list, DexType* new_head) {
  auto old_list = list->get_type_list();
  auto new_list = std::deque<DexType*>(old_list.begin(), old_list.end());
  always_assert(!new_list.empty());
  new_list.pop_front();
  new_list.push_front(new_head);
  return DexTypeList::make_type_list(std::move(new_list));
}

void update_method_signature_type_references(
    const Scope& scope,
    const std::unordered_map<const DexType*, DexType*>& old_to_new) {
  std::unordered_map<DexMethod*, DexProto*> colliding_candidates;
  TypeSet mergeables;
  for (const auto& pair : old_to_new) {
    mergeables.insert(pair.first);
  }

  const auto update_sig = [&](DexMethod* meth) {
    auto proto = meth->get_proto();
    if (!proto_has_reference_to(proto, mergeables)) {
      return;
    }
    auto new_proto = update_proto_reference(proto, old_to_new);
    DexMethodSpec spec;
    spec.proto = new_proto;
    if (!is_init(meth) && !meth->is_virtual()) {
      TRACE(REFU, 9, "sig: updating non ctor/virt method %s\n", SHOW(meth));
      meth->change(spec, true);
      return;
    }

    auto type = meth->get_class();
    auto name = meth->get_name();
    auto existing_meth = DexMethod::get_method(type, name, new_proto);
    if (existing_meth == nullptr) {
      TRACE(REFU, 9, "sig: updating method %s\n", SHOW(meth));
      meth->change(spec, true);
      return;
    }

    TRACE(REFU,
          9,
          "sig: found colliding candidate %s with %s\n",
          SHOW(existing_meth),
          SHOW(meth));
    colliding_candidates[meth] = new_proto;
  };

  walk::methods(scope, update_sig);

  if (colliding_candidates.empty()) {
    return;
  }

  // Compute virtual scopes and filter out non-virtuals.
  // Perform simple signature update and renaming for non-virtuals.
  auto non_virtuals = devirtualize(scope);
  std::unordered_set<DexMethod*> non_virt_set(non_virtuals.begin(),
                                              non_virtuals.end());
  std::unordered_map<DexMethod*, DexProto*> colliding_methods;
  for (const auto& pair : colliding_candidates) {
    auto meth = pair.first;
    auto new_proto = pair.second;
    if (non_virt_set.count(meth) > 0) {
      DexMethodSpec spec;
      spec.proto = new_proto;
      TRACE(REFU, 9, "sig: updating non virt method %s\n", SHOW(meth));
      meth->change(spec, true);
      continue;
    }
    // We cannot handle the renaming of true virtuals.
    always_assert_log(!meth->is_virtual(),
                      "sig: found true virtual colliding method %s\n",
                      SHOW(meth));
    colliding_methods[meth] = new_proto;
  }
  // Last resort. Fix colliding methods for non-virtuals.
  fix_colliding_method(scope, colliding_methods);
}

void update_field_type_references(
    const Scope& scope,
    const std::unordered_map<const DexType*, DexType*>& old_to_new) {
  TRACE(REFU, 4, " updating field refs\n");
  const auto update_fields = [&](const std::vector<DexField*>& fields) {
    for (const auto field : fields) {
      const auto ref_type = field->get_type();
      const auto type =
          is_array(ref_type) ? get_array_type(ref_type) : ref_type;
      if (old_to_new.count(type) == 0) {
        continue;
      }
      DexFieldSpec spec;
      auto new_type = old_to_new.at(type);
      auto new_type_incl_array =
          is_array(ref_type) ? make_array_type(new_type) : new_type;
      spec.type = new_type_incl_array;
      field->change(spec);
      TRACE(REFU, 9, " updating field ref to %s\n", SHOW(type));
    }
  };

  for (const auto cls : scope) {
    update_fields(cls->get_ifields());
    update_fields(cls->get_sfields());
  }
}

} // namespace type_reference
