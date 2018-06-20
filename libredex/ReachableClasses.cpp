/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ReachableClasses.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <string>
#include <unordered_set>

#include "ClassHierarchy.h"
#include "Walkers.h"
#include "DexClass.h"
#include "Match.h"
#include "RedexResources.h"
#include "SimpleReflectionAnalysis.h"
#include "StringUtil.h"
#include "Walkers.h"

namespace {

using namespace sra;

template<typename T, typename F>
struct DexItemIter {
};

template<typename F>
struct DexItemIter<DexField*, F> {
  static void iterate(DexClass* cls, F& yield) {
    if (cls->is_external()) return;
    for (auto* field : cls->get_sfields()) {
      yield(field);
    }
    for (auto* field : cls->get_ifields()) {
      yield(field);
    }
  }
};

template<typename F>
struct DexItemIter<DexMethod*, F> {
  static void iterate(DexClass* cls, F& yield) {
    if (cls->is_external()) return;
    for (auto* method : cls->get_dmethods()) {
      yield(method);
    }
    for (auto* method : cls->get_vmethods()) {
      yield(method);
    }
  }
};

template<typename T>
void blacklist(DexType* type, DexString *name, bool declared) {
  auto* cls = type_class(type);
  if (cls) {
    auto yield = [&](T t) {
      if (t->get_name() != name) return;
      TRACE(PGR, 4, "SRA BLACKLIST: %s\n", SHOW(t));
      t->rstate.set_keep();
      if (!declared) {
        // TOOD: handle not declared case, go up inheritance tree
      }
    };
    DexItemIter<T, decltype(yield)>::iterate(cls, yield);
  }
}

void analyze_reflection(const Scope& scope) {
  enum ReflectionType {
    GET_FIELD,
    GET_DECLARED_FIELD,
    GET_METHOD,
    GET_DECLARED_METHOD,
    INT_UPDATER,
    LONG_UPDATER,
    REF_UPDATER,
  };

  const auto JAVA_LANG_CLASS = "Ljava/lang/Class;";
  const auto ATOMIC_INT_FIELD_UPDATER = "Ljava/util/concurrent/atomic/AtomicIntegerFieldUpdater;";
  const auto ATOMIC_LONG_FIELD_UPDATER = "Ljava/util/concurrent/atomic/AtomicLongFieldUpdater;";
  const auto ATOMIC_REF_FIELD_UPDATER = "Ljava/util/concurrent/atomic/AtomicReferenceFieldUpdater;";

  const std::unordered_map<std::string,
                           std::unordered_map<std::string, ReflectionType>>
      refls = {
          {JAVA_LANG_CLASS,
           {
               {"getField", GET_FIELD},
               {"getDeclaredField", GET_DECLARED_FIELD},
               {"getMethod", GET_METHOD},
               {"getDeclaredMethod", GET_DECLARED_METHOD},
           }},
          {ATOMIC_INT_FIELD_UPDATER,
           {
               {"newUpdater", INT_UPDATER},
           }},
          {ATOMIC_LONG_FIELD_UPDATER,
           {
               {"newUpdater", LONG_UPDATER},
           }},
          {ATOMIC_REF_FIELD_UPDATER,
           {
               {"newUpdater", REF_UPDATER},
           }},
      };

  walk::parallel::code(scope, [&refls](DexMethod* method, IRCode& code) {
      std::unique_ptr<SimpleReflectionAnalysis> analysis = nullptr;
      for (auto& mie : InstructionIterable(code)) {
        IRInstruction* insn = mie.insn;
        if (!is_invoke(insn->opcode())) {
          continue;
        }

        // See if it matches something in refls
        auto& method_name = insn->get_method()->get_name()->str();
        auto& method_class_name = insn->get_method()->get_class()->get_name()->str();
        auto method_map = refls.find(method_class_name);
        if (method_map == refls.end()) {
          continue;
        }

        auto refl_entry = method_map->second.find(method_name);
        if (refl_entry == method_map->second.end()) {
          continue;
        }

        ReflectionType refl_type = refl_entry->second;
        int arg_cls_idx = 0;
        int arg_str_idx = refl_type == ReflectionType::REF_UPDATER ? 2 : 1;

        // Instantiating the analysis object also runs the reflection analysis
        // on the method. So, we wait until we're sure we need it.
        // We use a unique_ptr so that we'll still only have one per method.
        if (!analysis) {
          analysis = std::make_unique<SimpleReflectionAnalysis>(method);
        }

        auto arg_cls = analysis->get_abstract_object(insn->src(arg_cls_idx), insn);
        auto arg_str = analysis->get_abstract_object(insn->src(arg_str_idx), insn);
        if ((arg_cls && arg_cls->kind == AbstractObjectKind::CLASS) &&
            (arg_str && arg_str->kind == AbstractObjectKind::STRING)) {
          TRACE(PGR, 4, "SRA ANALYZE: %s: type:%d %s.%s cls: %d %s %s str: %d %s %s\n",
                insn->get_method()->get_name()->str().c_str(),
                refl_type,
                method_class_name.c_str(),
                method_name.c_str(),
                arg_cls->kind, SHOW(arg_cls->dex_type), SHOW(arg_cls->dex_string),
                arg_str->kind, SHOW(arg_str->dex_type), SHOW(arg_str->dex_string)
                );
        switch (refl_type) {
          case GET_FIELD:
            blacklist<DexField*>(arg_cls->dex_type, arg_str->dex_string, true);
            break;
          case GET_DECLARED_FIELD:
            blacklist<DexField*>(arg_cls->dex_type, arg_str->dex_string, false);
            break;
          case GET_METHOD:
            blacklist<DexMethod*>(arg_cls->dex_type, arg_str->dex_string, true);
            break;
          case GET_DECLARED_METHOD:
            blacklist<DexMethod*>(arg_cls->dex_type, arg_str->dex_string, false);
            break;
          case INT_UPDATER:
            blacklist<DexField*>(arg_cls->dex_type, arg_str->dex_string, true);
            break;
          case LONG_UPDATER:
            blacklist<DexField*>(arg_cls->dex_type, arg_str->dex_string, true);
            break;
          case REF_UPDATER:
            blacklist<DexField*>(arg_cls->dex_type, arg_str->dex_string, true);
            break;
          }
        }
      }
    }
  );
}

template<typename DexMember>
void mark_only_reachable_directly(DexMember* m) {
   m->rstate.ref_by_type();
}

/**
 * Indicates that a class is being used via reflection.
 *
 * Examples:
 *
 *   Bar.java:
 *     Object x = Class.forName("com.facebook.Foo").newInstance();
 *
 *   MyGreatLayout.xml:
 *     <com.facebook.MyTerrificView />
 */
void mark_reachable_by_classname(DexClass* dclass) {
  if (dclass == nullptr) return;
  dclass->rstate.ref_by_string();
  // When we mark a class as reachable, we also mark all fields and methods as
  // reachable.  Eventually we will be smarter about this, which will allow us
  // to remove unused methods and fields.
  for (DexMethod* dmethod : dclass->get_dmethods()) {
    dmethod->rstate.ref_by_string();
  }
  for (DexMethod* vmethod : dclass->get_vmethods()) {
    vmethod->rstate.ref_by_string();
  }
  for (DexField* sfield : dclass->get_sfields()) {
    sfield->rstate.ref_by_string();
  }
  for (DexField* ifield : dclass->get_ifields()) {
    ifield->rstate.ref_by_string();
  }
}

void mark_reachable_by_classname(DexType* dtype) {
  mark_reachable_by_classname(type_class_internal(dtype));
}

void mark_reachable_by_classname(std::string& classname) {
  DexString* dstring =
      DexString::get_string(classname.c_str(), (uint32_t)classname.size());
  DexType* dtype = DexType::get_type(dstring);
  if (dtype == nullptr) return;
  DexClass* dclass = type_class_internal(dtype);
  mark_reachable_by_classname(dclass);
}

void mark_reachable_by_xml(const std::string& classname, bool is_manifest) {
  auto dtype = DexType::get_type(classname.c_str());
  if (dtype == nullptr) {
    return;
  }
  auto dclass = type_class(dtype);
  if (dclass == nullptr) {
    return;
  }
  if (is_manifest) {
    // Mark these as permanantly reachable. We don't ever try to prune classes
    // from the manifest.
    dclass->rstate.set_keep();
    // Prevent renaming.
    dclass->rstate.increment_keep_count();
    for (DexMethod* dmethod : dclass->get_ctors()) {
      dmethod->rstate.set_keep();
    }
  } else {
    // Setting "referenced_by_resource_xml" essentially behaves like keep,
    // though breaking it out to its own flag will let us clear/recompute this.
    dclass->rstate.set_referenced_by_resource_xml();
    // Mark the constructors as used, which should be the expected use case from
    // layout inflation.
    for (DexMethod* dmethod : dclass->get_ctors()) {
      dmethod->rstate.set_referenced_by_resource_xml();
    }
  }
}

// Possible methods for an android:onClick accept 1 argument that is a View.
// Source:
// https://android.googlesource.com/platform/frameworks/base/+/android-8.0.0_r15/core/java/android/view/View.java#5331
// Returns true if it matches that criteria, and it's in the set of known
// attribute values.
bool matches_onclick_method(
  const DexMethod* dmethod,
  const std::set<std::string>& names_to_keep) {
  auto prototype = dmethod->get_proto();
  auto args_list = prototype->get_args();
  if (args_list->size() == 1) {
    auto first_type = args_list->get_type_list()[0];
    if (strcmp(first_type->c_str(), "Landroid/view/View;") == 0) {
      std::string method_name = dmethod->c_str();
      return names_to_keep.count(method_name) > 0;
    }
  }
  return false;
}

// Simulates aapt's generated keep statements for any View which has an
// android:onClick="foo" attribute.
// Example (from aapt):
// -keepclassmembers class * { *** foo(...); }
//
// This version however is much more specific, since keeping every method "foo"
// is overkill. We only need to keep methods "foo" defined on a subclass of
// android.content.Context that accept 1 argument (an android.view.View).
void mark_onclick_attributes_reachable(
  const Scope& scope,
  const std::set<std::string>& onclick_attribute_values) {
  if (onclick_attribute_values.size() == 0) {
    return;
  }
  auto type_context = DexType::get_type("Landroid/content/Context;");
  always_assert(type_context != nullptr);

  auto class_hierarchy = build_type_hierarchy(scope);
  TypeSet children;
  get_all_children(class_hierarchy, type_context, children);

  for (const auto &t : children) {
    auto dclass = type_class(t);
    if (dclass->is_external()) {
      continue;
    }
    // Methods are invoked via reflection. Only public methods are relevant.
    for (const auto &m : dclass->get_vmethods()) {
      if (matches_onclick_method(m, onclick_attribute_values)) {
        TRACE(
          PGR,
          2,
          "Keeping vmethod %s due to onClick attribute in XML.\n",
          SHOW(m));
        m->rstate.set_referenced_by_resource_xml();
      }
    }
  }
}

// 1) Marks classes (Fragments, Views) found in XML layouts as reachable along
// with their constructors.
// 2) Marks candidate methods that could be called via android:onClick
// attributes.
void analyze_reachable_from_xml_layouts(
  const Scope& scope,
  const std::string& apk_dir) {
  std::unordered_set<std::string> layout_classes;
  std::unordered_set<std::string> attrs_to_read;
  // Method names used by reflection
  attrs_to_read.emplace(ONCLICK_ATTRIBUTE);
  std::unordered_multimap<std::string, std::string> attribute_values;
  collect_layout_classes_and_attributes(
    apk_dir,
    attrs_to_read,
    layout_classes,
    attribute_values);
  for (std::string classname : layout_classes) {
    TRACE(PGR, 3, "xml_layout: %s\n", classname.c_str());
    mark_reachable_by_xml(classname, false);
  }
  auto attr_values = multimap_values_to_set(
    attribute_values,
    ONCLICK_ATTRIBUTE);
  mark_onclick_attributes_reachable(scope, attr_values);
}

template <typename DexMember>
bool anno_set_contains(
  DexMember m,
  const std::unordered_set<DexType*>& keep_annotations
) {
  auto const& anno_set = m->get_anno_set();
  if (anno_set == nullptr) return false;
  auto const& annos = anno_set->get_annotations();
  for (auto const& anno : annos) {
    if (keep_annotations.count(anno->type())) {
      return true;
    }
  }
  return false;
}

void keep_annotated_classes(
  const Scope& scope,
  const std::unordered_set<DexType*>& keep_annotations
) {
  for (auto const& cls : scope) {
    if (anno_set_contains(cls, keep_annotations)) {
      mark_only_reachable_directly(cls);
    }
    for (auto const& m : cls->get_dmethods()) {
      if (anno_set_contains(m, keep_annotations)) {
        mark_only_reachable_directly(m);
      }
    }
    for (auto const& m : cls->get_vmethods()) {
      if (anno_set_contains(m, keep_annotations)) {
        mark_only_reachable_directly(m);
      }
    }
    for (auto const& m : cls->get_sfields()) {
      if (anno_set_contains(m, keep_annotations)) {
        mark_only_reachable_directly(m);
      }
    }
    for (auto const& m : cls->get_ifields()) {
      if (anno_set_contains(m, keep_annotations)) {
        mark_only_reachable_directly(m);
      }
    }
  }
}

/*
 * This method handles the keep_class_members from the configuration file.
 */
void keep_class_members(
    const Scope& scope,
    const std::vector<std::string>& keep_class_mems) {
  for (auto const& cls : scope) {
    std::string name = std::string(cls->get_type()->get_name()->c_str());
    for (auto const& class_mem : keep_class_mems) {
      std::string class_mem_str = std::string(class_mem.c_str());
      std::size_t pos = class_mem_str.find(name);
      if (pos != std::string::npos) {
        std::string rem_str = class_mem_str.substr(pos+name.size());
        for (auto const& f : cls->get_sfields()) {
          if (rem_str.find(std::string(f->get_name()->c_str()))!=std::string::npos) {
            mark_only_reachable_directly(f);
            mark_only_reachable_directly(cls);
          }
        }
        break;
      }
    }
  }
}

void keep_methods(const Scope& scope, const std::vector<std::string>& ms) {
  std::set<std::string> methods_to_keep(ms.begin(), ms.end());
  for (auto const& cls : scope) {
    for (auto& m : cls->get_dmethods()) {
      if (methods_to_keep.count(m->get_name()->c_str())) {
        m->rstate.ref_by_string();
      }
    }
    for (auto& m : cls->get_vmethods()) {
      if (methods_to_keep.count(m->get_name()->c_str())) {
        m->rstate.ref_by_string();
      }
    }
  }
}

/*
 * Returns true iff this class or any of its super classes are in the set of
 * classes banned due to use of complex reflection.
 */
bool in_reflected_pkg(DexClass* dclass,
                      std::unordered_set<DexClass*>& reflected_pkg_classes) {
  if (dclass == nullptr) {
    // Not in our dex files
    return false;
  }

  if (reflected_pkg_classes.count(dclass)) {
    return true;
  }
  return in_reflected_pkg(type_class_internal(dclass->get_super_class()),
                          reflected_pkg_classes);
}

/*
 * Initializes list of classes that are reachable via reflection, and calls
 * or from code.
 *
 * These include:
 *  - Classes used in the manifest (e.g. activities, services, etc)
 *  - View or Fragment classes used in layouts
 *  - Classes that are in certain packages (specified in the reflected_packages
 *    section of the config) and classes that extend from them
 *  - Classes marked with special annotations (keep_annotations in config)
 *  - Classes reachable from native libraries
 */
void init_permanently_reachable_classes(
  const Scope& scope,
  const Json::Value& config,
  const std::unordered_set<DexType*>& no_optimizations_anno
) {
  PassConfig pc(config);

  std::string apk_dir;
  std::vector<std::string> reflected_package_names;
  std::vector<std::string> annotations;
  std::vector<std::string> class_members;
  std::vector<std::string> methods;
  bool compute_xml_reachability;
  bool legacy_reflection_reachability;

  pc.get("apk_dir", "", apk_dir);
  pc.get("keep_packages", {}, reflected_package_names);
  pc.get("keep_annotations", {}, annotations);
  pc.get("keep_class_members", {}, class_members);
  pc.get("keep_methods", {}, methods);
  pc.get("compute_xml_reachability", true, compute_xml_reachability);
  pc.get("legacy_reflection_reachability", true, legacy_reflection_reachability);

  if (legacy_reflection_reachability) {
    auto match = std::make_tuple(
        m::const_string(/* const-string {vX}, <any string> */),
        m::move_result_pseudo(/* const-string {vX}, <any string> */),
        m::invoke_static(/* invoke-static {vX}, java.lang.Class;.forName */
                         m::opcode_method(
                             m::named<DexMethodRef>("forName") &&
                             m::on_class<DexMethodRef>("Ljava/lang/Class;")) &&
                         m::has_n_args(1)));

    walk::parallel::matching_opcodes(
        scope,
        match,
        [&](const DexMethod* meth, const std::vector<IRInstruction*>& insns) {
          auto const_string = insns[0];
          auto move_result_pseudo = insns[1];
          auto invoke_static = insns[2];
          // Make sure that the registers agree
          if (move_result_pseudo->dest() == invoke_static->src(0)) {
            auto classname = JavaNameUtil::external_to_internal(
                const_string->get_string()->c_str());
            TRACE(PGR,
                  4,
                  "Found Class.forName of: %s, marking %s reachable\n",
                  const_string->get_string()->c_str(),
                  classname.c_str());
            mark_reachable_by_classname(classname);
          }
        });
  }

  std::unordered_set<DexType*> annotation_types(
    no_optimizations_anno.begin(),
    no_optimizations_anno.end());

  for (auto const& annostr : annotations) {
    DexType* anno = DexType::get_type(annostr.c_str());
    if (anno) {
      annotation_types.insert(anno);
    } else {
      fprintf(stderr, "WARNING: keep annotation %s not found\n", annostr.c_str());
    }
  }

  keep_annotated_classes(scope, annotation_types);
  keep_class_members(scope, class_members);
  keep_methods(scope, methods);

  if (apk_dir.size()) {
    if (compute_xml_reachability) {
      // Classes present in manifest
      std::string manifest = apk_dir + std::string("/AndroidManifest.xml");
      for (std::string classname : get_manifest_classes(manifest)) {
        TRACE(PGR, 3, "manifest: %s\n", classname.c_str());
        mark_reachable_by_xml(classname, true);
      }

      // Classes present in XML layouts
      analyze_reachable_from_xml_layouts(scope, apk_dir);
    }

    // Classnames present in native libraries (lib/*/*.so)
    for (std::string classname : get_native_classes(apk_dir)) {
      auto type = DexType::get_type(classname.c_str());
      if (type == nullptr) continue;
      TRACE(PGR, 3, "native_lib: %s\n", classname.c_str());
      mark_reachable_by_classname(type);
    }
  }

  analyze_reflection(scope);

  std::unordered_set<DexClass*> reflected_package_classes;
  for (auto clazz : scope) {
    const char* cname = clazz->get_type()->get_name()->c_str();
    for (auto pkg : reflected_package_names) {
      if (starts_with(cname, pkg.c_str())) {
        reflected_package_classes.insert(clazz);
        continue;
      }
    }
  }
  for (auto clazz : scope) {
    if (in_reflected_pkg(clazz, reflected_package_classes)) {
      reflected_package_classes.insert(clazz);
      /* Note:
       * Some of these are by string, others by type
       * but we have no way in the config to distinguish
       * them currently.  So, we mark with the most
       * conservative sense here.
       */
      TRACE(PGR, 3, "reflected_package: %s\n", SHOW(clazz));
      mark_reachable_by_classname(clazz);
    }
  }
}

}

/**
 * Walks all the code of the app, finding classes that are reachable from
 * code.
 *
 * Note that as code is changed or removed by Redex, this information will
 * become stale, so this method should be called periodically, for example
 * after each pass.
 */
void recompute_classes_reachable_from_code(const Scope& scope) {
  // Matches methods marked as native
  walk::methods(scope,
               [&](DexMethod* meth) {
                 if (meth->get_access() & DexAccessFlags::ACC_NATIVE) {
                   TRACE(PGR, 3, "native_method: %s\n", SHOW(meth->get_class()));
                   mark_reachable_by_classname(meth->get_class());
                 }
               });
}

void init_reachable_classes(
    const Scope& scope,
    const Json::Value& config,
    const redex::ProguardConfiguration& pg_config,
    const std::unordered_set<DexType*>& no_optimizations_anno) {
  // Find classes that are reachable in such a way that none of the redex
  // passes will cause them to be no longer reachable.  For example, if a
  // class is referenced from the manifest.
  init_permanently_reachable_classes(scope, config, no_optimizations_anno);

  // Classes that are reachable in ways that could change as Redex runs. For
  // example, a class might be instantiated from a method, but if that method
  // is later deleted then it might no longer be reachable.
  recompute_classes_reachable_from_code(scope);
}

std::string ReferencedState::str() const {
  std::ostringstream s;
  s << m_bytype;
  s << m_bystring;
  s << m_byresources;
  s << m_keep;
  s << allowshrinking();
  s << allowobfuscation();
  s << m_assumenosideeffects;
  s << m_blanket_keepnames;
  s << m_whyareyoukeeping;
  s << ' ';
  s << m_keep_count;
  return s.str();
}
