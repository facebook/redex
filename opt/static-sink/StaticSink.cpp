/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StaticSink.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Creators.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "Resolver.h"
#include "ConfigFiles.h"
#include "ReachableClasses.h"
#include "Walkers.h"
#include "Warning.h"
#include "ClassHierarchy.h"

////////////////////////////////////////////////////////////////////////////////

namespace {

/*
 * Parse a string representing a type list.  Assumes the same format used by dexdump, e.g.,
 * "[[ILjava/lang/String;B" would become (int[][], String, boolean)
 */
DexTypeList* parse_type_list_string(const char* str) {
  std::deque<DexType*> type_list;
  const char* p = str;
  while (*p != '\0') {
    if (*p == 'L') {
      auto end = strchr(p, ';');
      type_list.push_back(
        DexType::get_type(std::string(p, end - p + 1).c_str()));
      p = end + 1;
    } else if (*p == '[') {
      auto end = p + 1;
      while (*end == '[') {
        end++;
      }
      if (*end == 'L') {
        auto clsend = strchr(end, ';');
        type_list.push_back(
          DexType::get_type(std::string(p, clsend - p + 1).c_str()));
        p = clsend + 1;
      } else {
        type_list.push_back(
          DexType::get_type(std::string(p, end - p + 1).c_str()));
        p = end + 1;
      }
    } else {
      type_list.push_back(DexType::get_type(std::string(p, 1).c_str()));
      p++;
    }
    // check if any get_type generated a nullptr
    if (type_list.back() == nullptr) {
      return nullptr;
    }
  }
  return DexTypeList::make_type_list(std::move(type_list));
}

/*
 * Parse a vector of strings into the corresponding DexMethods.
 */
std::unordered_set<DexMethod*> strings_to_dexmethods(
  std::vector<std::string> method_list
) {
  std::unordered_set<DexMethod*> methods;
  for (auto const& mstr : method_list) {
    // Format: class.method(arglist)rtype
    auto dot = mstr.find('.');
    auto lparen = mstr.find('(');
    auto rparen = mstr.find(')');

    if (dot == std::string::npos ||
        lparen == std::string::npos ||
        rparen == std::string::npos) {
      opt_warn(COLDSTART_STATIC, "%s\n", mstr.c_str());
      continue;
    }
    auto classpart = mstr.substr(0, dot);
    auto methodpart = mstr.substr(dot + 1, lparen - dot - 1);
    auto arglistpart = mstr.substr(lparen + 1, rparen - lparen - 1);
    auto rtypepart = mstr.substr(rparen + 1, mstr.length() - rparen - 1);

    auto classtype = DexType::get_type(classpart.c_str());
    auto methodname = DexString::get_string(methodpart.c_str());
    auto arglist = parse_type_list_string(arglistpart.c_str());
    auto rtype = DexType::get_type(rtypepart.c_str());

    if (!classtype || !methodname || !arglist || !rtype) {
      opt_warn(COLDSTART_STATIC, "%s\n", mstr.c_str());
      continue;
    }
    auto proto = DexProto::get_proto(rtype, arglist);
    if (!proto) {
      opt_warn(COLDSTART_STATIC, "%s\n", mstr.c_str());
      continue;
    }
    auto method = DexMethod::get_method(classtype, methodname, proto);
    if (!method || !method->is_def()) {
      opt_warn(COLDSTART_STATIC, "%s\n", mstr.c_str());
      continue;
    }
    methods.insert(static_cast<DexMethod*>(method));
  }
  return methods;
}

std::vector<DexClass*> get_coldstart_classes(const DexClassesVector& dexen,
                                             ConfigFiles& conf) {
  auto interdex_list = conf.get_coldstart_classes();
  std::unordered_map<std::string, DexClass*> class_string_map;
  std::vector<DexClass*> coldstart_classes;
  for (auto const& dex : dexen) {
    for (auto const& cls : dex) {
      class_string_map[cls->get_type()->get_name()->str()] = cls;
    }
  }
  for (auto const& class_string : interdex_list) {
    if (class_string_map.count(class_string)) {
      coldstart_classes.push_back(class_string_map[class_string]);
    }
  }
  return coldstart_classes;
}

std::vector<DexMethod*> get_noncoldstart_statics(
    const std::vector<DexClass*>& classes,
    const std::unordered_set<DexMethod*>& coldstart_methods) {
  std::vector<DexMethod*> noncold_methods;
  int keep_statics = 0;
  for (auto const& cls : classes) {
    for (auto& method : cls->get_dmethods()) {
      if (is_static(method)) {
        if (!is_clinit(method) &&
            coldstart_methods.count(method) == 0 &&
            can_delete(cls) &&
            can_delete(method)) {
          noncold_methods.push_back(method);
        } else {
          keep_statics++;
        }
      }
    }
  }
  TRACE(SINK, 1,
          "statics that are used (or can't be moved): %d\n",
          keep_statics);
  return noncold_methods;
}

void remove_primary_dex_refs(
    const DexClasses& primary_dex,
    std::vector<DexMethod*>& statics) {
  std::unordered_set<DexMethod*> ref_set;
  walk::opcodes(
    primary_dex,
    [](DexMethod*) { return true; },
    [&](DexMethod*, IRInstruction* insn) {
      if (insn->has_method()) {
        auto callee =
            resolve_method(insn->get_method(), opcode_to_search(insn));
        if (callee != nullptr) {
          ref_set.insert(callee);
        }
      }
    }
  );
  statics.erase(
    std::remove_if(
      statics.begin(), statics.end(),
      [&](DexMethod* m) { return ref_set.count(m); }),
    statics.end());
}

bool allow_field_access(DexFieldRef* field) {
  auto fieldcls = type_class(field->get_class());
  if (!field->is_concrete()) {
    return false;
  }
  always_assert_log(fieldcls, "Undefined class for field %s\n", SHOW(field));
  if (!fieldcls->has_class_data()) {
    return false;
  }
  set_public(fieldcls);
  set_public(static_cast<DexField*>(field));
  return true;
}

bool allow_method_access(DexMethod* meth) {
  if (!meth->is_concrete()) {
    return false;
  }
  if (!is_static(meth) &&
      !is_constructor(meth) &&
      !is_public(meth)) {
    return false;
  }
  auto methcls = type_class(meth->get_class());
  always_assert_log(methcls, "Undefined class for method %s\n", SHOW(meth));
  if (!methcls->has_class_data()) {
    return false;
  }
  set_public(methcls);
  set_public(meth);
  return true;
}

bool allow_type_access(DexType* type) {
  if (is_array(type)) {
    type = get_array_type(type);
  }
  auto typecls = type_class(type);
  if (!typecls) {
    return true;
  }
  if (!typecls->has_class_data()) {
    return false;
  }
  set_public(typecls);
  return true;
}

bool illegal_access(DexMethod* method) {
  auto code = method->get_code();
  if (!code) {
    return true;
  }
  auto proto = method->get_proto();
  if (!allow_type_access(proto->get_rtype())) {
    return true;
  }
  for (auto paramtype : proto->get_args()->get_type_list()) {
    if (!allow_type_access(paramtype)) {
      return true;
    }
  }
  for (auto const& mie : InstructionIterable(code)) {
    auto op = mie.insn;
    if (op->opcode() == OPCODE_INVOKE_SUPER) {
      return true;
    }
    if (op->has_field()) {
      auto field = op->get_field();
      if (!allow_field_access(field)) {
        return true;
      }
    }
    if (op->has_method()) {
      auto meth = resolve_method(op->get_method(), opcode_to_search(op));
      if (meth != nullptr && !allow_method_access(meth)) {
        return true;
      }
    }
    if (op->has_type()) {
      auto type = op->get_type();
      if (!allow_type_access(type)) {
        return true;
      }
    }
  }
  return false;
}

DexClass* move_statics_out(
    const ClassHierarchy& ch,
    const std::vector<DexMethod*>& statics,
    const std::unordered_map<DexMethod*, DexClass*>& sink_map) {
  auto holder_type = DexType::make_type("Lredex/Static$Holder;");
  ClassCreator cc(holder_type);
  cc.set_access(ACC_PUBLIC);
  cc.set_super(get_object_type());
  auto holder = cc.create();

  long moved_count = 0;
  int collision_count = 0;
  int native_count = 0;
  int access_count = 0;
  for (auto& meth : statics) {
    auto it = sink_map.find(meth);
    auto sink_class = it == sink_map.end() ? holder : it->second;
    if (find_collision(
            ch, meth->get_name(), meth->get_proto(), sink_class, false)) {
      collision_count++;
      continue;
    }
    if (is_native(meth)) {
      native_count++;
      continue;
    }
    if (illegal_access(meth)) {
      access_count++;
      continue;
    }
    TRACE(SINK, 2, "sink %s to %s\n", SHOW(meth), SHOW(sink_class));
    type_class(meth->get_class())->remove_method(meth);
    DexMethodSpec spec;
    spec.cls = sink_class->get_type();
    meth->change(spec,
                 false /* rename on collision */,
                 false /* update deobfuscated name */);
    set_public(meth);
    sink_class->add_method(meth);
    moved_count++;
  }

  TRACE(SINK, 1,
    "cannot move:\n"
    "  collision: %-d\n"
    "  native:    %-d\n"
    "  access:    %-d\n",
    collision_count, native_count, access_count);
  TRACE(SINK, 1, "moved %lu methods\n", moved_count);
  return holder;
}

std::unordered_map<DexMethod*, DexClass*> get_sink_map(
    DexStoresVector& stores,
    const std::vector<DexClass*>& classes,
    const std::vector<DexMethod*>& statics) {
  std::unordered_map<DexMethod*, DexClass*> statics_to_callers;
  std::unordered_set<DexClass*> class_set(classes.begin(), classes.end());
  std::unordered_set<DexMethod*> static_set(statics.begin(), statics.end());
  auto scope = build_class_scope(stores);
  walk::opcodes(
    scope,
    [&](DexMethod* m) {
      auto cls = type_class(m->get_class());
      return class_set.count(cls) == 0 && is_public(cls);
    },
    [&](DexMethod* m, IRInstruction* insn) {
      if (insn->has_method()) {
        auto callee =
            resolve_method(insn->get_method(), opcode_to_search(insn));
        if (callee != nullptr && static_set.count(callee)) {
          statics_to_callers[callee] = type_class(m->get_class());
        }
      }
    }
  );
  return statics_to_callers;
}

void count_coldstart_statics(const std::vector<DexClass*>& classes) {
  int num_statics = 0;
  int num_dmethods = 0;
  int num_vmethods = 0;
  for (auto const cls : classes) {
    for (auto const m : cls->get_dmethods()) {
      num_dmethods++;
      if (is_static(m)) {
        num_statics++;
      }
    }
    num_vmethods += cls->get_vmethods().size();
  }
  TRACE(SINK, 1, "statics in coldstart classes: %d\n", num_statics);
  TRACE(SINK, 1, "dmethods in coldstart classes: %d\n", num_dmethods);
  TRACE(SINK, 1, "vmethods in coldstart classes: %d\n", num_vmethods);
}

}

void StaticSinkPass::run_pass(DexStoresVector& stores,
                              ConfigFiles& conf,
                              PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(SINK, 1, "StaticSinkPass not run because no ProGuard configuration was provided.");
    return;
  }
  ClassHierarchy ch = build_type_hierarchy(build_class_scope(stores));
  DexClassesVector& root_store = stores[0].get_dexen();
  auto method_list = conf.get_coldstart_methods();
  auto methods = strings_to_dexmethods(method_list);
  TRACE(SINK, 1, "methods used in coldstart: %lu\n", methods.size());
  auto coldstart_classes = get_coldstart_classes(root_store, conf);
  count_coldstart_statics(coldstart_classes);
  auto statics = get_noncoldstart_statics(coldstart_classes, methods);
  TRACE(SINK, 1, "statics not used in coldstart: %lu\n", statics.size());
  remove_primary_dex_refs(root_store[0], statics);
  TRACE(SINK, 1, "statics after removing primary dex: %lu\n", statics.size());
  auto sink_map = get_sink_map(stores, coldstart_classes, statics);
  TRACE(SINK, 1, "statics with sinkable callsite: %lu\n", sink_map.size());
  auto holder = move_statics_out(ch, statics, sink_map);
  TRACE(SINK, 1, "methods in static holder: %lu\n",
          holder->get_dmethods().size());
  DexClasses dc(1);
  dc.at(0) = holder;
  root_store.emplace_back(std::move(dc));
}

static StaticSinkPass s_pass;
