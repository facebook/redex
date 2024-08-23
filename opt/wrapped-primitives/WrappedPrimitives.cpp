/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WrappedPrimitives.h"

#include <inttypes.h>

#include "CFGMutation.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationState.h"
#include "ConstantPropagationWholeProgramState.h"
#include "ConstructorParams.h"
#include "DexUtil.h"
#include "InitDeps.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"
#include "WorkQueue.h"

namespace cp = constant_propagation;
namespace wp = wrapped_primitives;

namespace {
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

void validate_api_mapping(DexMethodRef* from, DexMethodRef* to) {
  // Simple validation for now; more involved use cases need to be added later.
  always_assert_log(
      from->get_class() == to->get_class(),
      "Unable to map API from class %s to %s - they are expected to match",
      SHOW(from->get_class()), SHOW(to->get_class()));
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

IROpcode sget_op_for_primitive(DexType* type) {
  always_assert(type::is_primitive(type));
  if (type::is_boolean(type)) {
    return OPCODE_SGET_BOOLEAN;
  } else if (type::is_byte(type)) {
    return OPCODE_SGET_BYTE;
  } else if (type::is_char(type)) {
    return OPCODE_SGET_CHAR;
  } else if (type::is_short(type)) {
    return OPCODE_SGET_SHORT;
  } else if (type::is_int(type) || type::is_float(type)) {
    return OPCODE_SGET;
  } else {
    return OPCODE_SGET_WIDE;
  }
}

IROpcode move_op_for_primitive(DexType* type) {
  always_assert(type::is_primitive(type));
  if (type::is_wide_type(type)) {
    return OPCODE_MOVE_WIDE;
  } else {
    return OPCODE_MOVE;
  }
}

IROpcode move_result_pseudo_op_for_primitive(DexType* type) {
  always_assert(type::is_primitive(type));
  if (type::is_wide_type(type)) {
    return IOPCODE_MOVE_RESULT_PSEUDO_WIDE;
  } else {
    return IOPCODE_MOVE_RESULT_PSEUDO;
  }
}
} // namespace

void WrappedPrimitivesPass::bind_config() {
  std::vector<Json::Value> wrappers;
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
    always_assert_log(spec.wrapper != nullptr, "Type %s does not exist",
                      wrapper_desc.c_str());
    // Ensure the wrapper type matches expectations by the pass.
    validate_wrapper_type(spec.wrapper);
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
      always_assert_log(wrapped_api != nullptr, "Method %s does not exist",
                        api.c_str());
      std::string unwrapped_api_desc;
      JsonWrapper jobj = JsonWrapper(obj);
      jobj.get(api.c_str(), "", unwrapped_api_desc);
      always_assert_log(!unwrapped_api_desc.empty(), "empty!");
      TRACE(WP, 2, "Checking for unwrapped API '%s'",
            unwrapped_api_desc.c_str());
      auto unwrapped_api = DexMethod::get_method(unwrapped_api_desc);
      always_assert_log(unwrapped_api != nullptr, "Method %s does not exist",
                        unwrapped_api_desc.c_str());

      // Make sure this API mapping is not obviously wrong up front.
      validate_api_mapping(wrapped_api, unwrapped_api);
      spec.allowed_invokes.emplace(wrapped_api, unwrapped_api);
      TRACE(WP, 2, "Allowed API call %s -> %s", SHOW(wrapped_api),
            SHOW(unwrapped_api));
    }
    m_wrapper_specs.emplace_back(spec);
  }
  trait(Traits::Pass::unique, true);
}

namespace {
bool has_static_final_wrapper_fields(
    const std::unordered_map<DexType*, wp::Spec>& wrapper_types,
    DexClass* cls) {
  for (auto& f : cls->get_sfields()) {
    if (is_final(f) && wrapper_types.count(f->get_type()) > 0) {
      return true;
    }
  }
  return false;
}

class ClinitMethodAnalysis : public wp::MethodAnalysis {
 public:
  ClinitMethodAnalysis(
      const std::unordered_map<DexType*, wp::Spec>& wrapper_types,
      wp::PassState* pass_state,
      DexClass* cls,
      DexMethod* method)
      : wp::MethodAnalysis(wrapper_types, pass_state, cls, method) {}

  void post_analyze() override {
    // Construct the representation of all fields that were understood and set
    // by the clinit.
    std::unordered_map<DexField*, int64_t> known_fields;
    auto& cfg = get_cfg();
    auto intra_cp = get_fixpoint_iterator();
    auto exit_env = intra_cp->get_exit_state_at(cfg.exit_block());

    m_pass_state->whole_program_state.collect_static_finals(
        m_cls, exit_env.get_field_environment());
    for (auto f : m_cls->get_sfields()) {
      if (m_wrapper_types.count(f->get_type()) > 0) {
        TRACE(WP, 2, "Checking field %s", SHOW(f));
        auto field_value = exit_env.get(f);
        auto maybe_constant = extract_object_attr_value(field_value);
        if (maybe_constant != boost::none) {
          auto constant = *maybe_constant;
          TRACE(WP,
                2,
                "  ==> Field %s is a known object with constant value %" PRId64,
                SHOW(f),
                constant);
          known_fields.emplace(f, constant);
        }
      }
    }

    // Even for understood field values, avoid emitting nodes for fields that
    // could be written to via different instructions/instances. Simplifies
    // later validation logic.
    std::unordered_set<DexField*> visited_fields;
    for (auto* block : cfg.blocks()) {
      for (auto& mie : InstructionIterable(block)) {
        auto insn = mie.insn;
        if (insn->opcode() == OPCODE_SPUT_OBJECT) {
          auto field_def = insn->get_field()->as_def();
          if (field_def != nullptr) {
            auto pair = visited_fields.emplace(field_def);
            if (!pair.second) {
              known_fields.erase(field_def);
              TRACE(WP, 2,
                    "  ==> Field %s written from multiple instructions; will "
                    "not consider",
                    SHOW(field_def));
            }
          }
        }
      }
    }
    // Actual creation of nodes.
    for (auto* block : cfg.blocks()) {
      for (auto& mie : InstructionIterable(block)) {
        auto insn = mie.insn;
        if (insn->opcode() == OPCODE_SPUT_OBJECT) {
          auto field_def = insn->get_field()->as_def();
          if (field_def != nullptr && known_fields.count(field_def) > 0) {
            // Emit a representation of the instructions that created the object
            // in this field.
            auto defs = m_live_ranges->use_def_chains->at({insn, 0});
            TRACE(WP, 2, "  %s -> %zu def(s)", SHOW(mie), defs.size());
            if (defs.size() == 1) {
              auto def_insn = *defs.begin();
              // TODO: Is there any practical way to trigger this assert to fire
              // for an understood value by collect_static_finals??
              always_assert_log(def_insn->opcode() == OPCODE_NEW_INSTANCE ||
                                    def_insn->opcode() == OPCODE_SGET_OBJECT,
                                "Unsupported instantiation %s",
                                SHOW(def_insn));
              if (def_insn->opcode() == OPCODE_NEW_INSTANCE) {
                auto constant = known_fields.at(field_def);
                emit_new_instance_node(constant, def_insn, field_def, insn);
              } else {
                emit_sget_node(def_insn, field_def, insn);
              }
            }
          }
        }
      }
    }
  }
};

void analyze_clinit(const std::unordered_map<DexType*, wp::Spec>& wrapper_types,
                    wp::PassState* pass_state,
                    DexClass* cls,
                    DexMethod* clinit) {
  // Check if this method could be relevant before analyzing.
  if (!has_static_final_wrapper_fields(wrapper_types, cls)) {
    return;
  }

  using CombinedClinitAnalyzer = InstructionAnalyzerCombiner<
      cp::ClinitFieldAnalyzer, cp::WholeProgramAwareAnalyzer,
      cp::ImmutableAttributeAnalyzer, cp::StaticFinalFieldAnalyzer,
      cp::PrimitiveAnalyzer>;

  cp::WholeProgramStateAccessor wps_accessor(pass_state->whole_program_state);

  ClinitMethodAnalysis method_analysis(wrapper_types, pass_state, cls, clinit);
  method_analysis.run(CombinedClinitAnalyzer(clinit->get_class(),
                                             &wps_accessor,
                                             &pass_state->attr_analyzer_state,
                                             nullptr,
                                             nullptr));
}

class FurtherMethodAnalysis : public wp::MethodAnalysis {
 public:
  FurtherMethodAnalysis(
      const std::unordered_map<DexType*, wp::Spec>& wrapper_types,
      wp::PassState* pass_state,
      DexClass* cls,
      DexMethod* method)
      : wp::MethodAnalysis(wrapper_types, pass_state, cls, method) {}

  void post_analyze() override {
    // Continue building the representation of uses of all instances and fields,
    // and their immediate uses.
    auto& cfg = get_cfg();
    for (auto* block : cfg.blocks()) {
      for (auto& mie : InstructionIterable(block)) {
        auto insn = mie.insn;
        if (insn->opcode() == OPCODE_SGET_OBJECT) {
          auto field_def =
              resolve_field(insn->get_field(), FieldSearch::Static);
          if (field_def != nullptr &&
              m_pass_state->sfield_to_node.count(field_def) > 0) {
            std::lock_guard<std::mutex> lock(m_pass_state->modifications_mtx);
            wp::Usage usage{insn, m_method};
            auto sget_node = std::make_unique<wp::Node>();
            sget_node->item = usage;
            // Find all users of the sget, add edges
            attach_usage_nodes(sget_node);
            // Then, connect the sget to pre-existing tree.
            auto existing_node_ptr = m_pass_state->sfield_to_node.at(field_def);
            existing_node_ptr->add_edge(std::move(sget_node));
          }
        }
      }
    }
  }
};

void analyze_method(const std::unordered_map<DexType*, wp::Spec>& wrapper_types,
                    wp::PassState* pass_state,
                    DexClass* cls,
                    DexMethod* m) {

  using CombinedAnalyzer = InstructionAnalyzerCombiner<
      cp::WholeProgramAwareAnalyzer, cp::ImmutableAttributeAnalyzer,
      cp::StaticFinalFieldAnalyzer, cp::PrimitiveAnalyzer>;

  cp::WholeProgramStateAccessor wps_accessor(pass_state->whole_program_state);
  FurtherMethodAnalysis method_analysis(wrapper_types, pass_state, cls, m);
  method_analysis.run(CombinedAnalyzer(
      &wps_accessor, &pass_state->attr_analyzer_state, nullptr, nullptr));
}

void transform_usage(const wp::Source& source,
                     const std::unique_ptr<wp::Node>& ptr,
                     const wp::Spec& spec,
                     PassManager& mgr) {
  auto usage = std::get<wp::Usage>(ptr->item);
  auto& cfg = usage.method->get_code()->cfg();
  auto usage_it = cfg.find_insn(usage.insn);
  cfg::CFGMutation mutation(cfg);

  auto get_insn_field = [&]() {
    auto def = resolve_field(usage.insn->get_field(), FieldSearch::Static);
    always_assert(def != nullptr && is_final(def) && is_static(def));
    return def;
  };

  auto op = usage.insn->opcode();
  if (op == OPCODE_SPUT_OBJECT) {
    // Swap the field of wrapper type to the type of primitive in the original
    // class.
    auto def = get_insn_field();
    DexFieldSpec primitive_spec(def->get_class(), def->get_name(),
                                spec.primitive);
    def->change(primitive_spec);
    auto encoded_value = DexEncodedValue::zero_for_type(spec.primitive);
    encoded_value->value(source.primitive_value);
    def->set_value(std::move(encoded_value));
    TRACE(WP, 1, "Edited field spec: %s", SHOW(def));
    mgr.incr_metric("fields_changed", 1);
    // Remove the sput-object; the encoded value will take its place.
    mutation.remove(usage_it);
  } else if (op == OPCODE_SGET_OBJECT) {
    auto sget = new IRInstruction(sget_op_for_primitive(spec.primitive));
    auto def = get_insn_field();
    auto new_ref =
        DexField::get_field(def->get_class(), def->get_name(), spec.primitive);
    sget->set_field(new_ref);
    // Update the following instruction too if it exists.
    auto move_pseudo_it = cfg.move_result_of(usage_it);
    if (move_pseudo_it.is_end()) {
      mutation.replace(usage_it, {sget});
    } else {
      auto move_pseudo = new IRInstruction(
          move_result_pseudo_op_for_primitive(spec.primitive));
      move_pseudo->set_dest(move_pseudo_it->insn->dest());
      mutation.replace(usage_it, {sget, move_pseudo});
    }
    mgr.incr_metric("sgets_changed", 1);
  } else if (op == OPCODE_MOVE_OBJECT) {
    auto move = new IRInstruction(move_op_for_primitive(spec.primitive));
    move->set_src(0, usage.insn->src(0));
    move->set_dest(usage.insn->dest());
    mutation.replace(usage_it, {move});
  } else {
    always_assert_log(opcode::is_an_invoke(op),
                      "Unsupported instruction for patching: %s",
                      SHOW(usage.insn));
    // TODO: as capabilities of this pass expand, this logic may need to swap
    // the opcode here too. For now, the types are asserted to match up front
    // (which is simpler).
    auto ref = usage.insn->get_method();
    auto search = spec.allowed_invokes.find(ref);
    always_assert_log(search != spec.allowed_invokes.end(),
                      "Unconfigured invoke to %s was allowed as a valid usage",
                      SHOW(ref));
    auto unwrapped_ref = search->second;
    usage.insn->set_method(unwrapped_ref);
    mgr.incr_metric("invokes_changed", 1);
  }
  mutation.flush();
  // Continue making edits down the tree.
  for (auto& next : ptr->edges) {
    transform_usage(source, next, spec, mgr);
  }
}

void transform_node(const std::unique_ptr<wp::Node>& source_ptr,
                    const wp::Spec& spec,
                    PassManager& mgr) {
  auto source = std::get<wp::Source>(source_ptr->item);
  for (auto& ptr : source_ptr->edges) {
    transform_usage(source, ptr, spec, mgr);
  }
}

// Checks the rstate of the method associated with node. Validation that allows/
// disallows transforms should respect this.
bool no_optimizations(const wp::Spec& spec,
                      const std::unique_ptr<wp::Node>& ptr) {
  auto method = ptr->get_method();
  if (method->rstate.no_optimizations()) {
    TRACE(WP,
          2,
          "[%s] Unsupported method %s via rstate",
          SHOW(spec.wrapper),
          SHOW(method));
    return true;
  }
  return false;
}

bool validate_usage(const std::unique_ptr<wp::Node>& ptr,
                    const wp::Spec& spec,
                    PassManager& mgr) {
  always_assert(ptr->is_usage());
  if (no_optimizations(spec, ptr)) {
    return false;
  }
  auto usage = std::get<wp::Usage>(ptr->item);
  auto log_unsupported = [&]() {
    TRACE(WP,
          2,
          "[%s] Unsupported usage %s from method %s",
          SHOW(spec.wrapper),
          SHOW(usage.insn),
          SHOW(usage.method));
    mgr.incr_metric("unsupported_usage", 1);
  };
  auto op = usage.insn->opcode();
  if (op == OPCODE_SPUT_OBJECT || op == OPCODE_SGET_OBJECT) {
    auto def = resolve_field(usage.insn->get_field(), FieldSearch::Static);
    if (def == nullptr || def->get_type() != spec.wrapper || !is_final(def) ||
        !def->rstate.can_delete()) {
      log_unsupported();
      return false;
    }
  } else if (opcode::is_an_invoke(op)) {
    // Check for invocations to configured method(s)
    if (spec.allowed_invokes.count(usage.insn->get_method()) == 0) {
      log_unsupported();
      return false;
    }
  } else if (op == OPCODE_MOVE_OBJECT) {
    // Support this automatically; patching this to change to primitve should
    // be fine. No logic here intentionally.
  } else {
    log_unsupported();
    return false;
  }
  for (auto& next : ptr->edges) {
    if (!validate_usage(next, spec, mgr)) {
      return false;
    }
  }
  return true;
}

// Returns true if the given node and all its downstream usages are simple
// enough to be transformed by this pass. Increments metrics for unsupported
// usages.
bool validate_node(const std::unique_ptr<wp::Node>& source_ptr,
                   const wp::Spec& spec,
                   PassManager& mgr) {
  always_assert(source_ptr->is_source());
  if (no_optimizations(spec, source_ptr)) {
    return false;
  }
  for (auto& ptr : source_ptr->edges) {
    if (!validate_usage(ptr, spec, mgr)) {
      return false;
    }
  }
  return true;
}

void print_edge(const size_t indent, const std::unique_ptr<wp::Node>& ptr) {
  always_assert(ptr->is_usage());
  std::string indent_str(indent, ' ');
  auto& usage = std::get<wp::Usage>(ptr->item);
  TRACE(WP,
        1,
        "%s-> USAGE@%p { %s (%s) }",
        indent_str.c_str(),
        ptr.get(),
        SHOW(usage.method),
        SHOW(usage.insn));
  for (auto& next : ptr->edges) {
    print_edge(indent + 2, next);
  }
}

void print_node(std::unique_ptr<wp::Node>& node, bool edges = true) {
  always_assert(node->is_source());
  auto& source = std::get<wp::Source>(node->item);
  TRACE(WP,
        1,
        "NODE@%p { %s (%s %s) value = %" PRId64 " }",
        node.get(),
        SHOW(source.method),
        SHOW(source.new_instance),
        SHOW(source.init),
        source.primitive_value);
  if (edges) {
    for (auto& ptr : node->edges) {
      print_edge(2, ptr);
    }
  }
}
} // namespace

void WrappedPrimitivesPass::eval_pass(DexStoresVector& stores,
                                      ConfigFiles& conf,
                                      PassManager&) {
  for (auto& spec : m_wrapper_specs) {
    for (auto&& [from, to] : spec.allowed_invokes) {
      auto def = to->as_def();
      if (def != nullptr && def->rstate.can_delete()) {
        TRACE(WP, 2, "Setting %s as root", SHOW(def));
        def->rstate.set_root();
        m_marked_root_methods.emplace(def);
        auto cls = type_class(def->get_class());
        if (cls->rstate.can_delete()) {
          TRACE(WP, 2, "Setting %s as root", SHOW(cls));
          cls->rstate.set_root();
          m_marked_root_classes.emplace(cls);
        }
      }
    }
    for (auto& method : spec.wrapper_type_constructors()) {
      if (!method->rstate.dont_inline()) {
        method->rstate.set_dont_inline();
        TRACE(WP, 2, "Disallowing inlining for %s", SHOW(method));
      }
    }
  }
}

// Undoes the changes made by eval_pass
void WrappedPrimitivesPass::unset_roots() {
  for (auto& def : m_marked_root_methods) {
    TRACE(WP, 2, "Unsetting %s as root", SHOW(def));
    def->rstate.unset_root();
  }
  for (auto& cls : m_marked_root_classes) {
    TRACE(WP, 2, "Unsetting %s as root", SHOW(cls));
    cls->rstate.unset_root();
  }
}

void WrappedPrimitivesPass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& /* unused */,
                                     PassManager& mgr) {
  std::unordered_map<DexType*, wp::Spec> wrapper_types;
  wp::PassState pass_state;
  for (auto& spec : m_wrapper_specs) {
    TRACE(WP,
          1,
          "Will check for wrapper type %s with supported methods:",
          SHOW(spec.wrapper));
    for (auto&& [from, to] : spec.allowed_invokes) {
      TRACE(WP, 1, "  %s", SHOW(from));
    }
    auto wrapper_cls = type_class(spec.wrapper);
    always_assert(wrapper_cls != nullptr);
    wrapper_types.emplace(spec.wrapper, spec);
    cp::immutable_state::analyze_constructors({wrapper_cls},
                                              &pass_state.attr_analyzer_state);
  }

  // First phase: analyze clinit methods to find static final field values.
  // Begin assembling a tree of construction of the wrapper types, their
  // immediate usages, and their writes and reads to static final fields.
  auto scope = build_class_scope(stores);
  size_t possible_cycles{0};
  auto sorted_scope =
      init_deps::reverse_tsort_by_clinit_deps(scope, possible_cycles);
  for (auto cls : sorted_scope) {
    if (cls->is_external()) {
      continue;
    }
    auto clinit = cls->get_clinit();
    if (clinit != nullptr && clinit->get_code() != nullptr) {
      analyze_clinit(wrapper_types, &pass_state, cls, clinit);
    }
  }

  // Continue analyzing the scope, find all uses of static final fields from the
  // initial phase. Continue building the tree of usages.
  InsertOnlyConcurrentSet<DexMethod*> further_analysis_set;
  walk::parallel::opcodes(scope, [&](DexMethod* m, IRInstruction* insn) {
    if (insn->opcode() == OPCODE_SGET_OBJECT) {
      auto ref = insn->get_field();
      auto def = resolve_field(ref, FieldSearch::Static);
      if (def != nullptr && is_final(def) && is_static(def) &&
          wrapper_types.count(def->get_type()) > 0) {
        further_analysis_set.insert(m);
      }
    }
  });
  workqueue_run<DexMethod*>(
      [&](DexMethod* m) {
        analyze_method(wrapper_types, &pass_state, type_class(m->get_class()),
                       m);
      },
      further_analysis_set,
      traceEnabled(WP, 9) ? 1 : redex_parallel::default_num_threads());

  TRACE(WP, 1, "\nDumping nodes:");
  for (auto& node : pass_state.forest.nodes) {
    print_node(node);
    TRACE(WP, 1, "");
  }
  TRACE(WP, 1, "*************************************************************");

  // For each understood creation of a wrapper type, check if all usages fit
  // into a very narrow definition of supported uses that could easily be
  // swapped out for its wrapped primitive type.
  for (auto& ptr : pass_state.forest.nodes) {
    auto source = std::get<wp::Source>(ptr->item);
    auto spec = wrapper_types.at(source.new_instance->get_type());
    if (validate_node(ptr, spec, mgr)) {
      TRACE(WP, 1, "SUPPORTED:");
      print_node(ptr);
      transform_node(ptr, spec, mgr);
    } else {
      TRACE(WP, 1, "Not supported:");
      print_node(ptr, false);
    }
    TRACE(WP, 1, "");
  }

  // Lastly, undo any reachability modifications that were applied during
  // eval_pass.
  unset_roots();
}

static WrappedPrimitivesPass s_pass;
