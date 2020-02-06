/*
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
                         const std::unordered_set<DexType*>& x) {
  if (x.empty()) {
    return out;
  }
  out << "(";
  for (auto i = x.begin(); i != x.end(); ++i) {
    out << SHOW(*i);
    if (std::next(i) != x.end()) {
      out << ",";
    }
  }
  out << ")";
  return out;
}

std::ostream& operator<<(std::ostream& out,
                         const reflection::AbstractObject& x) {
  switch (x.obj_kind) {
  case reflection::OBJECT: {
    out << "OBJECT{" << SHOW(x.dex_type) << x.potential_dex_types << "}";
    break;
  }
  case reflection::INT: {
    out << "INT{" << (x.dex_int ? std::to_string(*x.dex_int) : "none") << "}";
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
    out << "CLASS{" << SHOW(x.dex_type) << x.potential_dex_types << "}";
    break;
  }
  case reflection::FIELD: {
    out << "FIELD{" << SHOW(x.dex_type) << x.potential_dex_types << ":"
        << SHOW(x.dex_string) << "}";
    break;
  }
  case reflection::METHOD: {
    out << "METHOD{" << SHOW(x.dex_type) << x.potential_dex_types << ":"
        << SHOW(x.dex_string);

    if (x.dex_type_array) {
      out << "(";
      for (auto type : *x.dex_type_array) {
        out << type->str();
      }
      out << ")";
    }

    out << "}";
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

AbstractHeapAddress allocate_heap_address() {
  static AbstractHeapAddress addr = 1;
  return addr++;
}

bool is_not_reflection_output(const AbstractObject& obj) {
  switch (obj.obj_kind) {
  case reflection::OBJECT:
  case reflection::INT:
  case reflection::STRING:
    return true;
  default:
    return false;
  }
}

bool operator==(const AbstractObject& x, const AbstractObject& y) {
  if (x.obj_kind != y.obj_kind) {
    return false;
  }
  switch (x.obj_kind) {
  case INT: {
    return x.dex_int == y.dex_int;
  }
  case OBJECT: {
    return x.dex_type == y.dex_type &&
           x.potential_dex_types == y.potential_dex_types &&
           x.heap_address == y.heap_address &&
           x.dex_type_array == y.dex_type_array;
  }
  case CLASS: {
    return x.dex_type == y.dex_type &&
           x.potential_dex_types == y.potential_dex_types;
  }
  case STRING: {
    return x.dex_string == y.dex_string;
  }
  case FIELD: {
    return x.dex_type == y.dex_type &&
           x.potential_dex_types == y.potential_dex_types &&
           x.dex_string == y.dex_string;
  }
  case METHOD: {
    return x.dex_type == y.dex_type &&
           x.potential_dex_types == y.potential_dex_types &&
           x.dex_string == y.dex_string && x.dex_type_array == y.dex_type_array;
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
    case AbstractObjectKind::INT: {
      if (other.dex_int == boost::none) {
        return true;
      }
      break;
    }
    case AbstractObjectKind::CLASS:
    case AbstractObjectKind::OBJECT:
      if (dex_type && other.dex_type == nullptr) {
        return true;
      }
      if (dex_type_array && other.dex_type_array == boost::none) {
        return true;
      }
      if (heap_address && other.heap_address == 0) {
        return true;
      }
      break;
    case AbstractObjectKind::STRING:
      if (other.dex_string == nullptr) {
        return true;
      }
      break;
    case AbstractObjectKind::FIELD:
      if (other.dex_type == nullptr && other.dex_string == nullptr) {
        return true;
      }
      break;
    case AbstractObjectKind::METHOD:
      if (other.dex_type == nullptr && other.dex_string == nullptr) {
        return true;
      }
      if (dex_type_array && other.dex_type_array == boost::none) {
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
  case AbstractObjectKind::INT:
    // Be conservative and drop the int
    dex_int = boost::none;
    break;
  case AbstractObjectKind::OBJECT:
  case AbstractObjectKind::CLASS:
    // Be conservative and drop the type info
    dex_type = nullptr;
    heap_address = 0;
    dex_type_array = boost::none;
    potential_dex_types.clear();
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
    dex_type_array = boost::none;
    potential_dex_types.clear();
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
    PatriciaTreeMapAbstractEnvironment<reg_t, AbstractObjectDomain>;

using ClassObjectSourceEnvironment =
    PatriciaTreeMapAbstractEnvironment<reg_t, ClassObjectSourceDomain>;

using HeapClassArrayEnvironment = PatriciaTreeMapAbstractEnvironment<
    AbstractHeapAddress,
    ConstantAbstractDomain<std::vector<DexType*>>>;

class AbstractObjectEnvironment final
    : public ReducedProductAbstractDomain<AbstractObjectEnvironment,
                                          BasicAbstractObjectEnvironment,
                                          ClassObjectSourceEnvironment,
                                          HeapClassArrayEnvironment> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  static void reduce_product(
      std::tuple<BasicAbstractObjectEnvironment,
                 ClassObjectSourceEnvironment,
                 HeapClassArrayEnvironment>& /* product */) {}

  const AbstractObjectDomain get_abstract_obj(reg_t reg) const {
    return get<0>().get(reg);
  }

  void set_abstract_obj(reg_t reg, const AbstractObjectDomain aobj) {
    apply<0>([=](auto env) { env->set(reg, aobj); }, true);
  }

  void update_abstract_obj(
      reg_t reg,
      const std::function<AbstractObjectDomain(const AbstractObjectDomain&)>&
          operation) {
    apply<0>([=](auto env) { env->update(reg, operation); }, true);
  }

  const ClassObjectSourceDomain get_class_source(reg_t reg) const {
    return get<1>().get(reg);
  }

  void set_class_source(reg_t reg, const ClassObjectSourceDomain cls_src) {
    apply<1>([=](auto env) { env->set(reg, cls_src); }, true);
  }

  const ConstantAbstractDomain<std::vector<DexType*>> get_heap_class_array(
      AbstractHeapAddress addr) const {
    return get<2>().get(addr);
  }

  void set_heap_class_array(
      AbstractHeapAddress addr,
      const ConstantAbstractDomain<std::vector<DexType*>>& array) {
    apply<2>([=](auto env) { env->set(addr, array); }, true);
  }

  void set_heap_addr_to_top(AbstractHeapAddress addr) {
    auto domain = get_heap_class_array(addr);
    domain.set_to_top();
    set_heap_class_array(addr, domain);
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
      const IRInstruction* insn,
      AbstractObjectEnvironment* current_state) const override {
    switch (insn->opcode()) {
    case IOPCODE_LOAD_PARAM:
    case IOPCODE_LOAD_PARAM_OBJECT:
    case IOPCODE_LOAD_PARAM_WIDE: {
      // IOPCODE_LOAD_PARAM_* instructions have been processed before the
      // analysis.
      break;
    }
    case OPCODE_MOVE:
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
    case OPCODE_CONST: {
      current_state->set_abstract_obj(
          insn->dest(),
          AbstractObjectDomain(AbstractObject(insn->get_literal())));
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
      current_state->set_abstract_obj(
          RESULT_REGISTER,
          AbstractObjectDomain(
              AbstractObject(AbstractObjectKind::OBJECT, insn->get_type())));
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
    case OPCODE_INSTANCE_OF: {
      const auto aobj = current_state->get_abstract_obj(insn->src(0));
      auto obj = aobj.get_object();
      // Append the referenced type here to the potential dex types list.
      // Doing this increases the type information we have at the reflection
      // site. It's up to the user of the analysis  how to interpret this
      // information.
      if (obj && (obj->obj_kind == AbstractObjectKind::OBJECT) &&
          obj->dex_type) {
        auto dex_type = insn->get_type();
        if (obj->dex_type != dex_type) {
          obj->potential_dex_types.insert(dex_type);
          current_state->set_abstract_obj(
              insn->src(0),
              AbstractObjectDomain(AbstractObject(
                  obj->obj_kind, obj->dex_type, obj->potential_dex_types)));
        }
      }

      break;
    }
    case OPCODE_AGET_OBJECT: {
      const auto array_object =
          current_state->get_abstract_obj(insn->src(0)).get_object();
      if (array_object) {
        auto type = array_object->dex_type;
        if (type && type::is_array(type)) {
          const auto etype = type::get_array_component_type(type);
          update_non_string_input(current_state, insn, etype);
          break;
        }
      }
      default_semantics(insn, current_state);
      break;
    }
    case OPCODE_APUT_OBJECT: {
      // insn format: aput <source> <array> <offset>
      const auto source_object =
          current_state->get_abstract_obj(insn->src(0)).get_object();
      const auto array_object =
          current_state->get_abstract_obj(insn->src(1)).get_object();
      const auto offset_object =
          current_state->get_abstract_obj(insn->src(2)).get_object();

      if (source_object && source_object->obj_kind == CLASS && array_object &&
          array_object->is_known_class_array() && offset_object &&
          offset_object->obj_kind == INT) {

        auto type = source_object->dex_type;
        boost::optional<int64_t> offset = offset_object->dex_int;
        boost::optional<std::vector<DexType*>> class_array =
            current_state->get_heap_class_array(array_object->heap_address)
                .get_constant();

        if (offset && class_array && *offset >= 0 &&
            class_array->size() > (size_t)*offset) {
          (*class_array)[*offset] = type;
          current_state->set_heap_class_array(
              array_object->heap_address,
              ConstantAbstractDomain<std::vector<DexType*>>(*class_array));
        }
      }
      if (source_object && source_object->is_known_class_array()) {
        current_state->set_heap_addr_to_top(source_object->heap_address);
      }
      default_semantics(insn, current_state);
      break;
    }
    case OPCODE_IPUT_OBJECT:
    case OPCODE_SPUT_OBJECT: {
      const auto source_object =
          current_state->get_abstract_obj(insn->src(0)).get_object();
      if (source_object && source_object->is_known_class_array()) {
        current_state->set_heap_addr_to_top(source_object->heap_address);
      }
      break;
    }
    case OPCODE_IGET_OBJECT:
    case OPCODE_SGET_OBJECT: {
      always_assert(insn->has_field());
      const auto field = insn->get_field();
      DexType* primitive_type = check_primitive_type_class(field);
      if (primitive_type) {
        // The field being accessed is a Class object to a primitive type
        // likely being used for reflection
        auto aobj = AbstractObject(AbstractObjectKind::CLASS, primitive_type);
        current_state->set_abstract_obj(RESULT_REGISTER,
                                        AbstractObjectDomain(aobj));
        current_state->set_class_source(
            RESULT_REGISTER,
            ClassObjectSourceDomain(ClassObjectSource::REFLECTION));
      } else {
        update_non_string_input(current_state, insn, field->get_type());
      }
      break;
    }
    case OPCODE_NEW_INSTANCE: {
      current_state->set_abstract_obj(
          RESULT_REGISTER,
          AbstractObjectDomain(
              AbstractObject(AbstractObjectKind::OBJECT, insn->get_type())));
      break;
    }
    case OPCODE_NEW_ARRAY: {
      auto array_type = insn->get_type();
      always_assert(type::is_array(array_type));
      auto component_type = type::get_array_component_type(array_type);
      if (component_type ==
          DexType::make_type(DexString::make_string("Ljava/lang/Class;"))) {
        const auto aobj =
            current_state->get_abstract_obj(insn->src(0)).get_object();

        if (aobj && aobj->obj_kind == INT && aobj->dex_int) {
          AbstractHeapAddress addr = allocate_heap_address();
          int64_t size = *(aobj->dex_int);
          std::vector<DexType*> array(size);
          ConstantAbstractDomain<std::vector<DexType*>> heap_array(array);
          current_state->set_heap_class_array(addr, heap_array);
          current_state->set_abstract_obj(
              RESULT_REGISTER,
              AbstractObjectDomain(
                  AbstractObject(AbstractObjectKind::OBJECT, addr)));
          break;
        }
      }
      current_state->set_abstract_obj(
          RESULT_REGISTER,
          AbstractObjectDomain(
              AbstractObject(AbstractObjectKind::OBJECT, insn->get_type())));
      break;
    }
    case OPCODE_FILLED_NEW_ARRAY: {
      auto array_type = insn->get_type();
      always_assert(type::is_array(array_type));
      auto component_type = type::get_array_component_type(array_type);
      AbstractObject aobj(AbstractObjectKind::OBJECT, insn->get_type());
      if (component_type == DexType::make_type("Ljava/lang/Class;")) {
        auto arg_count = insn->srcs_size();
        std::vector<DexType*> known_types;
        known_types.reserve(arg_count);

        // collect known types from the filled new array
        for (auto src_reg : insn->srcs()) {
          auto reg_obj = current_state->get_abstract_obj(src_reg).get_object();
          if (reg_obj && reg_obj->obj_kind == CLASS && reg_obj->dex_type) {
            known_types.push_back(reg_obj->dex_type);
          }
        }

        if (known_types.size() == arg_count) {
          AbstractHeapAddress addr = allocate_heap_address();
          ConstantAbstractDomain<std::vector<DexType*>> heap_array(known_types);
          current_state->set_heap_class_array(addr, heap_array);
          aobj = AbstractObject(AbstractObjectKind::OBJECT, addr);
        }
      }

      current_state->set_abstract_obj(RESULT_REGISTER,
                                      AbstractObjectDomain(aobj));
      break;
    }
    case OPCODE_INVOKE_VIRTUAL: {
      auto receiver =
          current_state->get_abstract_obj(insn->src(0)).get_object();
      if (!receiver) {
        update_return_object_and_invalidate_heap_args(current_state, insn);
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
                DexString::make_string(java_names::external_to_internal(
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
      update_return_object_and_invalidate_heap_args(current_state, insn);
      break;
    }
    case OPCODE_INVOKE_INTERFACE:
    case OPCODE_INVOKE_SUPER:
    case OPCODE_INVOKE_DIRECT: {
      update_return_object_and_invalidate_heap_args(current_state, insn);
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
                               const IRInstruction* insn,
                               DexType* type) const {
    auto dest_reg =
        insn->has_move_result_any() ? RESULT_REGISTER : insn->dest();
    if (type == type::java_lang_Class()) {
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

  DexType* check_primitive_type_class(const DexFieldRef* field) const {
    std::map<const DexFieldRef*, DexType*, dexfields_comparator> field_to_type{
        {DexField::make_field("Ljava/lang/Boolean;.TYPE:Ljava/lang/Class;"),
         DexType::make_type("Z")},
        {DexField::make_field("Ljava/lang/Byte;.TYPE:Ljava/lang/Class;"),
         DexType::make_type("B")},
        {DexField::make_field("Ljava/lang/Character;.TYPE:Ljava/lang/Class;"),
         DexType::make_type("C")},
        {DexField::make_field("Ljava/lang/Short;.TYPE:Ljava/lang/Class;"),
         DexType::make_type("S")},
        {DexField::make_field("Ljava/lang/Integer;.TYPE:Ljava/lang/Class;"),
         DexType::make_type("I")},
        {DexField::make_field("Ljava/lang/Long;.TYPE:Ljava/lang/Class;"),
         DexType::make_type("J")},
        {DexField::make_field("Ljava/lang/Float;.TYPE:Ljava/lang/Class;"),
         DexType::make_type("F")},
        {DexField::make_field("Ljava/lang/Double;.TYPE:Ljava/lang/Class;"),
         DexType::make_type("D")},
    };
    auto type = field_to_type.find(field);
    if (type != field_to_type.end()) {
      return type->second;
    } else {
      return nullptr;
    }
  }

  void update_return_object_and_invalidate_heap_args(
      AbstractObjectEnvironment* current_state,
      const IRInstruction* insn) const {

    invalidate_argument_heap_objects(current_state, insn);
    DexMethodRef* callee = insn->get_method();
    DexType* return_type = callee->get_proto()->get_rtype();
    if (type::is_void(return_type) || !type::is_object(return_type)) {
      return;
    }
    update_non_string_input(current_state, insn, return_type);
  }

  void default_semantics(const IRInstruction* insn,
                         AbstractObjectEnvironment* current_state) const {
    // For instructions that are transparent for this analysis, we just need
    // to clobber the destination registers in the abstract environment. Note
    // that this also covers the MOVE_RESULT_* and MOVE_RESULT_PSEUDO_*
    // instructions following operations that are not considered by this
    // analysis. Hence, the effect of those operations is correctly abstracted
    // away regardless of the size of the destination register.
    if (insn->has_dest()) {
      current_state->set_abstract_obj(insn->dest(),
                                      AbstractObjectDomain::top());
      if (insn->dest_is_wide()) {
        current_state->set_abstract_obj(insn->dest() + 1,
                                        AbstractObjectDomain::top());
      }
    }
    // We need to invalidate RESULT_REGISTER if the instruction writes into
    // this register.
    if (insn->has_move_result_any()) {
      current_state->set_abstract_obj(RESULT_REGISTER,
                                      AbstractObjectDomain::top());
    }
  }

  DexString* get_dex_string_from_insn(AbstractObjectEnvironment* current_state,
                                      const IRInstruction* insn,
                                      reg_t reg) const {
    auto element_name =
        current_state->get_abstract_obj(insn->src(reg)).get_object();
    if (element_name && element_name->obj_kind == STRING) {
      return element_name->dex_string;
    } else {
      return nullptr;
    }
  }

  bool is_method_known_to_preserve_args(DexMethodRef* method) const {
    const std::set<DexMethodRef*, dexmethods_comparator> known_methods{
        m_get_method,
        m_get_declared_method,
    };
    return known_methods.count(method);
  }

  void invalidate_argument_heap_objects(
      AbstractObjectEnvironment* current_state,
      const IRInstruction* insn) const {

    if (!insn->has_method() ||
        is_method_known_to_preserve_args(insn->get_method())) {
      return;
    }

    for (const auto reg : insn->srcs()) {
      auto aobj = current_state->get_abstract_obj(reg).get_object();
      if (!aobj) {
        continue;
      }
      auto addr = aobj->heap_address;
      if (!addr) {
        continue;
      }
      current_state->set_heap_addr_to_top(addr);
    }
  }

  void process_virtual_call(const IRInstruction* insn,
                            const AbstractObject& receiver,
                            AbstractObjectEnvironment* current_state) const {
    DexMethodRef* callee = insn->get_method();
    switch (receiver.obj_kind) {
    case INT: {
      // calling on int, not valid
      break;
    }
    case OBJECT: {
      if (callee == m_get_class) {
        current_state->set_abstract_obj(
            RESULT_REGISTER,
            AbstractObjectDomain(AbstractObject(AbstractObjectKind::CLASS,
                                                receiver.dex_type,
                                                receiver.potential_dex_types)));
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
            AbstractObjectDomain(AbstractObject(AbstractObjectKind::CLASS,
                                                type::java_lang_String())));
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
      boost::optional<std::vector<DexType*>> method_param_types = boost::none;
      if (callee == m_get_method || callee == m_get_declared_method) {
        element_kind = METHOD;
        element_name = get_dex_string_from_insn(current_state, insn, 1);
        auto arr_reg = insn->src(2); // holds java.lang.Class array
        auto arr_obj = current_state->get_abstract_obj(arr_reg).get_object();
        if (arr_obj && arr_obj->is_known_class_array()) {
          auto maybe_array =
              current_state->get_heap_class_array(arr_obj->heap_address)
                  .get_constant();
          if (maybe_array) {
            method_param_types = *maybe_array;
          }
        }
      } else if (callee == m_get_constructor ||
                 callee == m_get_declared_constructor) {
        element_kind = METHOD;
        element_name = DexString::get_string("<init>");
        auto arr_reg = insn->src(1);
        auto arr_obj = current_state->get_abstract_obj(arr_reg).get_object();
        if (arr_obj && arr_obj->is_known_class_array()) {
          auto maybe_array =
              current_state->get_heap_class_array(arr_obj->heap_address)
                  .get_constant();
          if (maybe_array) {
            method_param_types = *maybe_array;
          }
        }
      } else if (callee == m_get_field || callee == m_get_declared_field) {
        element_kind = FIELD;
        element_name = get_dex_string_from_insn(current_state, insn, 1);
      } else if (callee == m_get_fields || callee == m_get_declared_fields) {
        element_kind = FIELD;
        element_name = DexString::get_string("");
      } else if (callee == m_get_methods || callee == m_get_declared_methods) {
        element_kind = METHOD;
        element_name = DexString::get_string("");
      } else if (callee == m_get_constructors ||
                 callee == m_get_declared_constructors) {
        element_kind = METHOD;
        element_name = DexString::get_string("<init>");
      }
      if (element_name == nullptr) {
        break;
      }
      AbstractObject aobj(element_kind,
                          receiver.dex_type,
                          element_name,
                          receiver.potential_dex_types);
      if (method_param_types) {
        aobj.dex_type_array = method_param_types;
      }
      current_state->set_abstract_obj(RESULT_REGISTER,
                                      AbstractObjectDomain(aobj));
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
    update_return_object_and_invalidate_heap_args(current_state, insn);
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
  DexMethodRef* m_get_methods{DexMethod::make_method(
      "Ljava/lang/Class;", "getMethods", {}, "[Ljava/lang/reflect/Method;")};
  DexMethodRef* m_get_declared_methods{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getDeclaredMethods",
                             {},
                             "[Ljava/lang/reflect/Method;")};
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
  DexMethodRef* m_get_fields{DexMethod::make_method(
      "Ljava/lang/Class;", "getFields", {}, "[Ljava/lang/reflect/Field;")};
  DexMethodRef* m_get_declared_fields{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getDeclaredFields",
                             {},
                             "[Ljava/lang/reflect/Field;")};
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
    const reg_t reg,
    IRInstruction* insn,
    std::map<reg_t, ReflectionAbstractObject>* abstract_objects) const {
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
    TRACE(REFL, 5, " reflection site: %s", out.str().c_str());
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
    std::map<reg_t, ReflectionAbstractObject> abstract_objects;
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

boost::optional<std::vector<DexType*>> ReflectionAnalysis::get_method_params(
    IRInstruction* invoke_insn) const {
  auto code = m_dex_method->get_code();
  IRInstruction* move_result_insn = nullptr;
  auto ii = InstructionIterable(code);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    auto* insn = it->insn;
    if (insn == invoke_insn) {
      move_result_insn = std::next(it)->insn;
      break;
    }
  }
  if (!move_result_insn ||
      !opcode::is_move_result(move_result_insn->opcode())) {
    return boost::none;
  }
  auto arg_param = get_abstract_object(impl::RESULT_REGISTER, move_result_insn);
  if (!arg_param ||
      arg_param->obj_kind != reflection::AbstractObjectKind::METHOD) {
    return boost::none;
  }
  return arg_param->dex_type_array;
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
