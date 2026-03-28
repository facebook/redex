/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypedefAnnoCheckerPass.h"

#include <sstream>

#include "AnnoUtils.h"
#include "ClassUtil.h"
#include "KotlinNullCheckMethods.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
#include "TypeUtil.h"
#include "TypedefAnnoPatcher.h"
#include "Walkers.h"

constexpr const char* DEFAULT_SUFFIX = "$default";

namespace {

DexMethod* resolve_method(DexMethod* caller, IRInstruction* insn) {
  auto* def_method = resolve_method_deprecated(insn->get_method(),
                                               opcode_to_search(insn), caller);
  if (def_method == nullptr && insn->opcode() == OPCODE_INVOKE_VIRTUAL) {
    def_method = resolve_method_deprecated(insn->get_method(),
                                           MethodSearch::InterfaceVirtual);
  }
  return def_method;
}

// The ModelGen class with the ModelDefinition annotation is the original
// _Spec class that all the other classes ($Builder, $Serializer, $Deserializer,
// etc.) are derived from. When we check a Parcel or Json read, we know that we
// are either in the $Builder or $Deserializer class and need to derive the base
// Spec class from the name
bool is_model_gen(const DexMethod* m) {
  DexType* type = m->get_class();
  auto model_gen_cls_name =
      type->str().substr(0, type->str().size() - 1) + "Spec;";
  DexClass* cls = type_class(DexType::make_type(model_gen_cls_name));
  if (cls == nullptr) {
    // the class could end in $Builder or $Deserializer
    auto model_gen_cls_name_from_builder =
        type->str().substr(0, type->str().find('$')) + "Spec;";
    cls = type_class(DexType::make_type(model_gen_cls_name_from_builder));
  }
  if ((cls == nullptr) || (cls->get_anno_set() == nullptr)) {
    return false;
  }
  DexAnnotation* anno = get_annotation(
      cls, DexType::make_type("Lcom/facebook/annotationprocessors/modelgen/"
                              "iface/ModelDefinition;"));
  return anno != nullptr;
}

struct MaybeParamName {
  std::optional<std::string_view> name;
  MaybeParamName(const DexMethod* method, size_t param_index)
      : name(method::get_param_name(method, param_index)) {}

  friend std::ostream& operator<<(std::ostream& os, const MaybeParamName& obj) {
    if (obj.name) {
      os << "(" << *obj.name << ")";
    }
    return os;
  }
};

class ErrorBuilder {
  std::ostringstream m_out;

 public:
  ErrorBuilder(const DexMethod* m, const std::string& source_loc) {
    m_out << "TypedefAnnoCheckerPass: in method " << show(m) << "\n"
          << source_loc;
  }

  template <typename... Args>
  ErrorBuilder& detail(const Args&... args) {
    m_out << "  ";
    (m_out << ... << args);
    m_out << "\n";
    return *this;
  }

  ErrorBuilder& failed_instruction(const IRInstruction* insn) {
    m_out << "  failed instruction: " << show(insn);
    return *this;
  }

  std::string str() const { return m_out.str(); }
};

} // namespace

bool TypedefAnnoChecker::is_value_of_opt(const DexMethod* m) {
  if (m->get_simple_deobfuscated_name() != "valueOfOpt") {
    return false;
  }

  // the util class
  auto* cls = type_class(m->get_class());
  if (cls == nullptr ||
      !cls->get_deobfuscated_name_or_empty_copy().ends_with("$Util;")) {
    return false;
  }

  if (cls->get_anno_set() == nullptr) {
    return false;
  }

  DexClass* typedef_cls = nullptr;
  DexAnnotation* anno = get_annotation(
      cls, DexType::make_type("Ldalvik/annotation/EnclosingClass;"));
  if (anno != nullptr) {
    const auto& value = anno->anno_elems().begin()->encoded_value;
    if (value->evtype() == DexEncodedValueTypes::DEVT_TYPE) {
      auto* type_value = dynamic_cast<DexEncodedValueType*>(value.get());
      auto type_name = type_value->show_deobfuscated();
      typedef_cls = type_class(DexType::make_type(type_name));
    }
  }

  if ((typedef_cls == nullptr) || (typedef_cls->get_anno_set() == nullptr)) {
    return false;
  }

  if ((get_annotation(typedef_cls, m_config.int_typedef) == nullptr) &&
      (get_annotation(typedef_cls, m_config.str_typedef) == nullptr)) {
    return false;
  }
  return true;
}

bool TypedefAnnoChecker::is_generated(const DexMethod* m) const {
  if (m_config.generated_type_annos.empty()) {
    return false;
  }
  DexType* type = m->get_class();
  DexClass* cls = type_class(type);
  if (cls->get_anno_set() == nullptr) {
    return false;
  }
  if (has_any_annotation(cls, m_config.generated_type_annos)) {
    return true;
  }
  return false;
}

bool TypedefAnnoChecker::should_not_check(const DexMethod* m) const {
  for (const auto& prefix : UnorderedIterable(m_config.do_not_check_list)) {
    if (m->get_deobfuscated_name_or_empty_copy().starts_with(prefix)) {
      return true;
    }
  }

  auto null_check_methods =
      kotlin_nullcheck_wrapper::get_kotlin_null_assertions();
  for (DexMethodRef* kotlin_null_method :
       UnorderedIterable(null_check_methods)) {
    if (kotlin_null_method->get_name() == m->get_name()) {
      return true;
    }
  }

  return false;
}

bool TypedefAnnoChecker::is_delegate(const DexMethod* m) {
  auto* cls = type_class(m->get_class());
  if (cls == nullptr) {
    return false;
  }
  DexTypeList* interfaces = cls->get_interfaces();

  if (interfaces->empty()) {
    return false;
  }

  const auto& cfg = m->get_code()->cfg();
  DexField* delegate = nullptr;

  for (auto* block : cfg.blocks()) {
    for (const auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      if (insn->opcode() == OPCODE_IGET_OBJECT) {
        DexField* field = insn->get_field()->as_def();
        if (field == nullptr) {
          continue;
        }
        // find methods that delegate with $$delegate_ P1697234372
        if (field->get_simple_deobfuscated_name().starts_with("$$delegate_")) {
          delegate = field;
        } else {
          // find methods that delegate without $$delegate_ P1698648093
          // the field type must match one of the interfaces
          for (const auto* interface : *interfaces) {
            if (interface->get_name() == field->get_type()->get_name()) {
              delegate = field;
            }
          }
        }
      } else if (opcode::is_an_invoke(insn->opcode()) &&
                 (delegate != nullptr)) {
        auto* callee = insn->get_method()->as_def();
        if (callee == nullptr) {
          continue;
        }
        if (m->get_simple_deobfuscated_name() ==
                callee->get_simple_deobfuscated_name() &&
            m->get_proto() == callee->get_proto()) {
          TRACE(TAC, 2, "skipping delegate method %s", SHOW(m));
          return true;
        }
      }
    }
  }
  return false;
}

void TypedefAnnoChecker::run(DexMethod* m) {
  IRCode* code = m->get_code();
  if (code == nullptr) {
    return;
  }

  if (is_value_of_opt(m) || is_delegate(m) || is_generated(m)) {
    return;
  }

  always_assert(code->cfg_built());
  auto& cfg = code->cfg();
  redex_assert(m_config.int_typedef != nullptr);
  redex_assert(m_config.str_typedef != nullptr);
  m_method = m;
  type_inference::TypeInference inference(
      cfg, false,
      UnorderedSet<const DexType*>{m_config.int_typedef, m_config.str_typedef},
      &m_method_override_graph);
  inference.run(m);

  live_range::MoveAwareChains chains(cfg);
  live_range::UseDefChains ud_chains = chains.get_use_def_chains();
  live_range::DefUseChains du_chains = chains.get_def_use_chains();
  m_inference = &inference;
  m_ud_chains = &ud_chains;
  m_du_chains = &du_chains;

  m_return_annotation = boost::none;
  DexAnnotationSet* return_annos = m->get_anno_set();
  if (return_annos != nullptr) {
    m_return_annotation = type_inference::get_typedef_annotation(
        return_annos->get_annotations(), inference.get_annotations());
  }
  m_envs = &inference.get_type_environments();
  TRACE(TAC, 5, "Start checking %s", SHOW(m));
  TRACE(TAC, 5, "%s", SHOW(cfg));
  for (cfg::Block* b : cfg.blocks()) {
    const DexPosition* last_pos = nullptr;
    for (auto& mie : *b) {
      if (mie.type == MFLOW_POSITION) {
        last_pos = mie.pos.get();
      } else if (mie.type == MFLOW_OPCODE && last_pos != nullptr) {
        m_insn_positions.emplace(mie.insn, last_pos);
      }
    }
  }
  for (cfg::Block* b : cfg.blocks()) {
    for (auto& mie : InstructionIterable(b)) {
      check_instruction(mie.insn);
    }
  }
  if (!m_good) {
    TRACE(TAC, 2, "Done checking %s", SHOW(m));
  }
  // Clean up the param names from dex debug items
  if (code->get_debug_item() != nullptr) {
    code->get_debug_item()->remove_param_names();
  }
}

std::string TypedefAnnoChecker::format_source_loc(
    const IRInstruction* insn) const {
  auto it = m_insn_positions.find(insn);
  if (it == m_insn_positions.end()) {
    return "";
  }
  const auto* pos = it->second;
  std::ostringstream oss;
  oss << "  at ";
  if (pos->file != nullptr) {
    oss << pos->file->str();
  } else {
    oss << "Unknown source";
  }
  oss << ":" << pos->line << "\n";
  return oss.str();
}

void TypedefAnnoChecker::add_error(const std::string& error) {
  if (!m_error.empty()) {
    m_error.append("\n\n");
  }
  m_error.append(error);
  m_good = false;
}

void TypedefAnnoChecker::check_instruction(IRInstruction* insn) {
  // if the invoked method's arguments have annotations with the
  // @SafeStringDef or @SafeIntDef annotation, check that TypeInference
  // inferred the correct annotation for the values being passed in
  const auto& env = m_envs->find(insn)->second;
  IROpcode opcode = insn->opcode();
  switch (opcode) {
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_INTERFACE: {
    auto* callee_def = resolve_method(m_method, insn);
    if (callee_def == nullptr) {
      return;
    }
    UnorderedBag<const DexMethod*> callees;
    if (mog::is_true_virtual(m_method_override_graph, callee_def) &&
        (callee_def->get_code() == nullptr)) {
      callees =
          mog::get_overriding_methods(m_method_override_graph, callee_def);
    }
    callees.insert(callee_def);
    for (const DexMethod* callee : UnorderedIterable(callees)) {
      if (callee->get_param_anno() == nullptr) {
        // Callee does not expect any Typedef value. Nothing to do.
        return;
      }
      for (auto const& param_anno : *callee->get_param_anno()) {
        auto annotation = type_inference::get_typedef_annotation(
            param_anno.second->get_annotations(),
            m_inference->get_annotations());
        if (annotation == boost::none) {
          continue;
        }
        int param_index = insn->opcode() == OPCODE_INVOKE_STATIC
                              ? param_anno.first
                              : param_anno.first + 1;
        reg_t reg = insn->src(param_index);
        auto anno_type = env.get_annotation(reg);
        auto type = env.get_dex_type(reg);

        // TypeInference inferred a different annotation
        if (anno_type && anno_type != annotation) {
          ErrorBuilder err(m_method, format_source_loc(insn));
          if (anno_type.value() == type::java_lang_Object()) {
            err.detail("while invoking ", show(callee), ", parameter ",
                       param_anno.first,
                       MaybeParamName(callee, param_anno.first),
                       " should have the annotation ",
                       annotation.value()->get_name()->c_str())
                .detail(
                    "but it instead contains an ambiguous annotation, "
                    "implying that the parameter was joined with another "
                    "typedef annotation before the method invocation. The "
                    "ambiguous annotation is unsafe, and typedef "
                    "annotations should not be mixed.")
                .failed_instruction(insn);
          } else {
            err.detail("while invoking ", show(callee), ", parameter ",
                       param_anno.first,
                       MaybeParamName(callee, param_anno.first),
                       " has the annotation ", show(anno_type))
                .detail("but the method expects the annotation to be ",
                        annotation.value()->get_name()->c_str(), ".")
                .failed_instruction(insn);
          }
          add_error(err.str());
        } else if (typedef_anno::is_not_str_nor_int(env, reg)) {
          auto* cls = type_class(callee->get_class());
          if (method::is_constructor(callee) && is_enum(cls) &&
              type::is_kotlin_class(cls)) {
            // Kotlin enums ctors param annotations are off by two because of
            // the artificially injected two params at the beginning.
            continue;
          } else {
            add_error(
                ErrorBuilder(m_method, format_source_loc(insn))
                    .detail("annotation ", show(annotation),
                            " annotates a parameter with an incompatible type ",
                            show(type),
                            " or a non-constant parameter while invoking ",
                            show(callee), ".")
                    .failed_instruction(insn)
                    .str());
          }
        } else if (!anno_type) {
          // TypeInference didn't infer anything
          if (auto err = check_typedef_value(annotation, insn, param_index)) {
            std::ostringstream ctx;
            ctx << "\n  Calling: " << show(callee) << "\n";
            ctx << "  Incorrect parameter: index " << param_anno.first
                << MaybeParamName(callee, param_anno.first);
            add_error(*err + ctx.str());
            TRACE(TAC, 1, "invoke method: %s", SHOW(callee));
          }
        }
      }
    }
    break;
  }
  // when writing to annotated fields, check that the value is annotated
  case OPCODE_IPUT:
  case OPCODE_SPUT:
  case OPCODE_SPUT_OBJECT:
  case OPCODE_IPUT_OBJECT: {
    auto env_anno = env.get_annotation(insn->src(0));
    auto field_anno = type_inference::get_typedef_anno_from_member(
        insn->get_field(), m_inference->get_annotations());
    if (env_anno != boost::none && field_anno != boost::none &&
        env_anno.value() != field_anno.value()) {
      add_error(ErrorBuilder(m_method, format_source_loc(insn))
                    .detail("assigned field ", insn->get_field()->c_str(),
                            " with annotation ", show(field_anno),
                            " to a value with annotation ", show(env_anno), ".")
                    .failed_instruction(insn)
                    .str());
    } else if (env_anno == boost::none && field_anno != boost::none) {
      if (auto err = check_typedef_value(field_anno, insn, 0)) {
        std::ostringstream ctx;
        ctx << "\n Error writing to field " << show(insn->get_field())
            << "in method" << SHOW(m_method);
        add_error(*err + ctx.str());
        TRACE(TAC, 1, "writing to field: %s", SHOW(insn->get_field()));
      }
    }
    break;
  }
  // if there's an annotation that has a string typedef or an int typedef
  // annotation in the method's signature, check that TypeInference
  // inferred that annotation in the returned value
  case OPCODE_RETURN:
  case OPCODE_RETURN_OBJECT: {
    if (m_return_annotation) {
      reg_t reg = insn->src(0);
      auto anno_type = env.get_annotation(reg);
      if (anno_type && anno_type != m_return_annotation) {
        ErrorBuilder err(m_method, format_source_loc(insn));
        if (anno_type.value() == type::java_lang_Object()) {
          err.detail("has return annotation ",
                     m_return_annotation.value()->get_name()->c_str(),
                     " but the returned value has an ambiguous annotation, "
                     "implying that the value was joined with another typedef "
                     "annotation within the method. The ambiguous annotation "
                     "is unsafe, and typedef annotations should not be mixed.")
              .failed_instruction(insn);
        } else {
          err.detail("has return annotation ",
                     m_return_annotation.value()->get_name()->c_str(),
                     " but the returned value has annotation ", show(anno_type),
                     " instead.")
              .failed_instruction(insn);
        }
        add_error(err.str());
      } else if (typedef_anno::is_not_str_nor_int(env, reg)) {
        add_error(
            ErrorBuilder(m_method, format_source_loc(insn))
                .detail("annotation ", show(m_return_annotation),
                        " annotates a value with an incompatible type or a "
                        "non-constant value.")
                .failed_instruction(insn)
                .str());
      } else if (!anno_type) {
        if (auto err = check_typedef_value(m_return_annotation, insn, 0)) {
          add_error(*err + "\n Error caught when returning the faulty value");
        }
      }
    }
    break;
  }
  default:
    break;
  }
}

std::optional<std::string> TypedefAnnoChecker::check_typedef_value(
    const boost::optional<const DexType*>& annotation,
    IRInstruction* insn,
    const src_index_t src) {

  auto* anno_class = type_class(annotation.value());
  const auto* str_value_set = m_strdef_constants.get_unsafe(anno_class);
  const auto* int_value_set = m_intdef_constants.get_unsafe(anno_class);

  bool has_str_vals = str_value_set != nullptr && !str_value_set->empty();
  bool has_int_vals = int_value_set != nullptr && !int_value_set->empty();
  always_assert_log(has_int_vals ^ has_str_vals,
                    "%s has both str and int const values", SHOW(anno_class));
  if (!has_str_vals && !has_int_vals) {
    TRACE(TAC, 1, "%s contains no annotation constants", SHOW(anno_class));
    return std::nullopt;
  }

  auto* cls = type_class(m_method->get_class());
  if (m_config.skip_anonymous_classes && klass::maybe_anonymous_class(cls)) {
    return std::nullopt;
  }

  live_range::Use use_of_id{insn, src};
  auto udchains_it = m_ud_chains->find(use_of_id);
  auto defs_set = udchains_it->second;

  for (IRInstruction* def : defs_set) {
    switch (def->opcode()) {
    case OPCODE_CONST_STRING: {
      const auto* const const_value = def->get_string();
      if (const_value->str().empty() && is_generated(m_method)) {
        break;
      }
      if (str_value_set->count(const_value) == 0) {
        return ErrorBuilder(m_method, format_source_loc(insn))
            .detail("the string value ", show(const_value),
                    " does not have the typedef annotation ", show(annotation),
                    " attached to it.")
            .detail(
                "Check that the value is annotated and exists in the "
                "typedef annotation class.")
            .failed_instruction(def)
            .str();
      }
      break;
    }
    case OPCODE_CONST: {
      auto const const_value = def->get_literal();
      if (has_str_vals && const_value == 0) {
        // Null assigned to a StringDef value. This is valid. We don't enforce
        // nullness.
        break;
      }
      if ((int_value_set != nullptr) &&
          int_value_set->count(const_value) == 0) {
        // when passing an integer to a default method, the value will be 0 if
        // the default method will the default value. The const 0 is not
        // annotated and might not be in the IntDef. Since the checker will
        // check that the default value is a member of the IntDef, passing in 0
        // is safe. Example caller and default methods: P1222824190 P1222829651
        if (const_value == 0 && opcode::is_an_invoke(insn->opcode())) {
          DexMethodRef* callee = insn->get_method();
          if (callee->is_def() &&
              callee->as_def()->get_simple_deobfuscated_name().ends_with(
                  DEFAULT_SUFFIX)) {
            break;
          }
        }
        return ErrorBuilder(m_method, format_source_loc(insn))
            .detail("the int value ", show(const_value),
                    " does not have the typedef annotation ", show(annotation),
                    " attached to it.")
            .detail(
                "Check that the value is annotated and exists in the "
                "typedef annotation class.")
            .failed_instruction(def)
            .str();
      }
      break;
    }
    case IOPCODE_LOAD_PARAM_OBJECT:
    case IOPCODE_LOAD_PARAM: {
      // this is for cases similar to testIfElseParam in the integ tests
      // where the boolean parameter undergoes an OPCODE_MOVE and
      // gets returned instead of one of the two ints
      auto env = m_envs->find(def);
      if (env->second.get_int_type(def->dest()).element() ==
          (IntType::BOOLEAN)) {
        if (int_value_set->count(0) == 0 || int_value_set->count(1) == 0) {
          return ErrorBuilder(m_method, format_source_loc(insn))
              .detail("assigns a int with typedef annotation ",
                      show(annotation),
                      " to either 0 or 1, which is invalid because the "
                      "typedef annotation class does not contain both the "
                      "values 0 and 1.")
              .failed_instruction(def)
              .str();
        }
        break;
      }
      auto anno = env->second.get_annotation(def->dest());
      if (anno == boost::none || anno != annotation) {
        return ErrorBuilder(m_method, format_source_loc(insn))
            .detail(
                "one of the parameters needs to have the typedef "
                "annotation ",
                show(annotation),
                " attached to it. Check that the value is annotated and "
                "exists in the typedef annotation class.")
            .failed_instruction(def)
            .str();
      }
      break;
    }
    case OPCODE_INVOKE_VIRTUAL:
    case OPCODE_INVOKE_SUPER:
    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_STATIC:
    case OPCODE_INVOKE_INTERFACE: {
      auto* def_method = resolve_method(m_method, def);
      if (def_method == nullptr) {
        return ErrorBuilder(m_method, format_source_loc(insn))
            .detail("the source of the value with annotation ",
                    show(annotation),
                    " is produced by invoking an unresolvable callee, so the "
                    "value safety is not guaranteed.")
            .failed_instruction(def)
            .str();
      }
      if (is_model_gen(m_method) || should_not_check(def_method)) {
        break;
      }
      UnorderedBag<const DexMethod*> callees;
      if (mog::is_true_virtual(m_method_override_graph, def_method) &&
          (def_method->get_code() == nullptr)) {
        callees =
            mog::get_overriding_methods(m_method_override_graph, def_method);
      }
      callees.insert(def_method);
      for (const DexMethod* callee : UnorderedIterable(callees)) {
        boost::optional<const DexType*> anno =
            type_inference::get_typedef_anno_from_member(
                callee, m_inference->get_annotations());
        if (anno == boost::none || anno != annotation) {
          DexType* return_type = callee->get_proto()->get_rtype();
          // constant folding might cause the source to be the invoked boolean
          // method https://fburl.com/code/h3dn0ft0
          if (type::is_boolean(return_type) && int_value_set->count(0) == 1 &&
              int_value_set->count(1) == 1) {
            break;
          }
          return ErrorBuilder(m_method, format_source_loc(insn))
              .detail("the return value of ", show(def->get_method()->as_def()))
              .detail("is used where annotation ", show(annotation),
                      " is required,")
              .detail(
                  "but that method does not have the annotation in its "
                  "return type.")
              .detail("To fix: use a constant from ", show(annotation),
                      ", or validate the value before passing it.")
              .failed_instruction(def)
              .str();
        }
      }
      break;
    }
    case OPCODE_XOR_INT:
    case OPCODE_XOR_INT_LIT: {
      // https://fburl.com/code/7lk98pj6
      // in the code linked above, NotifLogAppBadgeEnabled.ENABLED has a value
      // of 0, and NotifLogAppBadgeEnabled.DISABLED_FROM_OS_ONLY has a value
      // of 1. We essentially end up with
      // mNotificationsSharedPrefsHelper.get().getAppBadgeEnabledStatus() ? 0 :
      // 1 which gets optimized to an XOR by the compiler
      if (int_value_set->count(0) == 0 || int_value_set->count(1) == 0) {
        return ErrorBuilder(m_method, format_source_loc(insn))
            .detail("assigns a int with typedef annotation ",
                    show(annotation),
                    " to either 0 or 1, which is invalid because the typedef "
                    "annotation class does not contain both the values 0 "
                    "and 1.")
            .failed_instruction(def)
            .str();
      }
      break;
    }
    case OPCODE_IGET:
    case OPCODE_SGET:
    case OPCODE_IGET_OBJECT:
    case OPCODE_SGET_OBJECT: {
      auto field_anno = type_inference::get_typedef_anno_from_member(
          def->get_field(), m_inference->get_annotations());
      if (!field_anno || field_anno != annotation) {
        add_error(ErrorBuilder(m_method, format_source_loc(insn))
                      .detail("the field ", def->get_field()->str(),
                              " needs to have the annotation ",
                              show(annotation), ".")
                      .failed_instruction(def)
                      .str());
      }
      break;
    }
    case OPCODE_NEW_INSTANCE: {
      auto duchains_it = m_du_chains->find(def);
      const auto& uses_set = duchains_it->second;
      for (live_range::Use use : UnorderedIterable(uses_set)) {
        IRInstruction* use_insn = use.insn;
        if (opcode::is_an_iput(use_insn->opcode()) ||
            opcode::is_an_sput(use_insn->opcode())) {
          if (auto err = check_typedef_value(annotation, use_insn, 0)) {
            add_error(*err);
          }
        }
      }
      break;
    }
    case OPCODE_CHECK_CAST: {
      if (auto err = check_typedef_value(annotation, def, 0)) {
        add_error(*err);
      }
      break;
    }
    case OPCODE_MOVE_EXCEPTION: {
      break;
    }
    default: {
      return ErrorBuilder(m_method, format_source_loc(insn))
          .detail(show(m_method->get_code()->cfg(), true))
          .detail(
              "does not guarantee value safety for the value with "
              "typedef annotation ",
              show(annotation),
              ". Check that this value does not change within the "
              "method.")
          .failed_instruction(def)
          .str();
    }
    }
  }
  return std::nullopt;
}

void TypedefAnnoCheckerPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& /* unused */,
                                      PassManager& mgr) {
  assert(m_config.int_typedef != nullptr);
  assert(m_config.str_typedef != nullptr);
  auto scope = build_class_scope(stores);
  auto method_override_graph = mog::build_graph(scope);
  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
  TypedefAnnoPatcher patcher(m_config, *method_override_graph);
  walk::parallel::classes(scope, [&](DexClass* cls) {
    gather_typedef_values(cls, strdef_constants, intdef_constants);
  });

  patcher.run(scope);
  patcher.print_stats(mgr);
  TRACE(TAC, 1, "Finished patcher run");

  auto stats = walk::parallel::methods<CheckerStats>(scope, [&](DexMethod* m) {
    TypedefAnnoChecker checker = TypedefAnnoChecker(
        strdef_constants, intdef_constants, m_config, *method_override_graph);
    checker.run(m);
    if (!checker.complete()) {
      return CheckerStats(checker.error());
    }
    return CheckerStats();
  });

  always_assert_type_log(
      stats.m_count == 0,
      RedexError::TYPEDEF_ANNO_CHECKER_ERROR,
      R"(############ Typedef Annotation Value Safety Violation ############
######### Values passed to @SafeStringDef/@SafeIntDef      #######
######### parameters must come from the annotation's        #######
######### defined constants. See errors below for details.  #######
###################################################################
Encountered %zu faulty methods. The errors are
%s
)",
      stats.m_count,
      stats.m_errors.c_str());
}

void TypedefAnnoCheckerPass::gather_typedef_values(
    const DexClass* cls,
    StrDefConstants& strdef_constants,
    IntDefConstants& intdef_constants) {
  const std::vector<DexField*>& fields = cls->get_sfields();
  if (get_annotation(cls, m_config.str_typedef) != nullptr) {
    UnorderedSet<const DexString*> str_values;
    for (auto* field : fields) {
      str_values.emplace(
          dynamic_cast<DexEncodedValueString*>(field->get_static_value())
              ->string());
    }
    strdef_constants.emplace(cls, std::move(str_values));
  } else if (get_annotation(cls, m_config.int_typedef) != nullptr) {
    UnorderedSet<uint64_t> int_values;
    for (auto* field : fields) {
      int_values.emplace(field->get_static_value()->value());
    }
    intdef_constants.emplace(cls, std::move(int_values));
  }
}

static TypedefAnnoCheckerPass s_pass;
