/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WrappedPrimitivesPass.h"

#include <inttypes.h>

#include "DexUtil.h"
#include "Lazy.h"
#include "LiveRange.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"
#include "WrappedPrimitives.h"

namespace cp = constant_propagation;
namespace mog = method_override_graph;
namespace wp = wrapped_primitives;

namespace {

constexpr const char* METRIC_CONSTS_INSERTED = "const_instructions_inserted";
constexpr const char* METRIC_CASTS_INSERTED = "check_casts_inserted";

// Check assumptions about the wrapper class's hierarchy.
void validate_wrapper_type(DexType* type) {
  auto cls = type_class(type);
  always_assert(cls != nullptr);
  always_assert_log(cls->get_interfaces()->empty(),
                    "Wrapper type %s should not implement interfaces",
                    SHOW(type));
  auto super_cls = cls->get_super_class();
  always_assert_log(super_cls == type::java_lang_Object(),
                    "Wrapper type %s should inherit from Object; got %s",
                    SHOW(type), SHOW(super_cls));
}

// A wrapped primitive is assumed to be represented by the only final primitive
// field in the wrapper class.
DexType* get_wrapped_final_field_type(DexType* type) {
  auto cls = type_class(type);
  always_assert_log(cls != nullptr, "Spec class %s not found", SHOW(type));
  std::vector<DexField*> candidates;
  for (auto& f : cls->get_ifields()) {
    if (is_final(f) && type::is_primitive(f->get_type())) {
      candidates.emplace_back(f);
    }
  }
  always_assert_log(candidates.size() == 1,
                    "Expected 1 final field of primitive type in class %s",
                    SHOW(cls));
  return candidates.at(0)->get_type();
}

size_t how_many_fields(const Scope& scope, const DexType* t) {
  size_t result{0};
  walk::fields(scope, [&](DexField* f) {
    if (f->get_type() == t) {
      result++;
    }
  });
  return result;
}

void emit_field_count_metric(const std::string& metric_prefix,
                             const Scope& scope,
                             const std::string& name,
                             const DexType* type,
                             PassManager* mgr) {
  size_t value = type != nullptr ? how_many_fields(scope, type) : 0;
  TRACE(WP, 2, "%s: %zu field(s) of type %s", metric_prefix.c_str(), value,
        name.c_str());
  std::ostringstream name_builder;
  name_builder << metric_prefix << "_" << java_names::internal_to_simple(name)
               << "_fields";
  auto metric_name = name_builder.str();
  mgr->set_metric(metric_name, value);
}
} // namespace

void WrappedPrimitivesPass::bind_config() {
  std::vector<Json::Value> wrappers;
  std::vector<wp::Spec> wrapper_specs;
  bind("wrappers", {}, wrappers);
  for (auto it = wrappers.begin(); it != wrappers.end(); ++it) {
    const auto& value = *it;
    always_assert_log(value.isObject(),
                      "Wrong specification: spec in array not an object.");
    JsonWrapper json_obj = JsonWrapper(value);
    wp::Spec spec;
    std::string wrapper_desc;
    json_obj.get("wrapper", "", wrapper_desc);
    spec.wrapper = DexType::get_type(wrapper_desc);
    if (spec.wrapper == nullptr) {
      TRACE(WP, 2, "Spec type %s does not exist; skipping.",
            wrapper_desc.c_str());
      continue;
    }
    // Ensure the wrapper type matches expectations by the pass.
    validate_wrapper_type(spec.wrapper);
    m_wrapper_type_names.emplace(spec.wrapper->str());
    spec.primitive = get_wrapped_final_field_type(spec.wrapper);

    // Unpack an array of objects, each object is just a 1 key/value to map an
    // API using the wrapper type to the corresponding API of primitive type.
    Json::Value allowed_invokes_array;
    json_obj.get("allowed_invokes", Json::Value(), allowed_invokes_array);
    always_assert_log(
        allowed_invokes_array.isArray(),
        "Wrong specification: allowed_invokes must be an array of objects.");
    for (auto& obj : allowed_invokes_array) {
      always_assert_log(
          obj.isObject(),
          "Wrong specification: allowed_invokes must be an array of objects.");
      auto members = obj.getMemberNames();
      always_assert_log(
          members.size() == 1,
          "Wrong specification: allowed invoke object should be just 1 mapping "
          "of method ref string to method ref string.");
      auto api = members.at(0);
      TRACE(WP, 2, "Checking for API '%s'", api.c_str());
      auto wrapped_api = DexMethod::get_method(api);
      if (wrapped_api == nullptr) {
        continue;
      }
      std::string unwrapped_api_desc;
      JsonWrapper jobj = JsonWrapper(obj);
      jobj.get(api.c_str(), "", unwrapped_api_desc);
      always_assert_log(!unwrapped_api_desc.empty(), "empty!");
      TRACE(WP, 2, "Checking for unwrapped API '%s'",
            unwrapped_api_desc.c_str());
      auto unwrapped_api = DexMethod::get_method(unwrapped_api_desc);
      always_assert_log(unwrapped_api != nullptr, "Method %s does not exist",
                        unwrapped_api_desc.c_str());
      spec.allowed_invokes.emplace(wrapped_api, unwrapped_api);
      TRACE(WP, 2, "Allowed API call %s -> %s", SHOW(wrapped_api),
            SHOW(unwrapped_api));
    }
    wrapper_specs.emplace_back(spec);
  }
  wp::initialize(wrapper_specs);
  trait(Traits::Pass::unique, true);
}

void WrappedPrimitivesPass::eval_pass(DexStoresVector& stores,
                                      ConfigFiles& conf,
                                      PassManager& mgr) {
  auto scope = build_class_scope(stores);
  for (const auto& name : m_wrapper_type_names) {
    emit_field_count_metric("input", scope, name, DexType::get_type(name),
                            &mgr);
  }
  wp::get_instance()->mark_roots();
}

void WrappedPrimitivesPass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& /* unused */,
                                     PassManager& mgr) {
  auto wp_instance = wp::get_instance();
  wp_instance->unmark_roots();

  auto consts = wp_instance->consts_inserted();
  TRACE(WP, 1, "const instructions inserted: %zu", consts);
  mgr.set_metric(METRIC_CONSTS_INSERTED, consts);

  auto casts = wp_instance->casts_inserted();
  TRACE(WP, 1, "check-cast instructions inserted: %zu", casts);
  mgr.set_metric(METRIC_CASTS_INSERTED, casts);

  // Clear state so that no futher work gets done from multiple rounds of IPCP
  wp::initialize({});
}

namespace {
using PreceedingSourceBlockMap =
    std::unordered_map<IRInstruction*, SourceBlock*>;

PreceedingSourceBlockMap build_preceeding_source_block_map(
    cfg::ControlFlowGraph& cfg) {
  PreceedingSourceBlockMap result;
  for (auto b : cfg.blocks()) {
    SourceBlock* preceeding_source_block{nullptr};
    for (const auto& mie : *b) {
      if (mie.type == MFLOW_SOURCE_BLOCK) {
        preceeding_source_block = mie.src_block.get();
      } else if (mie.type == MFLOW_OPCODE) {
        result.emplace(mie.insn, preceeding_source_block);
      }
    }
  }
  return result;
}

void trace_field_usage(const std::string& field_name,
                       const std::string& method_name,
                       IRInstruction* insn,
                       SourceBlock* source_block) {
  if (source_block != nullptr && method_name != source_block->src->c_str()) {
    auto src = source_block->src->c_str();
    TRACE(WP, 2,
          "Note: unoptimized field %s use near "
          "%s or %s",
          field_name.c_str(), method_name.c_str(), src);
  } else {
    auto shown = show_deobfuscated(insn);
    TRACE(WP, 2, "Note: unoptimized field %s use in method %s at %s",
          field_name.c_str(), method_name.c_str(), shown.c_str());
  }
}
} // namespace

void ValidateWrappedPrimitivesPass::run_pass(DexStoresVector& stores,
                                             ConfigFiles& /* unused */,
                                             PassManager& mgr) {
  auto wrapped_primitives_pass = static_cast<WrappedPrimitivesPass*>(
      mgr.find_pass("WrappedPrimitivesPass"));
  if (wrapped_primitives_pass == nullptr) {
    return;
  }
  auto scope = build_class_scope(stores);
  // Look up types that were processed previously by name, in case of rename or
  // complete deletion.
  std::map<std::string, DexType*> wrapper_types_post;
  std::unordered_map<DexType*, std::string> wrapper_types_post_inverse;
  auto find_impl = [&](DexClass* cls, const std::string& name) {
    auto search = wrapped_primitives_pass->m_wrapper_type_names.find(name);
    if (search != wrapped_primitives_pass->m_wrapper_type_names.end()) {
      wrapper_types_post.emplace(name, cls->get_type());
      wrapper_types_post_inverse.emplace(cls->get_type(), name);
      return true;
    }
    return false;
  };
  for (auto& cls : scope) {
    if (!find_impl(cls, cls->get_deobfuscated_name_or_empty_copy())) {
      find_impl(cls, cls->get_name()->str_copy());
    }
  }

  for (auto&& [name, type] : wrapper_types_post) {
    emit_field_count_metric("post", scope, name, type, &mgr);
  }
  // Emit zero values for anything fully deleted.
  for (const auto& name : wrapped_primitives_pass->m_wrapper_type_names) {
    if (wrapper_types_post.count(name) == 0) {
      emit_field_count_metric("post", scope, name, nullptr, &mgr);
    }
  }

  // Stats for how many fields of wrapper types exist in the output program, and
  // how many of those fields were covered by keep rules.
  std::mutex stats_mtx;
  using FieldUsages = std::map<DexField*, size_t, dexfields_comparator>;
  std::map<DexType*, FieldUsages, dextypes_comparator> field_puts;
  std::map<DexType*, FieldUsages, dextypes_comparator> field_gets;
  auto incr_puts = [&](DexField* def) {
    std::lock_guard<std::mutex> lock(stats_mtx);
    field_puts[def->get_type()][def]++;
  };
  auto incr_gets = [&](DexField* def) {
    std::lock_guard<std::mutex> lock(stats_mtx);
    field_gets[def->get_type()][def]++;
  };
  walk::parallel::methods(scope, [&](DexMethod* m) {
    auto code = m->get_code();
    if (code == nullptr) {
      return;
    }
    auto method_name = show_deobfuscated(m);
    auto& cfg = code->cfg();
    Lazy<live_range::LazyLiveRanges> live_ranges(
        [&]() { return std::make_unique<live_range::LazyLiveRanges>(cfg); });
    Lazy<PreceedingSourceBlockMap> sb_lookup(
        [&]() { return build_preceeding_source_block_map(cfg); });
    for (MethodItemEntry& mie : cfg::InstructionIterable(cfg)) {
      if (mie.type != MFLOW_OPCODE) {
        continue;
      }
      auto insn = mie.insn;
      auto opcode = insn->opcode();
      if (opcode == OPCODE_SGET_OBJECT || opcode == OPCODE_SPUT_OBJECT) {
        auto def = insn->get_field()->as_def();
        if (def != nullptr &&
            wrapper_types_post_inverse.count(def->get_type()) > 0) {
          if (opcode == OPCODE_SGET_OBJECT) {
            incr_gets(def);
            if (traceEnabled(WP, 2)) {
              auto search = live_ranges->def_use_chains->find(insn);
              if (search != live_ranges->def_use_chains->end()) {
                // Print some info about immediate usages of the fields. Best
                // effort to give some information that could point the reader
                // to the original location of the usage before optimizations.
                auto field_name = show_deobfuscated(def);
                for (auto& use : search->second) {
                  SourceBlock* preceeding_source_block{nullptr};
                  auto sb = sb_lookup->find(use.insn);
                  if (sb != sb_lookup->end() && sb->second != nullptr) {
                    preceeding_source_block = sb->second;
                  }
                  trace_field_usage(field_name, method_name, insn,
                                    preceeding_source_block);
                }
              }
            }
          } else {
            incr_puts(def);
          }
        }
      }
    }
  });

  for (auto&& [wrapper, map] : field_puts) {
    size_t put_but_unread_count{0};
    size_t keep_count{0};
    for (auto&& [def, puts] : map) {
      const auto& field_gets_map = field_gets[wrapper];
      auto search = field_gets_map.find(def);
      if (search == field_gets_map.end() || search->second == 0) {
        auto shown = show_deobfuscated(def);
        if (!can_delete(def)) {
          keep_count++;
          TRACE(WP, 2, "Field %s was written but not read (keep)!",
                shown.c_str());
        } else {
          TRACE(WP, 2, "Field %s was written but not read!", shown.c_str());
        }
        put_but_unread_count++;
      }
    }
    const auto& name = wrapper_types_post_inverse.at(wrapper);
    auto simple_name = java_names::internal_to_simple(name);
    TRACE(WP, 2, "%s fields that cannot be deleted (keep): %zu", name.c_str(),
          keep_count);
    mgr.set_metric(simple_name + "_field_keeps", keep_count);
    TRACE(WP, 2, "%s fields that were unread: %zu", name.c_str(),
          put_but_unread_count);
    mgr.set_metric(simple_name + "_field_put_but_unread", put_but_unread_count);
  }
}

static WrappedPrimitivesPass s_pass;
static ValidateWrappedPrimitivesPass s_validate_pass;
