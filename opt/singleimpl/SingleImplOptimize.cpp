/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdio.h>
#include <memory>
#include <string>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "SingleImpl.h"
#include "SingleImplUtil.h"
#include "Debug.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "Resolver.h"
#include "SingleImplDefs.h"
#include "Trace.h"
#include "Walkers.h"
#include "ClassHierarchy.h"

namespace {

/**
 * Rewrite all typerefs from the interfaces to the concrete type.
 */
void set_type_refs(DexType* intf, SingleImplData& data) {
  for (auto opcode : data.typerefs) {
    TRACE(INTF, 3, "(TREF) %s\n", SHOW(opcode));
    redex_assert(opcode->get_type() == intf);
    opcode->set_type(data.cls);
    TRACE(INTF, 3, "(TREF) \t=> %s\n", SHOW(opcode));
  }
}

/**
 * Get or create a new proto given an original proto and an interface to be
 * substituted by an implementation.
 */
DexProto* get_or_make_proto(DexType* intf, DexType* impl, DexProto* proto) {
  DexType* rtype = proto->get_rtype();
  if (rtype == intf) rtype = impl;
  DexTypeList* new_args = nullptr;
  const auto args = proto->get_args();
  std::deque<DexType*> new_arg_list;
  const auto& arg_list = args->get_type_list();
  for (const auto arg : arg_list) {
    new_arg_list.push_back(arg == intf ? impl : arg);
  }
  new_args = DexTypeList::make_type_list(std::move(new_arg_list));
  return DexProto::make_proto(rtype, new_args, proto->get_shorty());
}

/**
 * Given a new method and a corresponding existing, set up the new method
 * with everything from the original one.
 */
void setup_method(DexMethod* orig_method, DexMethod* new_method) {
  auto method_anno = orig_method->get_anno_set();
  if (method_anno) {
    new_method->attach_annotation_set(method_anno);
  }
  const auto& params_anno = orig_method->get_param_anno();
  if (params_anno) {
    for (auto const& param_anno : *params_anno) {
      new_method->attach_param_annotation_set(param_anno.first,
                                              param_anno.second);
    }
  }
  new_method->make_concrete(
      orig_method->get_access(), orig_method->release_code(),
      orig_method->is_virtual());
}

/**
 * Remove interfaces from classes. We walk the interface chain and move
 * down parent interfaces as needed so the contract of the class stays
 * the same.
 */
void remove_interface(DexType* intf, SingleImplData& data) {
  auto cls = type_class(data.cls);
  TRACE(INTF, 3, "(REMI) %s\n", SHOW(intf));

  // the interface and all its methods are public, but the impl may not be.
  // We make the impl public given the impl is now a substitute of the
  // interface. Doing the analysis to see all accesses would allow us to
  // determine proper visibility but for now we conservatively flip the impl
  // to public
  set_public(cls);
  // removing interfaces may bring the same parent interface down to the
  // concrete class, so use a set to guarantee uniqueness
  std::unordered_set<DexType*> new_intfs;
  auto collect_interfaces = [&](DexClass* impl) {
    auto intfs = impl->get_interfaces();
    auto intf_types = intfs->get_type_list();
    for (auto type : intf_types) {
      if (intf != type) {
        // make interface public if it was not already. It may happen
        // the parent interface is package protected (a type cannot be
        // private or protected) but the type implementing it is in a
        // different package. Make the interface public then
        auto type_cls = type_class(type);
        if (type_cls != nullptr) {
          if(!is_public(cls))
            set_public(type_cls);
          TRACE(INTF, 4, "(REMI) make PUBLIC - %s\n", SHOW(type));
        }
        new_intfs.insert(type);
        continue;
      }
    }
  };

  collect_interfaces(cls);
  collect_interfaces(type_class(intf));

  std::deque<DexType*> revisited_intfs;
  std::copy(
      new_intfs.begin(), new_intfs.end(), std::back_inserter(revisited_intfs));
  std::sort(revisited_intfs.begin(), revisited_intfs.end(), compare_dextypes);
  cls->set_interfaces(DexTypeList::make_type_list(std::move(revisited_intfs)));
  TRACE(INTF, 3, "(REMI)\t=> %s\n", SHOW(cls));
}

bool must_rewrite_annotations(const SingleImplConfig& config) {
  return config.field_anno || config.intf_anno || config.meth_anno;
}

bool must_set_method_annotations(const SingleImplConfig& config) {
  return config.meth_anno;
}

}

struct OptimizationImpl {
  OptimizationImpl(
      std::unique_ptr<SingleImplAnalysis> analysis,
      const ClassHierarchy& ch)
          : single_impls(std::move(analysis))
          , ch(ch) {}

  size_t optimize(Scope& scope, const SingleImplConfig& config);

 private:
  EscapeReason can_optimize(DexType* intf, SingleImplData& data, bool rename);
  void do_optimize(DexType* intf, SingleImplData& data);
  EscapeReason check_field_collision(DexType* intf, SingleImplData& data);
  EscapeReason check_method_collision(DexType* intf, SingleImplData& data);
  void drop_single_impl_collision(DexType* intf,
                                  SingleImplData& data,
                                  DexMethod* method);
  void set_field_defs(DexType* intf, SingleImplData& data);
  void set_field_refs(DexType* intf, SingleImplData& data);
  void set_method_defs(DexType* intf, SingleImplData& data);
  void set_method_refs(DexType* intf, SingleImplData& data);
  void rewrite_interface_methods(DexType* intf, SingleImplData& data);
  void rewrite_annotations(Scope& scope, const SingleImplConfig& config);
  void rename_possible_collisions(DexType* intf, SingleImplData& data);

 private:
  std::unique_ptr<SingleImplAnalysis> single_impls;
  // Maps from original methods to current rewritten methods.
  // For every interface optimization a set of methods are rewritten
  // however the analysis maps still hold the original methods to refer
  // to the various methods/refs to change.
  // So the optimizer access this map and grabs the latest updated method
  NewMethods new_methods;
  // list of optimized types
  std::unordered_set<DexType*> optimized;
  const ClassHierarchy& ch;
};

/**
 * Rewrite fields by creating new ones and transferring values from the
 * old fields to the new ones. Remove old field and add the new one
 * to the list of fields.
 */
void OptimizationImpl::set_field_defs(DexType* intf, SingleImplData& data) {
  for (const auto& field : data.fielddefs) {
    redex_assert(!single_impls->is_escaped(field->get_class()));
    auto f = static_cast<DexField*>(DexField::make_field(
        field->get_class(), field->get_name(), data.cls));
    redex_assert(f != field);
    TRACE(INTF, 3, "(FDEF) %s\n", SHOW(field));
    f->set_deobfuscated_name(field->get_deobfuscated_name());
    f->rstate = field->rstate;
    auto field_anno = field->get_anno_set();
    if (field_anno) {
      f->attach_annotation_set(field_anno);
    }
    f->make_concrete(field->get_access(), field->get_static_value());
    auto cls = type_class(field->get_class());
    cls->remove_field(field);
    cls->add_field(f);
    TRACE(INTF, 3, "(FDEF)\t=> %s\n", SHOW(f));
  }
}

/**
 * Rewrite all fieldref.
 */
void OptimizationImpl::set_field_refs(DexType* intf, SingleImplData& data) {
  for (const auto& fieldrefs : data.fieldrefs) {
    const auto field = fieldrefs.first;
    redex_assert(!single_impls->is_escaped(field->get_class()));
    DexFieldRef* f = DexField::make_field(
        field->get_class(), field->get_name(), data.cls);
    for (const auto opcode : fieldrefs.second) {
      TRACE(INTF, 3, "(FREF) %s\n", SHOW(opcode));
      redex_assert(f != opcode->get_field());
      opcode->set_field(f);
      TRACE(INTF, 3, "(FREF) \t=> %s\n", SHOW(opcode));
    }
  }
}

/**
 * Rewrite all methods sigs by creating new ones. Remove old methods and push
 * the new to the proper class method list.
 */
void OptimizationImpl::set_method_defs(DexType* intf,
                                       SingleImplData& data) {
  for (auto method : data.methoddefs) {
    TRACE(INTF, 3, "(MDEF) %s\n", SHOW(method));
    auto meth = method;
    auto meth_it = new_methods.find(meth);
    if (meth_it != new_methods.end()) {
      // if a method rewrite existed it must not be on a single impl
      // given we have escaped to next pass all collisions
      always_assert(meth_it->second->is_def());
      meth = static_cast<DexMethod*>(meth_it->second);
      TRACE(INTF, 4, "(MDEF) current: %s\n", SHOW(meth));
      redex_assert(!single_impls->is_single_impl(method->get_class()) ||
                   single_impls->is_escaped(method->get_class()));
      redex_assert(!single_impls->is_single_impl(meth->get_class()) ||
                   single_impls->is_escaped(meth->get_class()));
    }
    auto proto = get_or_make_proto(intf, data.cls, meth->get_proto());
    TRACE(INTF,
          5,
          "(MDEF) make_method: %s.%s - %s => %s\n",
          SHOW(meth->get_class()),
          SHOW(meth->get_name()),
          SHOW(meth->get_proto()),
          SHOW(proto));
    redex_assert(proto != meth->get_proto());
    auto new_meth = static_cast<DexMethod*>(DexMethod::make_method(
        meth->get_class(), meth->get_name(), proto));
    // new_meth may have already existed in RedexContext, so
    // we need to make sure it isn't concrete.
    // TODO: this is horrible. After we remove methods, we shouldn't
    // have these zombies lying around.
    new_meth->clear_annotations();
    new_meth->make_non_concrete();
    auto new_deob_name =
        type_class(meth->get_class())->get_deobfuscated_name() + "." +
        meth->get_simple_deobfuscated_name() + ":" + show_deobfuscated(proto);
    new_meth->set_deobfuscated_name(new_deob_name);
    new_meth->rstate = meth->rstate;
    redex_assert(new_meth != meth);
    setup_method(meth, new_meth);
    new_methods[method] = new_meth;
    auto owner = type_class(new_meth->get_class());
    owner->remove_method(meth);
    owner->add_method(new_meth);
    TRACE(INTF, 3, "(MDEF)\t=> %s\n", SHOW(new_meth));
  }
}

/**
 * Rewrite all method refs.
 */
void OptimizationImpl::set_method_refs(DexType* intf,
                                       SingleImplData& data) {
  for (auto mrefit : data.methodrefs) {
    auto method = mrefit.first;
    TRACE(INTF, 3, "(MREF)  %s\n", SHOW(method));
    auto new_meth = method;
    auto meth_it = new_methods.find(new_meth);
    if (meth_it != new_methods.end()) {
      // if a method rewrite existed it must not be on a single impl
      // given we have escaped to "next pass" all collisions
      new_meth = meth_it->second;
      TRACE(INTF, 4, "current: %s\n", SHOW(new_meth));
      redex_assert(!single_impls->is_single_impl(method->get_class()) ||
                   single_impls->is_escaped(method->get_class()));
      redex_assert(!single_impls->is_single_impl(new_meth->get_class()) ||
                   single_impls->is_escaped(new_meth->get_class()));
    }
    // next 2 lines will generate no new proto or method when the ref matches
    // a def, which should be very common.
    // However it does not seem too much of an overkill and more
    // straightforward that any logic that manages that.
    // Both creation of protos or methods is "interned" and so the same
    // method would be returned if there is nothing to change and create.
    // Still we need to change the opcodes where we assert the new method
    // to go in the opcode is in fact different than what was there.
    auto proto = get_or_make_proto(intf, data.cls, new_meth->get_proto());
    auto created_meth = DexMethod::make_method(
            new_meth->get_class(), new_meth->get_name(), proto);
    if (created_meth->is_def() && method->is_def()) {
      redex_assert(new_meth->is_def());
      auto new_deob_name =
          type_class(new_meth->get_class())->get_deobfuscated_name() + "." +
          show(static_cast<DexMethod*>(new_meth)
                   ->get_simple_deobfuscated_name()) +
          ":" + show_deobfuscated(proto);
      static_cast<DexMethod*>(created_meth)
          ->set_deobfuscated_name(new_deob_name);
      static_cast<DexMethod*>(created_meth)->rstate =
          static_cast<DexMethod*>(method)->rstate;
    }
    new_methods[method] = created_meth;
    TRACE(INTF, 3, "(MREF)\t=> %s\n", SHOW(created_meth));
    for (auto opcode : mrefit.second) {
      TRACE(INTF, 3, "(MREF) %s\n", SHOW(opcode));
      redex_assert(opcode->get_method() != created_meth);
      opcode->set_method(created_meth);
      TRACE(INTF, 3, "(MREF) \t=> %s\n", SHOW(opcode));
    }
  }
}

/**
 * Move all methods of the interface to the concrete (if not there already)
 * and rewrite all refs that were calling to the interface
 * (invoke-interface* -> invoke-virtual*).
 */
void OptimizationImpl::rewrite_interface_methods(DexType* intf,
                                                 SingleImplData& data) {
  auto intf_cls = type_class(intf);
  for (auto method : intf_cls->get_vmethods()) {
    auto meth = method;
    auto meth_it = new_methods.find(meth);
    if (meth_it != new_methods.end()) {
      always_assert(meth_it->second->is_def());
      meth = static_cast<DexMethod*>(meth_it->second);
    }
    drop_single_impl_collision(intf, data, meth);
    auto impl = type_class(data.cls);
    // Given an interface method and a class determine whether the method
    // is already defined in the class and use it if so.
    // An interface method can be defined in some base class for "convenience"
    // even though the base class does not implement the interface so we walk
    // the chain looking for the method.
    // NOTICE: if we have interfaces that have methods defined up the chain
    // in some java, android, google or other library we are screwed.
    // We'll not find the method and introduce a possible abstract one that
    // will break things.
    // Hopefully we'll find that out during verification and correct things.

    // get the new method if one was created (interface method with a single
    // impl in signature)
    TRACE(INTF, 3, "(MITF) interface method %s\n", SHOW(meth));
    auto new_meth = resolve_virtual(impl, meth->get_name(), meth->get_proto());
    if (!new_meth) {
      new_meth = static_cast<DexMethod*>(
          DexMethod::make_method(
              impl->get_type(), meth->get_name(), meth->get_proto()));
      // new_meth may not be new, because RedexContext keeps methods around
      // after they are deleted. clear all pre-existing method state.
      // See related TODO by searching for "this is horrible" in this file.
      new_meth->clear_annotations();
      new_meth->make_non_concrete();
      auto new_deob_name = impl->get_deobfuscated_name() + "." +
                           meth->get_simple_deobfuscated_name() + ":" +
                           show_deobfuscated(meth->get_proto());
      new_meth->set_deobfuscated_name(new_deob_name);
      new_meth->rstate = meth->rstate;
      TRACE(INTF, 5, "(MITF) created impl method %s\n", SHOW(new_meth));
      setup_method(meth, new_meth);
      redex_assert(new_meth->is_virtual());
      impl->add_method(new_meth);
      TRACE(INTF, 3, "(MITF) moved interface method %s\n", SHOW(new_meth));
    } else {
      TRACE(INTF, 3, "(MITF) found method impl %s\n", SHOW(new_meth));
    }
    redex_assert(new_methods.find(meth) == new_methods.end());
    new_methods[method] = new_meth;
  }

  // rewrite invoke-interface to invoke-virtual
  for (const auto& mref_it : data.intf_methodrefs) {
    auto m = mref_it.first;
    redex_assert(new_methods.find(m) != new_methods.end());
    auto new_m = new_methods[m];
    redex_assert(new_m && new_m != m);
    TRACE(INTF, 3, "(MITFOP) %s\n", SHOW(new_m));
    for (auto mop : mref_it.second) {
      TRACE(INTF, 3, "(MITFOP) %s\n", SHOW(mop));
      mop->set_method(new_m);
      auto op = mop->opcode();
      if (op == OPCODE_INVOKE_INTERFACE) {
        mop->set_opcode(OPCODE_INVOKE_VIRTUAL);
      }
      SingleImplPass::s_invoke_intf_count++;
      TRACE(INTF, 3, "(MITFOP)\t=>%s\n", SHOW(mop));
    }
  }
}

/**
 * Rewrite annotations that are referring to update methods or deleted interfaces.
 */
void OptimizationImpl::rewrite_annotations(Scope& scope, const SingleImplConfig& config) {
  // TODO: this is a hack to fix a problem with enclosing methods only.
  //       There are more dalvik annotations to review.
  //       The infrastructure is here but the code for all cases not yet
  auto enclosingMethod = DexType::get_type("Ldalvik/annotation/EnclosingMethod;");
  if (enclosingMethod == nullptr) return; // nothing to do
  if (!must_set_method_annotations(config)) return;
  for (const auto& cls : scope) {
    auto anno_set = cls->get_anno_set();
    if (anno_set == nullptr) continue;
    for (auto& anno : anno_set->get_annotations()) {
      if (anno->type() != enclosingMethod) continue;
      const auto& elems = anno->anno_elems();
      for (auto& elem : elems) {
        auto value = elem.encoded_value;
        if (value->evtype() == DexEncodedValueTypes::DEVT_METHOD) {
          auto method_value = static_cast<DexEncodedValueMethod*>(value);
          const auto& meth_it = new_methods.find(method_value->method());
          if (meth_it == new_methods.end()) continue;
          TRACE(INTF, 4, "REWRITE: %s\n", SHOW(anno));
          method_value->set_method(meth_it->second);
          TRACE(INTF, 4, "TO: %s\n", SHOW(anno));
        }
      }
    }
  }
}

/**
 * Check collisions in field definition.
 */
EscapeReason OptimizationImpl::check_field_collision(DexType* intf,
                                                     SingleImplData& data) {
  for (const auto field : data.fielddefs) {
    redex_assert(!single_impls->is_escaped(field->get_class()));
    auto collision =
        resolve_field(field->get_class(), field->get_name(), data.cls);
    if (collision) return FIELD_COLLISION;
  }
  return NO_ESCAPE;
}

/**
 * Check collisions in method definition.
 */
EscapeReason OptimizationImpl::check_method_collision(DexType* intf,
                                                      SingleImplData& data) {
  for (auto method : data.methoddefs) {
    auto meth_it = new_methods.find(method);
    if (meth_it != new_methods.end()) {
      always_assert(meth_it->second->is_def());
      method = static_cast<DexMethod*>(meth_it->second);
    }
    auto proto = get_or_make_proto(intf, data.cls, method->get_proto());
    redex_assert(proto != method->get_proto());
    DexMethod* collision = find_collision(ch,
                                          method->get_name(),
                                          proto,
                                          type_class(method->get_class()),
                                          method->is_virtual());
    if (collision) return SIG_COLLISION;
  }
  return NO_ESCAPE;
}

/**
 * Move all single impl in a single impl method signature to next pass.
 * We make a single optimization per pass over any given single impl so
 * I1, I2 and void I1.m(I2)
 * the first optimization (I1 or I2) moves the other interface to next pass.
 * That is not the case for methods on non optimizable classes, so for
 * I1, I2 and void C.m(I1, I2)
 * then m is changed in a single pass for both I1 and I2.
 */
void OptimizationImpl::drop_single_impl_collision(DexType* intf,
                                                  SingleImplData& data,
                                                  DexMethod* method) {
  auto check_type = [&](DexType* type) {
    if (type != intf && single_impls->is_single_impl(type) &&
        !single_impls->is_escaped(type)) {
      single_impls->escape_interface(type, NEXT_PASS);
      redex_assert(optimized.find(type) == optimized.end());
    }
  };

  auto owner = method->get_class();
  if (!single_impls->is_single_impl(owner)) return;
  check_type(owner);
  auto proto = method->get_proto();
  check_type(proto->get_rtype());
  auto args_list = proto->get_args();
  for (auto arg : args_list->get_type_list()) {
    check_type(arg);
  }
}

/**
 * A single impl can be optimized if:
 * 1- there is no collision in fields rewrite
 * 2- there is no collision in methods rewrite
 */
EscapeReason OptimizationImpl::can_optimize(DexType* intf,
                                            SingleImplData& data,
                                            bool rename) {
  auto escape = check_field_collision(intf, data);
  if (escape != EscapeReason::NO_ESCAPE) return escape;
  escape = check_method_collision(intf, data);
  if (escape != EscapeReason::NO_ESCAPE) {
    if (rename) {
      rename_possible_collisions(intf, data);
      escape = check_method_collision(intf, data);
    }
    if (escape != EscapeReason::NO_ESCAPE) return escape;
  }
  for (auto method : data.methoddefs) {
    drop_single_impl_collision(intf, data, method);
  }
  return NO_ESCAPE;
}

/**
 * Remove any chance for collisions.
 */
void OptimizationImpl::rename_possible_collisions(
    DexType* intf, SingleImplData& data) {

  const auto& rename = [](DexMethodRef* meth, DexString* name) {
    DexMethodSpec spec;
    spec.cls = meth->get_class();
    spec.name = name;
    spec.proto = meth->get_proto();
    meth->change(
        spec, false /* rename on collision */, true /* update deob name */);
  };

  TRACE(INTF, 9, "Changing name related to %s\n", SHOW(intf));
  for (const auto& meth : data.methoddefs) {
    if (!can_rename(meth)) {
      TRACE(INTF, 9, "Changing name but cannot rename %s, give up\n",
          SHOW(meth));
      return;
    }
  }

  std::unordered_map<DexString*, DexString*> names;
  const auto new_name = [&](DexString* name) {
    const auto& name_it = names.find(name);
    if (name_it != names.end()) {
      return name_it->second;
    }
    DexString* possible_name = nullptr;
    static std::string sufx("$r");
    static int intf_id = 0;
    while(true) {
      const auto& str = name->str() + sufx + std::to_string(intf_id++);
      possible_name = DexString::get_string(str.c_str());
      if (possible_name == nullptr) {
        possible_name = DexString::make_string(str.c_str());
        break;
      }
    }
    names[name] = possible_name;
    return possible_name;
  };

  for (const auto& meth : data.methoddefs) {
    if (is_constructor(meth)) continue;
    auto name = new_name(meth->get_name());
    TRACE(INTF, 9, "Changing name for %s to %s\n", SHOW(meth), SHOW(name));
    rename(meth, name);
  }
  for (const auto& refs_it : data.methodrefs) {
    if (refs_it.first->is_def()) continue;
    static auto init = DexString::make_string("<init>");
    always_assert(refs_it.first->get_name() != init);
    auto name = new_name(refs_it.first->get_name());
    TRACE(INTF, 9, "Changing name for %s to %s\n",
        SHOW(refs_it.first), SHOW(name));
    if (names.count(refs_it.first->get_name()) == 0) {
      TRACE(INTF, 9, "Changing name on missing method def %s",
          SHOW(refs_it.first));
    }
    rename(refs_it.first, name);
  }
}

/**
 * Perform the optimization.
 */
void OptimizationImpl::do_optimize(DexType* intf, SingleImplData& data) {
  set_type_refs(intf, data);
  set_field_defs(intf, data);
  set_field_refs(intf, data);
  set_method_defs(intf, data);
  set_method_refs(intf, data);
  rewrite_interface_methods(intf, data);
  remove_interface(intf, data);
}

/**
 * Run an optimization step.
 */
size_t OptimizationImpl::optimize(
    Scope& scope, const SingleImplConfig& config) {
  TypeList to_optimize;
  single_impls->get_interfaces(to_optimize);
  for (auto intf : to_optimize) {
    auto& intf_data = single_impls->get_single_impl_data(intf);
    TRACE(INTF, 3, "(OPT) %s => %s\n", SHOW(intf), SHOW(intf_data.cls));
    if (intf_data.is_escaped()) continue;
    auto escape = can_optimize(intf, intf_data, config.rename_on_collision);
    if (escape != EscapeReason::NO_ESCAPE) {
      single_impls->escape_interface(intf, escape);
      continue;
    }
    do_optimize(intf, intf_data);
    optimized.insert(intf);
  }

  // make a new scope deleting all single impl interfaces
  Scope new_scope;
  for (auto cls : scope) {
    if (optimized.find(cls->get_type()) != optimized.end()) continue;
    new_scope.push_back(cls);
  }
  scope.swap(new_scope);

  if (must_rewrite_annotations(config)) {
    rewrite_annotations(scope, config);
  }

  return optimized.size();
}

/**
 * Entry point for an optimization pass.
 */
size_t optimize(
    std::unique_ptr<SingleImplAnalysis> analysis,
    const ClassHierarchy& ch,
    Scope& scope, const SingleImplConfig& config) {
  OptimizationImpl optimizer(std::move(analysis), ch);
  return optimizer.optimize(scope, config);
}
