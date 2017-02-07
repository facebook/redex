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
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "Transform.h"
#include "Walkers.h"

static size_t unhandled_inline = 0;

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
 * TODO: wide instructions
 */
static bool validate_const_for_ev(DexInstruction* op) {
  if (!is_const(op->opcode())) {
    return false;
  }
  switch (op->opcode()) {
  case OPCODE_CONST_4:
  case OPCODE_CONST_16:
  case OPCODE_CONST:
  case OPCODE_CONST_STRING:
  case OPCODE_CONST_STRING_JUMBO:
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
    DexEncodedValue *ev;
    if (const_op->has_strings()) {
      auto str_op = static_cast<DexOpcodeString*>(const_op);
      auto str = str_op->get_string();
      ev = new DexEncodedValueString(str);
    } else {
      ev = DexEncodedValue::zero_for_type(field->get_type());
      ev->value((uint64_t) const_op->literal());
    }
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
  inline_field_values(scope);
  remove_unused_fields(scope, m_remove_class_members, m_keep_class_members);
}

static FinalInlinePass s_pass;
