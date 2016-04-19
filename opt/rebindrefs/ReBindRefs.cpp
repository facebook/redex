/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ReBindRefs.h"

#include <functional>
#include <vector>

#include "Debug.h"
#include "DexClass.h"
#include "DexOpcode.h"
#include "DexUtil.h"
#include "Resolver.h"
#include "PassManager.h"
#include "walkers.h"

namespace {

DexMethod* object_array_clone() {
  static DexMethod* clone = DexMethod::make_method(
    "[Ljava/lang/Object;",
    "clone",
    "Ljava/lang/Object;",
    {});
  return clone;
}

DexMethod* object_equals() {
  static DexMethod* equals = DexMethod::make_method(
    "Ljava/lang/Object;",
    "equals",
    "Z",
    {"Ljava/lang/Object;"});
  return equals;
}

DexMethod* object_hashCode() {
  static DexMethod* hashCode = DexMethod::make_method(
    "Ljava/lang/Object;",
    "hashCode",
    "I",
    {});
  return hashCode;
}

DexMethod* object_getClass() {
  static DexMethod* getClass = DexMethod::make_method(
    "Ljava/lang/Object;",
    "getClass",
    "Ljava/lang/Class;",
    {});
  return getClass;
}

bool is_object_equals(DexMethod* mref) {
  static DexString* equals = DexString::make_string("equals");
  static DexProto* bool_obj = DexProto::make_proto(
    DexType::make_type("Z"),
    DexTypeList::make_type_list({get_object_type()}));
  return mref->get_name() == equals && mref->get_proto() == bool_obj;
}

bool is_object_hashCode(DexMethod* mref) {
  static DexString* hashCode = DexString::make_string("hashCode");
  static DexProto* int_void = DexProto::make_proto(
    get_int_type(),
    DexTypeList::make_type_list({}));
  return mref->get_name() == hashCode && mref->get_proto() == int_void;
}

bool is_object_getClass(DexMethod* mref) {
  static DexString* getClass = DexString::make_string("getClass");
  static DexProto* cls_void = DexProto::make_proto(
    get_class_type(),
    DexTypeList::make_type_list({}));
  return mref->get_name() == getClass && mref->get_proto() == cls_void;
}

/**
 * Level of "compression" for rebinding.
 * The compression level relates exclusively to virtual invocation.
 * NORMAL:  virtual invocation are not resolved and left as they are
 * MEDIUM:  virtual invocation are resolved to the first definition up the
 *          hierarchy
 * HIGH:    virtual invocation are resolved to the top most definition up
 *          the hierarchy. That is, to the place where the method is first
 *          introduced
 */
enum RebindLevel {
  NORMAL = 0,
  MEDIUM = 1,
  HIGH = 2
};

struct Rebinder {
  Rebinder(Scope& scope, RebindLevel level)
      : m_scope(scope), m_level(level) {}

  void rewrite_refs() {
    walk_opcodes(
      m_scope,
      [](DexMethod*) { return true; },
      [&](DexMethod* m, DexOpcode* opcode) {
        switch (opcode->opcode()) {
          case OPCODE_INVOKE_INTERFACE:
          case OPCODE_INVOKE_INTERFACE_RANGE: {
            const auto mop = static_cast<DexOpcodeMethod*>(opcode);
            const auto mref = mop->get_method();
            rebind_method_opcode(mop, mref, resolve_intf_methodref(mref));
            break;
          }
          case OPCODE_INVOKE_VIRTUAL:
          case OPCODE_INVOKE_VIRTUAL_RANGE:
            if (m_level != RebindLevel::NORMAL) {
              rebind_virtual_method(opcode);
            }
            break;
          case OPCODE_INVOKE_STATIC:
          case OPCODE_INVOKE_STATIC_RANGE: {
            const auto mop = static_cast<DexOpcodeMethod*>(opcode);
            const auto mref = mop->get_method();
            rebind_method_opcode(mop, mref,
                resolve_method(mref, MethodSearch::Static));
            break;
          }
          case OPCODE_SGET:
          case OPCODE_SGET_WIDE:
          case OPCODE_SGET_OBJECT:
          case OPCODE_SGET_BOOLEAN:
          case OPCODE_SGET_BYTE:
          case OPCODE_SGET_CHAR:
          case OPCODE_SGET_SHORT:
            rebind_field(opcode, FieldSearch::Static);
            break;
          case OPCODE_IGET:
          case OPCODE_IGET_WIDE:
          case OPCODE_IGET_OBJECT:
          case OPCODE_IGET_BOOLEAN:
          case OPCODE_IGET_BYTE:
          case OPCODE_IGET_CHAR:
          case OPCODE_IGET_SHORT:
            rebind_field(opcode, FieldSearch::Instance);
            break;
          default:
            break;
        }
      });
  }

  void print_stats() {
    m_frefs.print("Field refs");
    m_mrefs.print("Method refs");
    m_array_clone_refs.print("Array clone");
  }

 private:
  template<typename T>
  struct RefStats {
    int count = 0;
    std::unordered_set<DexClass*> non_public;
    std::unordered_set<DexClass*> made_public;
    std::unordered_set<T> in;
    std::unordered_set<T> out;

    void insert(T tin, T tout) {
      ++count;
      in.emplace(tin);
      out.emplace(tout);
    }

    void print(const char* tag) {
      TRACE(BIND, 1,
          "%11s [refs total count: %6d, unique old refs: %6lu, "
          "unique new refs: %6lu, non public classes: %6lu, "
          "classes made public: %6lu]\n",
          tag, count, in.size(), out.size(),
          non_public.size(), made_public.size());
    }
  };

  void rebind_virtual_method(DexOpcode* opcode) {
    const auto mop = static_cast<DexOpcodeMethod*>(opcode);
    const auto mref = mop->get_method();
    auto mtype = mref->get_class();
    if (is_array_clone(mref, mtype)) {
      rebind_method_opcode(mop, mref, rebind_array_clone(mref));
      return;
    }

    // leave java.lang.String alone not to interfere with OP_EXECUTE_INLINE
    // and possibly any smart handling of String
    static auto str = DexType::make_type("Ljava/lang/String;");
    if (mtype == str) return;

    auto mdef = find_visible_ancestor(mref);
    rebind_method_opcode(mop, mref, mdef);
    return;
  }

  void rebind_method_opcode(
      DexOpcodeMethod* mop,
      DexMethod* mref,
      DexMethod* mdef) {
    if (mdef == nullptr || mdef == mref) {
      return;
    }
    auto cls = type_class(mdef->get_class());
    if (cls != nullptr && !is_public(cls)) {
      if (cls->is_external()) {
        m_mrefs.non_public.insert(cls);
        return;
      }
      m_mrefs.made_public.insert(cls);
      set_public(cls);
    }

    TRACE(BIND, 2, "Rebinding %s\n\t=>%s\n", SHOW(mref), SHOW(mdef));
    m_mrefs.insert(mref, mdef);
    mop->rewrite_method(mdef);
  }

  bool is_array_clone(DexMethod* mref, DexType* mtype) {
    static auto clone = DexString::make_string("clone");
    return is_array(mtype) &&
        mref->get_name() == clone &&
        !is_primitive(get_array_type(mtype));
  }

  DexMethod* rebind_array_clone(DexMethod* mref) {
    DexMethod* real_ref = object_array_clone();
    m_array_clone_refs.insert(mref, real_ref);
    return real_ref;
  }

  DexMethod* check_object_methods(DexMethod* mref) {
    if (is_object_equals(mref)) {
      return object_equals();
    } else if (is_object_hashCode(mref)) {
      return object_hashCode();
    } else if (is_object_getClass(mref)) {
      return object_getClass();
    }
    return nullptr;
  }

  void rebind_field(DexOpcode* opcode, FieldSearch field_search) {
    const auto fop = static_cast<DexOpcodeField*>(opcode);
    const auto fref = fop->field();
    const auto fdef = resolve_field(fref, field_search);
    if (fdef != nullptr && fdef != fref) {
      auto cls = type_class(fdef->get_class());
      if (!is_public(cls)) {
        if (cls->is_external()) {
          m_frefs.non_public.insert(cls);
          return;
        }
        m_frefs.made_public.insert(cls);
        set_public(cls);
      }
      TRACE(BIND,
            2,
            "Rebinding %s\n\t=>%s\n",
            SHOW(fref),
            SHOW(fdef));
      fop->rewrite_field(fdef);
      m_frefs.insert(fref, fdef);
    }
  }

  /**
   * Java allows relaxing visibility down the hierarchy chain so while
   * rebinding we don't want to bind to a method up the hierarchy that would
   * not be visible.
   * Walk up the hierarchy chain as long as the method is public.
   * If the RebindLevel is MEDIUM rebind to the first definition.
   */
  DexMethod* find_visible_ancestor(DexMethod* mref) {
    const auto mtype = mref->get_class();
    const auto name = mref->get_name();
    const auto proto = mref->get_proto();
    const DexClass* cls = type_class(mtype);
    DexMethod* super_def = nullptr;
    while (cls) {
      for (const auto& cls_meth : cls->get_vmethods()) {
        if (name == cls_meth->get_name() && proto == cls_meth->get_proto()) {
          auto curr_vis = cls_meth->get_access() & VISIBILITY_MASK;
          auto curr_cls_vis = cls->get_access() & VISIBILITY_MASK;
          if (curr_vis != ACC_PUBLIC || curr_cls_vis != ACC_PUBLIC) {
            return super_def != nullptr ? super_def : cls_meth;
          }
          if (super_def != nullptr) {
            if (m_level == RebindLevel::MEDIUM) {
              return super_def;
            }
            auto top_vis = super_def->get_access() & VISIBILITY_MASK;
            auto top_cls_vis = type_class(super_def->get_class())->get_access()
                & VISIBILITY_MASK;
            if (top_vis != curr_vis || top_cls_vis != curr_cls_vis) {
              return super_def;
            }
          }
          super_def = cls_meth;
          break;
        }
      }
      cls = type_class(cls->get_super_class());
    }
    // level MEDIUM and no super_def found is a bit of a stretch because
    // rebinding to java.lang.Object may skip some definition between
    // java.lang.Object the class that was not known
    if (m_level == RebindLevel::HIGH ||
        (m_level == RebindLevel::MEDIUM && super_def == nullptr)) {
      auto mdef = check_object_methods(mref);
      if (mdef != nullptr) {
        return mdef;
      }
    }
    return super_def;
  }

  Scope& m_scope;
  RebindLevel m_level;

  RefStats<DexField*> m_frefs;
  RefStats<DexMethod*> m_mrefs;
  RefStats<DexMethod*> m_array_clone_refs;
};

RebindLevel get_rebind_level(const folly::dynamic& config) {
  if (config.isObject()) {
    auto it = config.find("level");
    if (it != config.items().end()) {
      int l = it->second.asInt();
      switch (l) {
      case 0:
        return RebindLevel::NORMAL;
      case 1:
        return RebindLevel::MEDIUM;
      case 2:
        return RebindLevel::HIGH;
      }
    }
  }
  return RebindLevel::NORMAL;
}

}

void ReBindRefsPass::run_pass(DexClassesVector& dexen, ConfigFiles& cfg) {
  Scope scope = build_class_scope(dexen);
  auto level = get_rebind_level(m_config);
  Rebinder rb(scope, level);
  rb.rewrite_refs();
  rb.print_stats();
}
