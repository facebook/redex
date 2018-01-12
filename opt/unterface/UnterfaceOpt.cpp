/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "UnterfaceOpt.h"

#include "DexClass.h"
#include "DexUtil.h"
#include "Creators.h"
#include "Walkers.h"

namespace {

/**
 * Main Unterface struct used across the optimization.
 * Carries useful data used during optimization.
 */
struct Unterface {

  Unterface(DexClass* intf, std::unordered_set<DexClass*>& classes)
      : intf(intf),
        impls(classes.begin(), classes.end()),
        untf(nullptr),
        sw_field(nullptr),
        obj_field(nullptr),
        ctor(nullptr) {
    std::sort(impls.begin(), impls.end(),
        [](const DexClass* first, const DexClass* second) {
          return compare_dextypes(first->get_type(), second->get_type());
        });
  }

  // interface to optimize
  DexClass* intf;
  // implementors of the interface
  std::vector<DexClass*> impls;
  // unterface class creator
  ClassCreator* untf;
  // switch field
  DexField* sw_field;
  //object field
  DexField* obj_field;
  // unterface ctor
  DexMethod* ctor;
  // map from an implementor to the interface implemented methods.
  // The order of the methods is that of the interface vmethods.
  std::unordered_map<DexClass*, std::vector<DexMethod*>> methods;
};

/**
 * Create a DexMethod for an unterface ctor.
 */
DexMethodRef* obj_ctor() {
  static DexMethodRef* ctor = DexMethod::make_method(
      get_object_type(),
      DexString::make_string("<init>"),
      DexProto::make_proto(get_void_type(),
          DexTypeList::make_type_list(std::deque<DexType*>())));
  return ctor;
}

DexProto* get_updated_proto(DexProto* proto, DexType* impl, DexType* untf) {
  std::deque<DexType*> new_args;
  new_args.push_back(untf);
  for (auto arg : proto->get_args()->get_type_list()) {
    if (arg == impl) {
      new_args.push_back(untf);
    } else {
      new_args.push_back(arg);
    }
  }
  return DexProto::make_proto(
      proto->get_rtype() == impl ? untf : proto->get_rtype(),
      DexTypeList::make_type_list(std::move(new_args)));
}

// TODO: come up with a good story for names
DexString* get_name(DexString* base) {
  auto name =
      std::string(base->c_str()).substr(0, strlen(base->c_str()) - 1);
  return DexString::make_string((name + "__untf__;").c_str());
}

bool find_impl(DexType* type, Unterface& unterface) {
  return std::find_if(unterface.impls.begin(), unterface.impls.end(),
      [&](const DexClass* impl) {
        return impl->get_type() == type;
      }) != unterface.impls.end();
};

/**
 * Helper for update_impl_refereces() which performs the code transformation.
 *
 * TODO: this needs a serious rationalization around MEthodCreator and
 * IRCode usage. It works for now given the simplicity of the scenario.
 */
void do_update_method(DexMethod* meth, Unterface& unterface) {
  auto code = meth->get_code();
  code->set_registers_size(code->get_registers_size() + 1);
  IRInstruction* last = nullptr;
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    IRInstruction* invoke = nullptr;
    auto op = insn->opcode();
    switch (op) {
    case OPCODE_NEW_INSTANCE:
      if (find_impl(insn->get_type(), unterface)) {
        auto new_inst = new IRInstruction(OPCODE_NEW_INSTANCE);
        new_inst->set_type(unterface.untf->get_type())
            ->set_dest(insn->dest());
        code->replace_opcode(insn, new_inst);
        last = new_inst;
      } else {
        last = insn;
      }
      break;
    case OPCODE_INVOKE_DIRECT: {
      auto cls = insn->get_method()->get_class();
      for (size_t i = 0; i < unterface.impls.size(); i++) {
        auto impl = unterface.impls[i];
        if (impl->get_type() == cls) {
          auto load = new IRInstruction(OPCODE_CONST);
          load->set_dest(0);
          load->set_literal(static_cast<int32_t>(i));
          invoke = (new IRInstruction(OPCODE_INVOKE_DIRECT))
                       ->set_method(unterface.ctor);
          uint16_t arg_count = insn->srcs_size();
          invoke->set_arg_word_count(arg_count + 1);
          for (int j = 0; j < arg_count; j++) {
            invoke->set_src(j, insn->src(j) + 1);
          }
          invoke->set_src(arg_count, 0);
          code->remove_opcode(insn);
          std::vector<IRInstruction*> new_insns;
          new_insns.push_back(load);
          new_insns.push_back(invoke);
          code->insert_after(last, new_insns);
          last = invoke;
          break;
        }
      }
      if (invoke == nullptr) {
        last = insn;
      }
      break;
    }
    default:
      // TODO the other infinite number of cases...
      last = insn;
      break;
    }

    if (invoke == nullptr) {
      if (last->dests_size() != 0) {
        last->set_dest(last->dest() + 1);
      }
      for (int i = 0; i < static_cast<int>(last->srcs_size()); i++) {
        last->set_src(i, last->src(i) + 1);
      }
    }
  }
}

/**
 * Remove references to the implementors and change them to the unterface
 * reference.
 * Particularly take care of the constructor which have to be changed to
 * construct the unterface and pass the extra "switch type" argument.
 *
 * TODO: this is just an initial example and there is a ton more to do
 */
void update_impl_refereces(Scope& scope, Unterface& unterface) {
  std::vector<DexMethod*> to_change;
  walk::code(scope,
      [&](DexMethod* meth) {
        return !find_impl(meth->get_class(), unterface);
      },
      [&](DexMethod* meth, IRCode& code) {
        for (auto& mie : InstructionIterable(&code)) {
          auto insn = mie.insn;
          auto op = insn->opcode();
          switch (op) {
          case OPCODE_NEW_INSTANCE:
            if (find_impl(insn->get_type(), unterface)) {
              to_change.push_back(meth);
              return;
            }
            break;
          case OPCODE_INVOKE_DIRECT:
            if (find_impl(insn->get_method()->get_class(), unterface)) {
              to_change.push_back(meth);
              return;
            }
            break;
          default:
            // TODO the other infinite number of cases...
            break;
          }
        }
      });

  for (auto meth : to_change) {
    do_update_method(meth, unterface);
  }
}

/**
 * Create an invoke for each interface method that switches over the int
 * field and call into the correct static function that was moved from the
 * implementor of the interface.
 * Essentially given
 * interface I { Object m(); }
 * class Outer {
 *   class A implements I { public Object m() { return ...; } }
 *   class B implements I { public Object m() { return ...; } }
 * }
 * after this steps the unterface class looks like
 * class I__untf__ implements I {
 *   private int sw;
 *   private Object obj;
 *
 *   public I__untf__(Object obj, int sw) {
 *     this.obj = obj;
 *     this.sw = sw;
 *   }
 *
 *   // A method moved
 *   static void m00(I__untf__ thiz) {
 *     Object obj = thiz.obj;
 *     (Outer)obj....;
 *     return ...;
 *   }
 *
 *   // B method moved
 *   static void m10(I__untf__ thiz) {
 *     Object obj = thiz.obj;
 *     (Outer)obj....;
 *     return ...;
 *   }
 *
 *   // interface implementation
 *   public void m() {
 *     Object ret;
 *     switch(sw) {
 *     case 0: ret = m00(this); break;
 *     case 1: ret = m10(this); break;
 *     defaut: ret = null; break;
 *     }
 *     return ret;
 *   }
 * }
 *
 */
void build_invoke(Unterface& unterface) {
  auto vmethods = unterface.intf->get_vmethods();
  int i = 0;
  for (auto vmeth : vmethods) {
    auto proto = vmeth->get_proto();
    auto ret = proto->get_rtype();

    MethodCreator* mc = new MethodCreator(unterface.untf->get_type(),
        vmeth->get_name(), vmeth->get_proto(),
        vmeth->get_access() & ~ACC_ABSTRACT);
    auto ret_loc = ret != get_void_type() ?
        mc->make_local(ret) : Location::empty();
    auto mb = mc->get_main_block();
    auto switch_loc = mc->make_local(get_int_type());
    mb->iget(unterface.sw_field, mc->get_local(0), switch_loc);
    std::map<int, MethodBlock*> cases;
    for (int idx = 0; idx < static_cast<int>(unterface.impls.size()); idx++) {
      cases[idx] = nullptr;
    }
    auto def_block = mb->switch_op(switch_loc, cases);
    if (ret != get_void_type()) {
      def_block->load_null(ret_loc);
      mb->ret(ret_loc);
    } else {
      mb->ret_void();
    }
    for (auto case_block : cases) {
      std::vector<Location> args;
      args.push_back(mc->get_local(0));
      for (int loc = 0;
          loc < static_cast<int>(proto->get_args()->get_type_list().size());
          loc++) {
        args.push_back(mc->get_local(loc + 1));
      }
      case_block.second->invoke(
          unterface.methods[unterface.impls[case_block.first]][i], args);
      if (ret != get_void_type()) {
        case_block.second->move_result(ret_loc, ret);
      }
    }
    auto new_meth = mc->create();
    unterface.untf->add_method(new_meth);
    TRACE(UNTF, 8, "Generated implementation for %s\n", SHOW(new_meth));
    i++;
  }
}

/**
 * Helper for move_methods which performs the code transformation.
 */
void update_code(DexClass* cls, DexMethod* meth, DexField* new_field) {
  assert(cls->get_ifields().size() == 1);
  auto outer = cls->get_ifields().front();
  auto type = outer->get_type();
  auto code = meth->get_code();

  // collect all field access that use the outer field
  std::vector<IRInstruction*> field_ops;
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (is_iget(insn->opcode())) {
      if (insn->get_field() == outer) {
        field_ops.push_back(insn);
      }
    }
  }

  // transform every field access to the new field and add a check_cast
  for (auto fop : field_ops) {
    IRInstruction* new_fop = new IRInstruction(fop->opcode());
    new_fop->set_field(new_field);
    auto dst = fop->dest();
    new_fop->set_dest(dst);
    new_fop->set_src(0, fop->src(0));
    code->replace_opcode(fop, new_fop);
    auto* check_cast = new IRInstruction(OPCODE_CHECK_CAST);
    check_cast->set_type(type)->set_src(0, dst);
    std::vector<IRInstruction*> ops;
    ops.push_back(check_cast);
    TRACE(UNTF, 8, "Changed %s to\n%s\n%s\n", show(fop).c_str(),
        show(new_fop).c_str(), show(check_cast).c_str());
    code->insert_after(new_fop, ops);
  }
}

/**
 * For each implementation take the interface methods and move them to
 * static method in the unterface class.
 * Change every field access in the method to load the field in the unterface
 * class and also add a proper check cast to "specialize" the object.
 *
 * Essentially given
 * interface I { Object m(); }
 * class Outer {
 *   class A implements I { public Object m() { return ...; } }
 *   class B implements I { public Object m() { return ...; } }
 * }
 * after this steps the unterface class looks like
 * class I__untf__ implements I {
 *   private int sw;
 *   private Object obj;
 *
 *   public I__untf__(Object obj, int sw) {
 *     this.obj = obj;
 *     this.sw = sw;
 *   }
 *
 *   // A method moved
 *   static void m00(I__untf__ thiz) {
 *     Object obj = thiz.obj;
 *     (Outer)obj....;
 *     return ...;
 *   }
 *
 *   // B method moved
 *   static void m10(I__untf__ thiz) {
 *     Object obj = thiz.obj;
 *     (Outer)obj....;
 *     return ...;
 *   }
 * }
 *
 */
void move_methods(Unterface& unterface) {
  // load the type used by the class
  for (size_t i = 0; i < unterface.impls.size(); i++) {
    auto impl = unterface.impls[i];
    int j = 0;
    for (auto vmeth : impl->get_vmethods()) {
      // create the static method on the unterface class to host the
      // vmethod original code
      std::string smeth_name = vmeth->get_name()->c_str();
      smeth_name = smeth_name + std::to_string(i) + std::to_string(j++);
      auto name = DexString::make_string(smeth_name.c_str());
      auto smeth = MethodCreator::make_static_from(name,
          get_updated_proto(vmeth->get_proto(), impl->get_type(),
              unterface.untf->get_type()),
          vmeth, unterface.untf->get_class());
      unterface.methods[impl].push_back(smeth);
      update_code(impl, smeth, unterface.obj_field);
      TRACE(UNTF, 8, "Moved implementation to %s\n", SHOW(smeth));
    }
  }
}

/**
 * Create the unterface class given the interface to optimize.
 * The class contains 2 fields:
 * 1- an object used in the implementors (typically the outer class in anonymous
 * classes)
 * 2- an int field to switch on in order to invoke on the proper object
 * Defines the constructor which takes the 2 arguments to set up the fields.
 *
 * Essentially given
 * interface I { Object m(); }
 * class Outer {
 *   class A implements I { public Object m() { return ...; } }
 *   class B implements I { public Object m() { return ...; } }
 * }
 * after this steps the unterface class looks like
 * class I__untf__ implements I {
 *   private int sw;
 *   private Object obj;
 *
 *   public I__untf__(Object obj, int sw) {
 *     this.obj = obj;
 *     this.sw = sw;
 *   }
 * }
 *
 */
void make_unterface_class(Unterface& unterface) {
  TRACE(UNTF, 8, "Make unterface for %s\n", SHOW(unterface.intf));
  auto untf_type = DexType::make_type(get_name(
      unterface.intf->get_type()->get_name()));
  auto untf_cls = new ClassCreator(untf_type);
  untf_cls->set_super(get_object_type());
  untf_cls->set_access(ACC_PUBLIC);
  untf_cls->add_interface(unterface.intf->get_type());

  auto switch_field_name = DexString::make_string("sw");
  auto switch_field = static_cast<DexField*>(DexField::make_field(
      untf_cls->get_type(), switch_field_name, get_int_type()));
  switch_field->make_concrete(ACC_PRIVATE);
  untf_cls->add_field(switch_field);
  TRACE(UNTF, 8, "Unterface field %s\n", SHOW(switch_field));
  unterface.sw_field = switch_field;
  auto obj_field_name = DexString::make_string("obj");
  auto obj_field = static_cast<DexField*>(DexField::make_field(
      untf_cls->get_type(), obj_field_name, get_object_type()));
  obj_field->make_concrete(ACC_PRIVATE);
  untf_cls->add_field(obj_field);
  TRACE(UNTF, 8, "Unterface field %s\n", SHOW(obj_field));
  unterface.obj_field = obj_field;

  std::deque<DexType*> args{get_object_type(), get_int_type()};
  auto proto = DexProto::make_proto(get_void_type(),
      DexTypeList::make_type_list(std::move(args)));
  auto cr_ctor = new MethodCreator(untf_type, DexString::make_string("<init>"),
      proto, ACC_PUBLIC | ACC_CONSTRUCTOR);
  auto mb = cr_ctor->get_main_block();
  auto self = cr_ctor->get_local(0);
  auto obj = cr_ctor->get_local(1);
  auto sw = cr_ctor->get_local(2);
  mb->iput(obj_field, self, obj);
  mb->iput(switch_field, self, sw);
  std::vector<Location> ctor_args{cr_ctor->get_local(0)};
  mb->invoke(OPCODE_INVOKE_DIRECT, obj_ctor(), ctor_args);
  mb->ret_void();
  auto ctor = cr_ctor->create();
  untf_cls->add_method(ctor);
  TRACE(UNTF, 8, "Unterface ctor %s\n", SHOW(ctor));

  unterface.untf = untf_cls;
  unterface.ctor = ctor;
}

void optimize_interface(Scope& scope, Unterface& unterface) {
  TRACE(UNTF, 5, "Optimizing %s\n", SHOW(unterface.intf->get_type()));
  for (auto cls : unterface.impls) {
    TRACE(UNTF, 5, "Implementor %s\n", SHOW(cls->get_type()));
    (void)cls;
  }

  make_unterface_class(unterface);
  move_methods(unterface);
  build_invoke(unterface);
  update_impl_refereces(scope, unterface);
}

}

void optimize(Scope& scope, TypeRelationship& candidates,
    std::vector<DexClass*>& untfs, std::unordered_set<DexClass*>& removed) {
  for (auto& cand_it : candidates) {
    Unterface unterface(cand_it.first, cand_it.second);
    optimize_interface(scope, unterface);
    auto cls = unterface.untf->create();
    untfs.push_back(cls);
    for (auto rem : cand_it.second) {
      removed.insert(rem);
    }
  }
  TRACE(UNTF, 5, "Unterfaces created %ld\n", untfs.size());
}
