/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "FinalInline.h"

#include <stdio.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Debug.h"
#include "DexAccess.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "Transform.h"
#include "Walkers.h"

static size_t unhandled_inline = 0;

static bool validate_sput_for_ev(DexClass* clazz, DexInstruction* op);

std::unordered_set<DexField*> get_called_field_defs(Scope& scope) {
  std::vector<DexField*> field_refs;
  walk_methods(scope,
               [&](DexMethod* method) { method->gather_fields(field_refs); });
  sort_unique(field_refs);
  /* Okay, now we have a complete list of field refs
   * for this particular dex.  Map to the def actually invoked.
   */
  std::unordered_set<DexField*> field_defs;
  for (auto field_ref : field_refs) {
    auto field_def = resolve_field(field_ref);
    if (field_def == nullptr || !field_def->is_concrete()) continue;
    field_defs.insert(field_def);
  }
  return field_defs;
}

std::unordered_set<DexField*> get_field_target(
    Scope& scope, const std::vector<DexField*>& fields) {
  std::unordered_set<DexField*> field_defs = get_called_field_defs(scope);
  std::unordered_set<DexField*> ftarget;
  for (auto field : fields) {
    if (field_defs.count(field) > 0) {
      ftarget.insert(field);
    }
  }
  return ftarget;
}

bool keep_member(
  const std::vector<std::string>& keep_members,
  const DexField* field
) {
  for (auto const& keep : keep_members) {
    if (!strcmp(keep.c_str(), field->get_name()->c_str())) {
      return true;
    }
  }
  return false;
}

void remove_unused_fields(
  Scope& scope,
  const std::vector<std::string>& remove_members,
  const std::vector<std::string>& keep_members
) {
  std::vector<DexField*> moveable_fields;
  std::vector<DexClass*> smallscope;
  uint32_t aflags = ACC_STATIC | ACC_FINAL;
  for (auto clazz : scope) {
    bool found = can_delete(clazz);
    if (!found) {
      auto name = clazz->get_name()->c_str();
      for (const auto& name_prefix : remove_members) {
        if (strstr(name, name_prefix.c_str()) != nullptr) {
          found = true;
          break;
        }
      }
      if (!found) {
        TRACE(FINALINLINE, 2, "Cannot delete: %s\n", SHOW(clazz));
        continue;
      }
    }
    auto sfields = clazz->get_sfields();
    for (auto sfield : sfields) {
      if (keep_member(keep_members, sfield)) continue;
      if ((sfield->get_access() & aflags) != aflags) continue;
      auto value = sfield->get_static_value();
      if (value == nullptr && !is_primitive(sfield->get_type())) continue;
      if (!found && !can_delete(sfield)) continue;

      moveable_fields.push_back(sfield);
      smallscope.push_back(clazz);
    }
  }
  sort_unique(smallscope);

  std::unordered_set<DexField*> field_target =
      get_field_target(scope, moveable_fields);
  std::unordered_set<DexField*> dead_fields;
  for (auto field : moveable_fields) {
    if (field_target.count(field) == 0) {
      dead_fields.insert(field);
    }
  }
  TRACE(FINALINLINE, 1,
          "Removable fields %lu/%lu\n",
          dead_fields.size(),
          moveable_fields.size());
  TRACE(FINALINLINE, 1, "Unhandled inline %ld\n", unhandled_inline);

  for (auto clazz : smallscope) {
    auto& sfields = clazz->get_sfields();
    sfields.erase(std::remove_if(sfields.begin(), sfields.end(),
      [&](DexField* field) {
      return dead_fields.count(field) > 0;
    }), sfields.end());
  }
}

static bool check_sget(DexOpcodeField* opfield) {
  auto opcode = opfield->opcode();
  switch (opcode) {
  case OPCODE_SGET_WIDE:
    unhandled_inline++;
    return false;
  case OPCODE_SGET:
  case OPCODE_SGET_BOOLEAN:
  case OPCODE_SGET_BYTE:
  case OPCODE_SGET_CHAR:
  case OPCODE_SGET_SHORT:
    return true;
  default:
    return false;
  }
}

static bool validate_sget(DexMethod* context, DexOpcodeField* opfield) {
  auto opcode = opfield->opcode();
  switch (opcode) {
  case OPCODE_SGET_WIDE:
    unhandled_inline++;
    return false;
  case OPCODE_SGET:
  case OPCODE_SGET_BOOLEAN:
  case OPCODE_SGET_BYTE:
  case OPCODE_SGET_CHAR:
  case OPCODE_SGET_SHORT:
    return true;
  default:
    auto field = resolve_field(opfield->field(), FieldSearch::Static);
    always_assert_log(field->is_concrete(), "Must be a concrete field");
    auto value = field->get_static_value();
    always_assert_log(
        false,
        "Unexpected field type in inline_*sget %s for field %s value %s in "
        "method %s\n",
        SHOW(opfield),
        SHOW(field),
        value != nullptr ? value->show().c_str() : "('nullptr')",
        SHOW(context));
  }
  return false;
}

void replace_opcode(DexMethod* method, DexInstruction* from, DexInstruction* to) {
  MethodTransform* mt = MethodTransform::get_method_transform(method);
  mt->replace_opcode(from, to);
}

void inline_cheap_sget(DexMethod* method, DexOpcodeField* opfield) {
  if (!validate_sget(method, opfield)) return;
  auto dest = opfield->dest();
  auto field = resolve_field(opfield->field(), FieldSearch::Static);
  always_assert_log(field->is_concrete(), "Must be a concrete field");
  auto value = field->get_static_value();
  /* FIXME for sget_wide case */
  uint32_t v = value != nullptr ? (uint32_t)value->value() : 0;
  auto opcode = [&] {
    if ((v & 0xffff) == v) {
      return OPCODE_CONST_16;
    } else if ((v & 0xffff0000) == v) {
      return OPCODE_CONST_HIGH16;
    }
    always_assert_log(false,
                      "Bad inline_cheap_sget queued up, can't fit to"
                      " CONST_16 or CONST_HIGH16, bailing\n");
  }();

  auto newopcode = (new DexInstruction(opcode, 0))->set_dest(dest)->set_literal(v);
  replace_opcode(method, opfield, newopcode);
}

void inline_sget(DexMethod* method, DexOpcodeField* opfield) {
  if (!validate_sget(method, opfield)) return;
  auto opcode = OPCODE_CONST;
  auto dest = opfield->dest();
  auto field = resolve_field(opfield->field(), FieldSearch::Static);
  always_assert_log(field->is_concrete(), "Must be a concrete field");
  auto value = field->get_static_value();
  /* FIXME for sget_wide case */
  uint32_t v = value != nullptr ? (uint32_t)value->value() : 0;

  auto newopcode = (new DexInstruction(opcode))->set_dest(dest)->set_literal(v);
  replace_opcode(method, opfield, newopcode);
}

/*
 * There's no "good way" to differentiate blank vs. non-blank
 * finals.  So, we just scan the code in the CL-init.  If
 * it's sput there, then it's a blank.  Lame, agreed, but functional.
 *
 */
void get_sput_in_clinit(DexClass* clazz,
                        std::unordered_map<DexField*, bool>& blank_statics) {
  auto clinit = clazz->get_clinit();
  if (clinit == nullptr) {
    return;
  }
  always_assert_log(is_static(clinit) && is_constructor(clinit),
                    "static constructor doesn't have the proper access bits set\n");
  auto& code = clinit->get_code();
  auto opcodes = code->get_instructions();
  for (auto opcode : opcodes) {
    if (opcode->has_fields() && is_sput(opcode->opcode())) {
      auto fieldop = static_cast<DexOpcodeField*>(opcode);
      auto field = resolve_field(fieldop->field(), FieldSearch::Static);
      if (field == nullptr || !field->is_concrete()) continue;
      if (field->get_class() != clazz->get_type()) continue;
      blank_statics[field] = true;
    }
  }
}

void inline_field_values(Scope& fullscope) {
  std::unordered_set<DexField*> inline_field;
  std::unordered_set<DexField*> cheap_inline_field;
  std::vector<DexClass*> scope;
  uint32_t aflags = ACC_STATIC | ACC_FINAL;
  for (auto clazz : fullscope) {
    std::unordered_map<DexField*, bool> blank_statics;
    get_sput_in_clinit(clazz, blank_statics);
    auto sfields = clazz->get_sfields();
    for (auto sfield : sfields) {
      if ((sfield->get_access() & aflags) != aflags) continue;
      if (blank_statics[sfield]) continue;
      auto value = sfield->get_static_value();
      if (value == nullptr && !is_primitive(sfield->get_type())) {
        continue;
      }
      if (value != nullptr && !value->is_evtype_primitive()) {
        continue;
      }
      uint64_t v = value != nullptr ? value->value() : 0;
      if ((v & 0xffff) == v || (v & 0xffff0000) == v) {
        cheap_inline_field.insert(sfield);
      }
      inline_field.insert(sfield);
      scope.push_back(clazz);
    }
  }
  std::vector<std::pair<DexMethod*, DexOpcodeField*>> cheap_rewrites;
  std::vector<std::pair<DexMethod*, DexOpcodeField*>> simple_rewrites;
  walk_opcodes(
      fullscope,
      [](DexMethod* method) { return true; },
      [&](DexMethod* method, DexInstruction* insn) {
        if (insn->has_fields() && is_sfield_op(insn->opcode())) {
          auto fieldop = static_cast<DexOpcodeField*>(insn);
          auto field = resolve_field(fieldop->field(), FieldSearch::Static);
          if (field == nullptr || !field->is_concrete()) return;
          if (inline_field.count(field) == 0) return;
          if (cheap_inline_field.count(field) > 0) {
            cheap_rewrites.push_back(std::make_pair(method, fieldop));
            return;
          }
          simple_rewrites.push_back(std::make_pair(method, fieldop));
        }
      });
  TRACE(FINALINLINE, 1,
          "Method Re-writes Cheap %lu  Simple %lu\n",
          cheap_rewrites.size(),
          simple_rewrites.size());
  for (auto cheapcase : cheap_rewrites) {
    inline_cheap_sget(cheapcase.first, cheapcase.second);
  }
  for (auto simplecase : simple_rewrites) {
    inline_sget(simplecase.first, simplecase.second);
  }
  MethodTransform::sync_all();
}

/*
 * Verify that we can handle converting the literal contained in the
 * const op into an encoded value.
 *
 * TODO: Strings and wide
 */
static bool validate_const_for_ev(DexInstruction* op) {
  if (!is_const(op->opcode())) {
    return false;
  }
  switch (op->opcode()) {
  case OPCODE_CONST_4:
  case OPCODE_CONST_16:
  case OPCODE_CONST:
    return true;
  default:
    return false;
  }
}

/*
 * Verify that we can convert the field in the sput into an encoded value.
 */
static bool validate_sput_for_ev(DexClass* clazz, DexInstruction* op) {
  if (!(op->has_fields() && is_sput(op->opcode()))) {
    return false;
  }
  auto fieldop = static_cast<DexOpcodeField*>(op);
  auto field = resolve_field(fieldop->field(), FieldSearch::Static);
  return (field != nullptr) && (field->get_class() == clazz->get_type());
}

/*
 * Attempt to replace the clinit with corresponding encoded values.
 */
static bool try_replace_clinit(DexClass* clazz, DexMethod* clinit) {
  auto& code = clinit->get_code();
  auto opcodes = code->get_instructions();

  // If there are an odd number of insns the last must be a return
  if ((opcodes.size() % 2 != 0) && !is_return(opcodes.back()->opcode())) {
    return false;
  }

  // Verify opcodes are (const, sput)* pairs
  for (size_t i = 0; i < opcodes.size() - 1; i += 2) {
    auto const_op = opcodes[i];
    auto sput_op = opcodes[i + 1];
    if (!(validate_const_for_ev(const_op) &&
          validate_sput_for_ev(clazz, sput_op) &&
          (const_op->dest() == sput_op->src(0)))) {
      return false;
    }
  }

  // Attach encoded values and remove the clinit
  for (size_t i = 0; i < opcodes.size() - 1; i += 2) {
    auto const_op = opcodes[i];
    auto sput_op = opcodes[i + 1];
    auto fieldop = static_cast<DexOpcodeField*>(sput_op);
    auto field = resolve_field(fieldop->field(), FieldSearch::Static);
    auto ev = DexEncodedValue::zero_for_type(field->get_type());
    ev->value((uint64_t) const_op->literal());
    field->make_concrete(field->get_access(), ev);
  }
  clazz->remove_method(clinit);

  return true;
}

static size_t replace_encodable_clinits(Scope& fullscope) {
  size_t nreplaced = 0;
  size_t ntotal = 0;
  for (auto clazz : fullscope) {
    auto clinit = clazz->get_clinit();
    if (clinit == nullptr) {
      continue;
    }
    ntotal++;
    if (try_replace_clinit(clazz, clinit)) {
      TRACE(FINALINLINE, 2, "Replaced clinit for class %s with encoded values\n", SHOW(clazz));
      nreplaced++;
    }
  }
  MethodTransform::sync_all();
  TRACE(FINALINLINE, 1, "Replaced %lu/%lu clinits with encoded values\n", nreplaced, ntotal);
  return nreplaced;
}

struct FieldDependency {
  DexMethod *clinit;
  DexInstruction *sget;
  DexInstruction *sput;
  DexField *field;

  FieldDependency(DexMethod *clinit, DexInstruction *sget, DexInstruction *sput,
                  DexField *field) : clinit(clinit), sget(sget), sput(sput), field(field)
  {}
};

/*
 * Attempt to propagate constant values that are known only after the APK has been
 * created. Our build process can result in situation where javac sees something
 * resembling:
 *
 *   class Parent {
 *     public static int CONST = 0;
 *   }
 *
 *   class Child {
 *     public static final CONST = Parent.CONST;
 *   }
 *
 * Parent.CONST is not final, so javac cannot perform constant propagation. However,
 * Parent.CONST may be marked final when we package the APK, thereby opening up an
 * opportunity for constant propagation by redex.
 */
size_t FinalInlinePass::propagate_constants(Scope& fullscope) {
  // Build dependency map (static -> [statics] that depend on it)
  TRACE(FINALINLINE, 2, "Building dependency map\n");
  std::unordered_map<DexField*, std::unique_ptr<std::vector<FieldDependency>>> deps;
  for (auto clazz : fullscope) {
    auto clinit = clazz->get_clinit();
    if (clinit == nullptr) {
      continue;
    }
    auto& code = clinit->get_code();
    auto opcodes = code->get_instructions();
    if (opcodes.size() == 0) {
      continue;
    }
    for (size_t i = 0; i < opcodes.size() - 1; ++i) {
      // Check for sget from static final
      if (!opcodes[i]->has_fields()) {
        continue;
      }
      auto sget_op = static_cast<DexOpcodeField*>(opcodes[i]);
      if (!check_sget(sget_op)) {
        continue;
      }
      auto src_field = resolve_field(sget_op->field(), FieldSearch::Static);
      if ((src_field == nullptr) ||
          !(is_static(src_field) && is_final(src_field))) {
        continue;
      }

      // Check for sput to static final
      if (!validate_sput_for_ev(clazz, opcodes[i + 1])) {
        continue;
      }
      auto sput_op = static_cast<DexOpcodeField*>(opcodes[i + 1]);
      auto dst_field = resolve_field(sput_op->field(), FieldSearch::Static);
      if (!(is_static(dst_field) && is_final(dst_field))) {
        continue;
      }

      // Check that dst register for sget is src register for sput
      if (sget_op->dest() != sput_op->src(0)) {
        continue;
      }

      // Check that source register is either overwritten or isn't used
      // again. This ensures we can safely remove the opcode pair without
      // breaking future instructions that rely on the value of the source
      // register.  Yes, this means we're N^2 in theory, but hopefully in
      // practice we don't approach that.
      bool src_reg_reused = false;
      for (size_t j = i + 2; j < opcodes.size() && !src_reg_reused; ++j) {
        // Check if the source register is overwritten
        if (opcodes[j]->dests_size() > 0 &&
            opcodes[j]->dest() == sget_op->dest() &&
            !opcodes[j]->dest_is_src()) {
          break;
        }
        // Check if the source register is reused as the source for another
        // instruction
        for (size_t r = 0; r < opcodes[j]->srcs_size(); ++r) {
          if (opcodes[j]->src(r) == sget_op->dest()) {
            src_reg_reused = true;
          }
        }
      }
      if (src_reg_reused) {
        TRACE(FINALINLINE, 2, "Cannot propagate %s to %s. Source register reused.\n", SHOW(src_field), SHOW(dst_field));
        continue;
      }

      // Yay, we found a dependency!
      if (deps.count(src_field) == 0) {
        deps[src_field] = std::make_unique<std::vector<FieldDependency>>();
      }
      TRACE(FINALINLINE, 2, "Field %s depends on %s\n", SHOW(dst_field), SHOW(src_field));
      FieldDependency dep(clinit, opcodes[i], opcodes[i + 1], dst_field);
      deps[src_field]->push_back(dep);
    }
  }

  // Collect static finals whose values are known. These serve as the starting point
  // of the dependency resolution process.
  std::deque<DexField*> resolved;
  for (auto clazz : fullscope) {
    std::unordered_map<DexField*, bool> blank_statics;
    // TODO: Should we allow static finals that are initialized w/ const, sput?
    get_sput_in_clinit(clazz, blank_statics);
    auto sfields = clazz->get_sfields();
    for (auto sfield : sfields) {
      if (!(is_static(sfield) && is_final(sfield)) || blank_statics[sfield]) {
        continue;
      }
      resolved.push_back(sfield);
    }
  }

  // Resolve dependencies (tsort)
  size_t nresolved = 0;
  while (!resolved.empty()) {
    auto cur = resolved.front();
    resolved.pop_front();
    if (deps.count(cur) == 0) {
      continue;
    }
    auto val = cur->get_static_value();
    for (auto dep : *deps[cur]) {
      dep.field->make_concrete(dep.field->get_access(), val);
      auto mt = MethodTransform::get_method_transform(dep.clinit);
      mt->remove_opcode(dep.sget);
      mt->remove_opcode(dep.sput);
      mt->sync();
      ++nresolved;
      resolved.push_back(dep.field);
      TRACE(FINALINLINE, 2, "Resolved field %s\n", SHOW(dep.field));
    }
  }
  TRACE(FINALINLINE, 1, "Resolved %lu static finals via const prop\n", nresolved);
  return nresolved;
}

void FinalInlinePass::run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(FINALINLINE, 1, "FinalInlinePass not run because no ProGuard configuration was provided.");
    return;
  }
  auto scope = build_class_scope(stores);

  if (m_replace_encodable_clinits) {
    auto nreplaced = replace_encodable_clinits(scope);
    mgr.incr_metric("encodable_clinits_replaced", nreplaced);
  }

  if (m_propagate_static_finals) {
    auto nresolved = propagate_constants(scope);
    mgr.incr_metric("static_finals_resolved", nresolved);
  }

  // Constprop may resolve statics that were initialized via clinit. This opens
  // up another opportunity to remove (potentially empty) clinits.
  if (m_replace_encodable_clinits) {
    auto nreplaced = replace_encodable_clinits(scope);
    mgr.incr_metric("encodable_clinits_replaced", nreplaced);
  }

  inline_field_values(scope);
  remove_unused_fields(scope, m_remove_class_members, m_keep_class_members);
}

static FinalInlinePass s_pass;
