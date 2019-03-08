/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReflectionAnalysis.h"

#include <iomanip>
#include <unordered_map>

#include <boost/optional.hpp>

#include "BaseIRAnalyzer.h"
#include "ControlFlow.h"
#include "DexUtil.h"
#include "FiniteAbstractDomain.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "ReducedProductAbstractDomain.h"
#include "Show.h"

using namespace sparta;

std::ostream& operator<<(std::ostream& out,
                         const reflection::AbstractObject& x) {
  switch (x.obj_kind) {
  case reflection::OBJECT: {
    out << "OBJECT{" << SHOW(x.dex_type) << "}";
    break;
  }
  case reflection::STRING: {
    if (x.dex_string != nullptr) {
      const std::string& str = x.dex_string->str();
      if (str.empty()) {
        out << "\"\"";
      } else {
        out << std::quoted(str);
      }
    }
    break;
  }
  case reflection::CLASS: {
    out << "CLASS{" << SHOW(x.dex_type) << "}";
    break;
  }
  case reflection::FIELD: {
    out << "FIELD{" << SHOW(x.dex_type) << ":" << SHOW(x.dex_string) << "}";
    break;
  }
  case reflection::METHOD: {
    out << "METHOD{" << SHOW(x.dex_type) << ":" << SHOW(x.dex_string) << "}";
    break;
  }
  }
  return out;
}

std::ostream& operator<<(std::ostream& out,
                         const reflection::ClassObjectSource& cls_src) {
  switch (cls_src) {
  case reflection::NON_REFLECTION: {
    out << "NON_REFLECTION";
    break;
  }
  case reflection::REFLECTION: {
    out << "REFLECTION";
    break;
  }
  }
  return out;
}

std::ostream& operator<<(std::ostream& out,
                         const reflection::ReflectionAbstractObject& aobj) {
  out << aobj.first;
  if (aobj.first.obj_kind == reflection::CLASS && aobj.second) {
    out << "(" << *aobj.second << ")";
  }
  return out;
}

namespace reflection {

bool is_not_reflection_output(const AbstractObject& obj) {
  return obj.obj_kind == OBJECT || obj.obj_kind == STRING;
}

bool operator==(const AbstractObject& x, const AbstractObject& y) {
  if (x.obj_kind != y.obj_kind) {
    return false;
  }
  switch (x.obj_kind) {
  case OBJECT:
  case CLASS: {
    return x.dex_type == y.dex_type;
  }
  case STRING: {
    return x.dex_string == y.dex_string;
  }
  case FIELD:
  case METHOD: {
    return x.dex_type == y.dex_type && x.dex_string == y.dex_string;
  }
  }
}

bool operator!=(const AbstractObject& x, const AbstractObject& y) {
  return !(x == y);
}

bool AbstractObject::leq(const AbstractObject& other) const {
  // Check if `other` is a general CLASS or OBJECT
  if (obj_kind == other.obj_kind) {
    switch (obj_kind) {
    case AbstractObjectKind::CLASS:
    case AbstractObjectKind::OBJECT:
      if (other.dex_type == nullptr) {
        return true;
      }
      break;
    case AbstractObjectKind::STRING:
      if (other.dex_string == nullptr) {
        return true;
      }
      break;
    case AbstractObjectKind::FIELD:
    case AbstractObjectKind::METHOD:
      if (other.dex_type == nullptr && other.dex_string == nullptr) {
        return true;
      }
      break;
    }
  }
  return equals(other);
}

bool AbstractObject::equals(const AbstractObject& other) const {
  return *this == other;
}

sparta::AbstractValueKind AbstractObject::join_with(
    const AbstractObject& other) {
  if (other.leq(*this)) {
    // We are higher on the lattice
    return sparta::AbstractValueKind::Value;
  }
  if (obj_kind != other.obj_kind) {
    return sparta::AbstractValueKind::Top;
  }

  switch (obj_kind) {
  case AbstractObjectKind::OBJECT:
  case AbstractObjectKind::CLASS:
    // Be conservative and drop the type info
    dex_type = nullptr;
    break;
  case AbstractObjectKind::STRING:
    // Be conservative and drop the string info
    dex_string = nullptr;
    break;
  case AbstractObjectKind::FIELD:
  case AbstractObjectKind::METHOD:
    // Be conservative and drop the field and method info
    dex_type = nullptr;
    dex_string = nullptr;
    break;
  }
  return sparta::AbstractValueKind::Value;
}

sparta::AbstractValueKind AbstractObject::meet_with(
    const AbstractObject& other) {
  if (leq(other)) {
    // We are lower on the lattice
    return sparta::AbstractValueKind::Value;
  }
  if (other.leq(*this)) {
    *this = other;
    return sparta::AbstractValueKind::Value;
  }
  return sparta::AbstractValueKind::Bottom;
}

namespace impl {

using register_t = ir_analyzer::register_t;
using namespace ir_analyzer;

class AbstractObjectDomain final
    : public sparta::AbstractDomainScaffolding<AbstractObject,
                                               AbstractObjectDomain> {
 public:
  AbstractObjectDomain() { this->set_to_top(); }
  explicit AbstractObjectDomain(AbstractObject obj) {
    this->set_to_value(AbstractObject(std::move(obj)));
  }
  explicit AbstractObjectDomain(sparta::AbstractValueKind kind)
      : sparta::AbstractDomainScaffolding<AbstractObject, AbstractObjectDomain>(
            kind) {}

  boost::optional<AbstractObject> get_object() const {
    return (this->kind() == sparta::AbstractValueKind::Value)
               ? boost::optional<AbstractObject>(*this->get_value())
               : boost::none;
  }
};

using ClassObjectSourceDomain =
    sparta::ConstantAbstractDomain<ClassObjectSource>;

using BasicAbstractObjectEnvironment =
    PatriciaTreeMapAbstractEnvironment<register_t, AbstractObjectDomain>;

using ClassObjectSourceEnvironment =
    PatriciaTreeMapAbstractEnvironment<register_t, ClassObjectSourceDomain>;

class AbstractObjectEnvironment final
    : public ReducedProductAbstractDomain<AbstractObjectEnvironment,
                                          BasicAbstractObjectEnvironment,
                                          ClassObjectSourceEnvironment> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  static void reduce_product(
      std::tuple<BasicAbstractObjectEnvironment,
                 ClassObjectSourceEnvironment>& /* product */) {}

  const AbstractObjectDomain get_abstract_obj(register_t reg) const {
    return get<0>().get(reg);
  }

  void set_abstract_obj(register_t reg, const AbstractObjectDomain aobj) {
    apply<0>([=](auto env) { env->set(reg, aobj); }, true);
  }

  void update_abstract_obj(
      register_t reg,
      const std::function<AbstractObjectDomain(const AbstractObjectDomain&)>&
          operation) {
    apply<0>([=](auto env) { env->update(reg, operation); }, true);
  }

  const ClassObjectSourceDomain get_class_source(register_t reg) const {
    return get<1>().get(reg);
  }

  void set_class_source(register_t reg, const ClassObjectSourceDomain cls_src) {
    apply<1>([=](auto env) { env->set(reg, cls_src); }, true);
  }
};

class Analyzer final : public BaseIRAnalyzer<AbstractObjectEnvironment> {
 public:
  explicit Analyzer(const cfg::ControlFlowGraph& cfg)
      : BaseIRAnalyzer(cfg), m_cfg(cfg) {}

  void run(DexMethod* dex_method) {
    // We need to compute the initial environment by assigning the parameter
    // registers their correct abstract object derived from the method's
    // signature. The IOPCODE_LOAD_PARAM_* instructions are pseudo-operations
    // that are used to specify the formal parameters of the method. They must
    // be interpreted separately.
    //
    // Note that we do not try to infer them as STRINGs.
    // Since we don't have the the actual value of the string other than their
    // type being String. Also for CLASSes, the exact Java type they refer to is
    // not available here.
    auto init_state = AbstractObjectEnvironment::top();
    const auto& signature =
        dex_method->get_proto()->get_args()->get_type_list();
    auto sig_it = signature.begin();
    bool first_param = true;
    // By construction, the IOPCODE_LOAD_PARAM_* instructions are located at the
    // beginning of the entry block of the CFG.
    for (const auto& mie : InstructionIterable(m_cfg.entry_block())) {
      IRInstruction* insn = mie.insn;
      switch (insn->opcode()) {
      case IOPCODE_LOAD_PARAM_OBJECT: {
        if (first_param && !is_static(dex_method)) {
          // If the method is not static, the first parameter corresponds to
          // `this`.
          first_param = false;
          update_non_string_input(&init_state, insn, dex_method->get_class());
        } else {
          // This is a regular parameter of the method.
          DexType* type = *sig_it;
          always_assert(sig_it++ != signature.end());
          update_non_string_input(&init_state, insn, type);
        }
        break;
      }
      case IOPCODE_LOAD_PARAM:
      case IOPCODE_LOAD_PARAM_WIDE: {
        default_semantics(insn, &init_state);
        break;
      }
      default: {
        // We've reached the end of the LOAD_PARAM_* instruction block and we
        // simply exit the loop. Note that premature loop exit is probably the
        // only legitimate use of goto in C++ code.
        goto done;
      }
      }
    }
  done:
    MonotonicFixpointIterator::run(init_state);
    populate_environments(m_cfg);
  }

  void analyze_instruction(
      IRInstruction* insn,
      AbstractObjectEnvironment* current_state) const override {
    switch (insn->opcode()) {
    case IOPCODE_LOAD_PARAM:
    case IOPCODE_LOAD_PARAM_OBJECT:
    case IOPCODE_LOAD_PARAM_WIDE: {
      // IOPCODE_LOAD_PARAM_* instructions have been processed before the
      // analysis.
      break;
    }
    case OPCODE_MOVE_OBJECT: {
      const auto aobj = current_state->get_abstract_obj(insn->src(0));
      current_state->set_abstract_obj(insn->dest(), aobj);
      const auto obj = aobj.get_object();
      if (obj && obj->obj_kind == AbstractObjectKind::CLASS) {
        current_state->set_class_source(
            insn->dest(), current_state->get_class_source(insn->src(0)));
      }
      break;
    }
    case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
    case OPCODE_MOVE_RESULT_OBJECT: {
      const auto aobj = current_state->get_abstract_obj(RESULT_REGISTER);
      current_state->set_abstract_obj(insn->dest(), aobj);
      const auto obj = aobj.get_object();
      if (obj && obj->obj_kind == AbstractObjectKind::CLASS) {
        current_state->set_class_source(
            insn->dest(), current_state->get_class_source(RESULT_REGISTER));
      }
      break;
    }
    case OPCODE_CONST_STRING: {
      current_state->set_abstract_obj(
          RESULT_REGISTER,
          AbstractObjectDomain(AbstractObject(insn->get_string())));
      break;
    }
    case OPCODE_CONST_CLASS: {
      auto aobj = AbstractObject(AbstractObjectKind::CLASS, insn->get_type());
      current_state->set_abstract_obj(RESULT_REGISTER,
                                      AbstractObjectDomain(aobj));
      current_state->set_class_source(
          RESULT_REGISTER,
          ClassObjectSourceDomain(ClassObjectSource::REFLECTION));
      break;
    }
    case OPCODE_CHECK_CAST: {
      const auto aobj = current_state->get_abstract_obj(insn->src(0));
      current_state->set_abstract_obj(RESULT_REGISTER, aobj);
      const auto obj = aobj.get_object();
      if (obj && obj->obj_kind == AbstractObjectKind::CLASS) {
        current_state->set_class_source(
            RESULT_REGISTER, current_state->get_class_source(insn->src(0)));
      }
      // Note that this is sound. In a concrete execution, if the check-cast
      // operation fails, an exception is thrown and the control point
      // following the check-cast becomes unreachable, which corresponds to
      // _|_ in the abstract domain. Any abstract state is a sound
      // approximation of _|_.
      break;
    }
    case OPCODE_AGET_OBJECT: {
      const auto array_object =
          current_state->get_abstract_obj(insn->src(0)).get_object();
      if (array_object) {
        auto type = array_object->dex_type;
        if (type && is_array(type)) {
          const auto etype = get_array_component_type(type);
          update_non_string_input(current_state, insn, etype);
          break;
        }
      }
      default_semantics(insn, current_state);
      break;
    }
    case OPCODE_IGET_OBJECT:
    case OPCODE_SGET_OBJECT: {
      always_assert(insn->has_field());
      const auto field = insn->get_field();
      update_non_string_input(current_state, insn, field->get_type());
      break;
    }
    case OPCODE_NEW_INSTANCE:
    case OPCODE_NEW_ARRAY:
    case OPCODE_FILLED_NEW_ARRAY: {
      current_state->set_abstract_obj(
          RESULT_REGISTER,
          AbstractObjectDomain(
              AbstractObject(AbstractObjectKind::OBJECT, insn->get_type())));
      break;
    }
    case OPCODE_INVOKE_VIRTUAL: {
      auto receiver =
          current_state->get_abstract_obj(insn->src(0)).get_object();
      if (!receiver) {
        update_return_object(current_state, insn);
        break;
      }
      process_virtual_call(insn, *receiver, current_state);
      break;
    }
    case OPCODE_INVOKE_STATIC: {
      if (insn->get_method() == m_for_name) {
        auto class_name =
            current_state->get_abstract_obj(insn->src(0)).get_object();
        if (class_name && class_name->obj_kind == STRING) {
          if (class_name->dex_string != nullptr) {
            auto internal_name =
                DexString::make_string(JavaNameUtil::external_to_internal(
                    class_name->dex_string->str()));
            current_state->set_abstract_obj(
                RESULT_REGISTER,
                AbstractObjectDomain(
                    AbstractObject(AbstractObjectKind::CLASS,
                                   DexType::make_type(internal_name))));
          } else {
            current_state->set_abstract_obj(
                RESULT_REGISTER,
                AbstractObjectDomain(
                    AbstractObject(AbstractObjectKind::CLASS, nullptr)));
          }

          current_state->set_class_source(
              RESULT_REGISTER,
              ClassObjectSourceDomain(ClassObjectSource::REFLECTION));
          break;
        }
      }
      update_return_object(current_state, insn);
      break;
    }
    case OPCODE_INVOKE_INTERFACE:
    case OPCODE_INVOKE_SUPER:
    case OPCODE_INVOKE_DIRECT: {
      update_return_object(current_state, insn);
      break;
    }
    default: {
      default_semantics(insn, current_state);
    }
    }
  }

  boost::optional<AbstractObject> get_abstract_object(
      size_t reg, IRInstruction* insn) const {
    auto it = m_environments.find(insn);
    if (it == m_environments.end()) {
      return boost::none;
    }
    return it->second.get_abstract_obj(reg).get_object();
  }

  boost::optional<ClassObjectSource> get_class_source(
      size_t reg, IRInstruction* insn) const {
    auto it = m_environments.find(insn);
    if (it == m_environments.end()) {
      return boost::none;
    }
    return it->second.get_class_source(reg).get_constant();
  }

 private:
  const cfg::ControlFlowGraph& m_cfg;
  std::unordered_map<IRInstruction*, AbstractObjectEnvironment> m_environments;

  void update_non_string_input(AbstractObjectEnvironment* current_state,
                               IRInstruction* insn,
                               DexType* type) const {
    auto dest_reg = insn->has_move_result() ? RESULT_REGISTER : insn->dest();
    if (type == get_class_type()) {
      // We don't have precise type information to which the Class obj refers
      // to.
      current_state->set_abstract_obj(dest_reg,
                                      AbstractObjectDomain(AbstractObject(
                                          AbstractObjectKind::CLASS, nullptr)));
      current_state->set_class_source(
          dest_reg, ClassObjectSourceDomain(ClassObjectSource::NON_REFLECTION));
    } else {
      current_state->set_abstract_obj(dest_reg,
                                      AbstractObjectDomain(AbstractObject(
                                          AbstractObjectKind::OBJECT, type)));
    }
  }

  void update_return_object(AbstractObjectEnvironment* current_state,
                            IRInstruction* insn) const {
    DexMethodRef* callee = insn->get_method();
    DexType* return_type = callee->get_proto()->get_rtype();
    if (is_void(return_type) || !is_object(return_type)) {
      return;
    }
    update_non_string_input(current_state, insn, return_type);
  }

  void default_semantics(IRInstruction* insn,
                         AbstractObjectEnvironment* current_state) const {
    // For instructions that are transparent for this analysis, we just need
    // to clobber the destination registers in the abstract environment. Note
    // that this also covers the MOVE_RESULT_* and MOVE_RESULT_PSEUDO_*
    // instructions following operations that are not considered by this
    // analysis. Hence, the effect of those operations is correctly abstracted
    // away regardless of the size of the destination register.
    if (insn->dests_size() > 0) {
      current_state->set_abstract_obj(insn->dest(),
                                      AbstractObjectDomain::top());
      if (insn->dest_is_wide()) {
        current_state->set_abstract_obj(insn->dest() + 1,
                                        AbstractObjectDomain::top());
      }
    }
    // We need to invalidate RESULT_REGISTER if the instruction writes into
    // this register.
    if (insn->has_move_result()) {
      current_state->set_abstract_obj(RESULT_REGISTER,
                                      AbstractObjectDomain::top());
    }
  }

  DexString* get_dex_string_from_insn(AbstractObjectEnvironment* current_state,
                                      IRInstruction* insn,
                                      register_t reg) const {
    auto element_name =
        current_state->get_abstract_obj(insn->src(reg)).get_object();
    if (element_name && element_name->obj_kind == STRING) {
      return element_name->dex_string;
    } else {
      return nullptr;
    }
  }

  void process_virtual_call(IRInstruction* insn,
                            const AbstractObject& receiver,
                            AbstractObjectEnvironment* current_state) const {
    DexMethodRef* callee = insn->get_method();
    switch (receiver.obj_kind) {
    case OBJECT: {
      if (callee == m_get_class) {
        current_state->set_abstract_obj(
            RESULT_REGISTER,
            AbstractObjectDomain(
                AbstractObject(AbstractObjectKind::CLASS, receiver.dex_type)));
        current_state->set_class_source(
            RESULT_REGISTER,
            ClassObjectSourceDomain(ClassObjectSource::REFLECTION));
        return;
      }
      break;
    }
    case STRING: {
      if (callee == m_get_class) {
        current_state->set_abstract_obj(
            RESULT_REGISTER,
            AbstractObjectDomain(
                AbstractObject(AbstractObjectKind::CLASS, get_string_type())));
        current_state->set_class_source(
            RESULT_REGISTER,
            ClassObjectSourceDomain(ClassObjectSource::REFLECTION));
        return;
      }
      break;
    }
    case CLASS: {
      AbstractObjectKind element_kind;
      DexString* element_name = nullptr;
      if (callee == m_get_method || callee == m_get_declared_method) {
        element_kind = METHOD;
        element_name = get_dex_string_from_insn(current_state, insn, 1);
      } else if (m_ctor_lookup_vmethods.count(callee) > 0) {
        element_kind = METHOD;
        // Hard code the <init> method name, to continue on treating this as
        // no different than a method.
        element_name = DexString::get_string("<init>");
      } else if (callee == m_get_field || callee == m_get_declared_field) {
        element_kind = FIELD;
        element_name = get_dex_string_from_insn(current_state, insn, 1);
      }
      if (element_name == nullptr) {
        break;
      }
      current_state->set_abstract_obj(
          RESULT_REGISTER,
          AbstractObjectDomain(
              AbstractObject(element_kind, receiver.dex_type, element_name)));
      return;
    }
    case FIELD:
    case METHOD: {
      if ((receiver.obj_kind == FIELD && callee == m_get_field_name) ||
          (receiver.obj_kind == METHOD && callee == m_get_method_name)) {
        current_state->set_abstract_obj(
            RESULT_REGISTER,
            AbstractObjectDomain(AbstractObject(receiver.dex_string)));
        return;
      }
      break;
    }
    }
    update_return_object(current_state, insn);
  }

  // After the fixpoint iteration completes, we replay the analysis on all
  // blocks and we cache the abstract state at each instruction. This cache is
  // used by get_abstract_object() to query the state of a register at a given
  // instruction. Since we use an abstract domain based on Patricia trees, the
  // memory footprint of storing the abstract state at each program point is
  // small.
  void populate_environments(const cfg::ControlFlowGraph& cfg) {
    // We reserve enough space for the map in order to avoid repeated
    // rehashing during the computation.
    m_environments.reserve(cfg.blocks().size() * 16);
    for (cfg::Block* block : cfg.blocks()) {
      AbstractObjectEnvironment current_state = get_entry_state_at(block);
      for (auto& mie : InstructionIterable(block)) {
        IRInstruction* insn = mie.insn;
        m_environments.emplace(insn, current_state);
        analyze_instruction(insn, &current_state);
      }
    }
  }

  DexMethodRef* m_get_class{DexMethod::make_method(
      "Ljava/lang/Object;", "getClass", {}, "Ljava/lang/Class;")};
  DexMethodRef* m_get_method{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getMethod",
                             {"Ljava/lang/String;", "[Ljava/lang/Class;"},
                             "Ljava/lang/reflect/Method;")};
  DexMethodRef* m_get_declared_method{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getDeclaredMethod",
                             {"Ljava/lang/String;", "[Ljava/lang/Class;"},
                             "Ljava/lang/reflect/Method;")};
  DexMethodRef* m_get_constructor{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getConstructor",
                             {"[Ljava/lang/Class;"},
                             "Ljava/lang/reflect/Constructor;")};
  DexMethodRef* m_get_declared_constructor{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getDeclaredConstructor",
                             {"[Ljava/lang/Class;"},
                             "Ljava/lang/reflect/Constructor;")};
  DexMethodRef* m_get_constructors{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getConstructors",
                             {},
                             "[Ljava/lang/reflect/Constructor;")};
  DexMethodRef* m_get_declared_constructors{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getDeclaredConstructors",
                             {},
                             "[Ljava/lang/reflect/Constructor;")};
  // Set of vmethods on java.lang.Class that can find constructors.
  std::unordered_set<DexMethodRef*> m_ctor_lookup_vmethods{{
      m_get_constructor,
      m_get_declared_constructor,
      m_get_constructors,
      m_get_declared_constructors,
  }};
  DexMethodRef* m_get_field{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getField",
                             {"Ljava/lang/String;"},
                             "Ljava/lang/reflect/Field;")};
  DexMethodRef* m_get_declared_field{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getDeclaredField",
                             {"Ljava/lang/String;"},
                             "Ljava/lang/reflect/Field;")};
  DexMethodRef* m_get_method_name{DexMethod::make_method(
      "Ljava/lang/reflect/Method;", "getName", {}, "Ljava/lang/String;")};
  DexMethodRef* m_get_field_name{DexMethod::make_method(
      "Ljava/lang/reflect/Field;", "getName", {}, "Ljava/lang/String;")};
  DexMethodRef* m_for_name{DexMethod::make_method("Ljava/lang/Class;",
                                                  "forName",
                                                  {"Ljava/lang/String;"},
                                                  "Ljava/lang/Class;")};
};

} // namespace impl

ReflectionAnalysis::~ReflectionAnalysis() {}

ReflectionAnalysis::ReflectionAnalysis(DexMethod* dex_method)
    : m_dex_method(dex_method) {
  always_assert(dex_method != nullptr);
  IRCode* code = dex_method->get_code();
  if (code == nullptr) {
    return;
  }
  code->build_cfg(/* editable */ false);
  cfg::ControlFlowGraph& cfg = code->cfg();
  cfg.calculate_exit_block();
  m_analyzer = std::make_unique<impl::Analyzer>(cfg);
  m_analyzer->run(dex_method);
}

void ReflectionAnalysis::get_reflection_site(
    const register_t reg,
    IRInstruction* insn,
    std::map<register_t, ReflectionAbstractObject>* abstract_objects) const {
  auto aobj = m_analyzer->get_abstract_object(reg, insn);
  if (!aobj) {
    return;
  }
  if (is_not_reflection_output(*aobj)) {
    return;
  }
  boost::optional<ClassObjectSource> cls_src =
      aobj->obj_kind == AbstractObjectKind::CLASS
          ? m_analyzer->get_class_source(reg, insn)
          : boost::none;
  if (aobj->obj_kind == AbstractObjectKind::CLASS &&
      cls_src == ClassObjectSource::NON_REFLECTION) {
    return;
  }
  if (traceEnabled(REFL, 5)) {
    std::ostringstream out;
    out << "reg " << reg << " " << *aobj << " ";
    if (cls_src) {
      out << *cls_src;
    }
    out << std::endl;
    TRACE(REFL, 5, " reflection site: %s\n", out.str().c_str());
  }
  (*abstract_objects)[reg] = ReflectionAbstractObject(*aobj, cls_src);
}

const ReflectionSites ReflectionAnalysis::get_reflection_sites() const {
  ReflectionSites reflection_sites;
  auto code = m_dex_method->get_code();
  if (code == nullptr) {
    return reflection_sites;
  }
  auto reg_size = code->get_registers_size();
  for (auto& mie : InstructionIterable(code)) {
    IRInstruction* insn = mie.insn;
    std::map<register_t, ReflectionAbstractObject> abstract_objects;
    for (size_t i = 0; i < reg_size; i++) {
      get_reflection_site(i, insn, &abstract_objects);
    }
    get_reflection_site(impl::RESULT_REGISTER, insn, &abstract_objects);

    if (!abstract_objects.empty()) {
      reflection_sites.push_back(std::make_pair(insn, abstract_objects));
    }
  }
  return reflection_sites;
}

bool ReflectionAnalysis::has_found_reflection() const {
  return !get_reflection_sites().empty();
}

boost::optional<AbstractObject> ReflectionAnalysis::get_abstract_object(
    size_t reg, IRInstruction* insn) const {
  if (m_analyzer == nullptr) {
    return boost::none;
  }
  return m_analyzer->get_abstract_object(reg, insn);
}

} // namespace reflection
