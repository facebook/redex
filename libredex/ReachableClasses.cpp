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
#include <string>
#include <unordered_set>
#include <fstream>
#include <string>

#include "Walkers.h"
#include "DexClass.h"
#include "Match.h"
#include "RedexResources.h"
#include "StringUtil.h"

namespace {

template<typename DexMember>
void mark_only_reachable_directly(DexMember* m) {
   m->rstate.ref_by_type();
}

/**
 * Indicates that a class is being used via reflection.
 *
 * If from_code is true, it's used from the dex files, otherwise it is
 * used by an XML file or from native code.
 *
 * Examples:
 *
 *   Bar.java: (from_code = true, directly created via reflection)
 *     Object x = Class.forName("com.facebook.Foo").newInstance();
 *
 *   MyGreatLayout.xml: (from_code = false, created when view is inflated)
 *     <com.facebook.MyTerrificView />
 */
void mark_reachable_by_classname(DexClass* dclass, bool from_code) {
  if (dclass == nullptr) return;
  dclass->rstate.ref_by_string(from_code);
  // When we mark a class as reachable, we also mark all fields and methods as
  // reachable.  Eventually we will be smarter about this, which will allow us
  // to remove unused methods and fields.
  for (DexMethod* dmethod : dclass->get_dmethods()) {
    dmethod->rstate.ref_by_string(from_code);
  }
  for (DexMethod* vmethod : dclass->get_vmethods()) {
    vmethod->rstate.ref_by_string(from_code);
  }
  for (DexField* sfield : dclass->get_sfields()) {
    sfield->rstate.ref_by_string(from_code);
  }
  for (DexField* ifield : dclass->get_ifields()) {
    ifield->rstate.ref_by_string(from_code);
  }
}

void mark_reachable_by_classname(DexType* dtype, bool from_code) {
  mark_reachable_by_classname(type_class_internal(dtype), from_code);
}

void mark_reachable_by_classname(std::string& classname, bool from_code) {
  DexString* dstring =
      DexString::get_string(classname.c_str(), (uint32_t)classname.size());
  DexType* dtype = DexType::get_type(dstring);
  if (dtype == nullptr) return;
  DexClass* dclass = type_class_internal(dtype);
  mark_reachable_by_classname(dclass, from_code);
}

template<typename T>
void mark_reachable_by_seed(T dclass) {
  if (dclass == nullptr) return;
  dclass->rstate.ref_by_seed();
}

void mark_reachable_by_seed(DexType* dtype) {
  if (dtype == nullptr) return;
  mark_reachable_by_seed(type_class_internal(dtype));
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
        m->rstate.ref_by_string(false);
      }
    }
    for (auto& m : cls->get_vmethods()) {
      if (methods_to_keep.count(m->get_name()->c_str())) {
        m->rstate.ref_by_string(false);
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

  auto match = std::make_tuple(
    m::const_string(/* const-string {vX}, <any string> */),
    m::invoke_static(/* invoke-static {vX}, java.lang.Class;.forName */
      m::opcode_method(m::named<DexMethod>("forName") && m::on_class<DexMethod>("Ljava/lang/Class;"))
      && m::has_n_args(1))
  );

  walk_matching_opcodes(scope, match, [&](const DexMethod* meth, size_t n, DexInstruction** insns){
    DexOpcodeString* const_string = (DexOpcodeString*)insns[0];
    DexOpcodeMethod* invoke_static = (DexOpcodeMethod*)insns[1];
    // Make sure that the registers agree
    if (const_string->dest() == invoke_static->src(0)) {
      std::string classname(const_string->get_string()->c_str());
      classname = "L" + classname + ";";
      std::replace(classname.begin(), classname.end(), '.', '/');
      TRACE(RENAME, 4, "Found Class.forName of: %s, marking %s reachable\n",
        const_string->get_string()->c_str(), classname.c_str());
      mark_reachable_by_classname(classname, true);
    }
  });

  std::string apk_dir;
  std::vector<std::string> reflected_package_names;
  std::vector<std::string> annotations;
  std::vector<std::string> class_members;
  std::vector<std::string> methods;

  pc.get("apk_dir", "", apk_dir);
  pc.get("keep_packages", {}, reflected_package_names);
  pc.get("keep_annotations", {}, annotations);
  pc.get("keep_class_members", {}, class_members);
  pc.get("keep_methods", {}, methods);

  std::unordered_set<DexType*> annotation_types(
    no_optimizations_anno.begin(),
    no_optimizations_anno.end());

  for (auto const& annostr : annotations) {
    DexType* anno = DexType::get_type(annostr.c_str());
    if (anno) annotation_types.insert(anno);
  }

  keep_annotated_classes(scope, annotation_types);
  keep_class_members(scope, class_members);
  keep_methods(scope, methods);

  if (apk_dir.size()) {
    // Classes present in manifest
    std::string manifest = apk_dir + std::string("/AndroidManifest.xml");
    for (std::string classname : get_manifest_classes(manifest)) {
      TRACE(PGR, 3, "manifest: %s\n", classname.c_str());
      mark_reachable_by_classname(classname, false);
    }

    // Classes present in XML layouts
    for (std::string classname : get_layout_classes(apk_dir)) {
      TRACE(PGR, 3, "xml_layout: %s\n", classname.c_str());
      mark_reachable_by_classname(classname, false);
    }

    // Classnames present in native libraries (lib/*/*.so)
    for (std::string classname : get_native_classes(apk_dir)) {
      auto type = DexType::get_type(classname.c_str());
      if (type == nullptr) continue;
      TRACE(PGR, 3, "native_lib: %s\n", classname.c_str());
      mark_reachable_by_classname(type, false);
    }
  }

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
      mark_reachable_by_classname(clazz, false);
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
  walk_methods(scope,
               [&](DexMethod* meth) {
                 if (meth->get_access() & DexAccessFlags::ACC_NATIVE) {
                   TRACE(PGR, 3, "native_method: %s\n", SHOW(meth->get_class()));
                   mark_reachable_by_classname(meth->get_class(), true);
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

namespace {
struct SeedsParser {
  SeedsParser(const ProguardMap& pgmap) : m_pgmap(pgmap) {}

  bool parse_seed_line(std::string line) {
    if (line.find(':') == std::string::npos) {
      return parse_class(line);
    }
    if (line.find('(') == std::string::npos) {
      return parse_field(line);
    }
    return parse_method(line);
  }

 private:
  bool parse_class(std::string line) {
    auto canon_name = convert_type(line);
    auto xlate_name = m_pgmap.translate_class(canon_name);
    auto dex_type = DexType::get_type(xlate_name.c_str());
    TRACE(PGR, 2,
          "Parsing seed class: %s\n"
          "  canon: %s\n"
          "  xlate: %s\n"
          "  interned: %s\n",
          line.c_str(), canon_name.c_str(), xlate_name.c_str(), SHOW(dex_type));
    auto nonrenamed_type = DexType::get_type(canon_name.c_str());
    if (!dex_type && !nonrenamed_type) {
      TRACE(PGR, 2,
            "Seed file contains class for which Dex type can't be found: %s\n",
            line.c_str());
      return false;
    }
    if (dex_type) {
      mark_reachable_by_seed(dex_type);
    }
    if (line.find('$') == std::string::npos) {
      if (nonrenamed_type) {
        mark_reachable_by_seed(nonrenamed_type);
      }
    }
    return true;
  }

  std::string convert_field(
    std::string cls,
    std::string type,
    std::string name
  ) {
    return convert_type(cls) + "." + name + ":" + convert_type(type);
  }

  std::string canonicalize_field(std::string line) {
    auto cpos = line.find(':');
    auto cls = line.substr(0, cpos);
    auto spos = line.find(' ', cpos + 2);
    auto type = line.substr(cpos + 2, spos - (cpos + 2));
    auto name = line.substr(spos + 1);
    return convert_field(cls, type, name);
  }

  bool parse_field(std::string line) {
    auto canon_field = canonicalize_field(line);
    auto xlate_name = m_pgmap.translate_field(canon_field);
    auto cls_end = xlate_name.find('.');
    auto name_start = cls_end + 1;
    auto name_end = xlate_name.find(':', name_start);
    auto type_start = name_end + 1;
    auto clsstr = xlate_name.substr(0, cls_end);
    auto namestr = xlate_name.substr(name_start, name_end - name_start);
    auto typestr = xlate_name.substr(type_start);
    auto dex_field = DexField::get_field(
      DexType::get_type(clsstr.c_str()),
      DexString::get_string(namestr.c_str()),
      DexType::get_type(typestr.c_str()));
    TRACE(PGR, 2,
          "Parsing seed field: %s\n"
          "  canon: %s\n"
          "  xlate: %s\n"
          "  interned: %s\n",
          line.c_str(), canon_field.c_str(), xlate_name.c_str(),
          SHOW(dex_field));
    if (!dex_field) {
      TRACE(PGR, 2,
            "Seed file contains field not found in dex: %s (obfuscated: %s)\n",
            canon_field.c_str(), xlate_name.c_str());
      return false;
    }
    mark_reachable_by_seed(dex_field);
    return true;
  }

  std::string convert_args(std::string args) {
    std::string ret;
    std::stringstream ss(args);
    std::string arg;
    while (std::getline(ss, arg, ',')) {
      ret += convert_type(arg);
    }
    return ret;
  }

  std::string convert_method(
    std::string cls,
    std::string type,
    std::string name,
    std::string args
  ) {
    return
      convert_type(cls)
      + "." + name
      + ":(" + convert_args(args) + ")"
      + convert_type(type);
  }

  std::string canonicalize_method(std::string line) {
    auto cls_end = line.find(':');
    auto type_start = cls_end + 2;
    auto type_end = line.find(' ', type_start);
    if (type_end == std::string::npos) {
      // It's an <init> constructor.
      auto args_start = line.find('(', type_start) + 1;
      auto args_end = line.find(')', args_start);
      auto cls = line.substr(0, cls_end);
      auto args = line.substr(args_start, args_end - args_start);
      return convert_method(cls, "void", "<init>", args);
    }
    auto name_start = type_end + 1;
    auto name_end = line.find('(', name_start);
    auto args_start = name_end + 1;
    auto args_end = line.find(')', args_start);
    auto cls = line.substr(0, cls_end);
    auto type = line.substr(type_start, type_end - type_start);
    auto name = line.substr(name_start, name_end - name_start);
    auto args = line.substr(args_start, args_end - args_start);
    return convert_method(cls, type, name, args);
  }

  bool parse_method(std::string line) {
    auto canon_method = canonicalize_method(line);
    auto xlate_method = m_pgmap.translate_method(canon_method);
    auto dex_method = DexMethod::get_method(xlate_method);
    TRACE(PGR, 2,
          "Parsing seed method: %s\n"
          "  canon: %s\n"
          "  xlate: %s\n"
          "  interned: %s\n",
          line.c_str(), canon_method.c_str(), xlate_method.c_str(),
          SHOW(dex_method));
    if (!dex_method) {
      TRACE(PGR, 2,
            "Seed file contains method not found in dex: %s (obfuscated: %s)\n",
            canon_method.c_str(), xlate_method.c_str());
      return false;
    }
    mark_reachable_by_seed(dex_method);
    return true;
  }

 private:
  const ProguardMap& m_pgmap;
};
}

unsigned int init_seed_classes(
  const std::string seeds_filename, const ProguardMap& pgmap
) {
  TRACE(PGR, 8, "Reading seed classes from %s\n", seeds_filename.c_str());
  auto start = std::chrono::high_resolution_clock::now();
  std::ifstream seeds_file(seeds_filename);
  SeedsParser parser(pgmap);
  unsigned int count = 0;
  if (!seeds_file) {
    TRACE(PGR, 8, "Seeds file %s was not found (ignoring error).",
          seeds_filename.c_str());
    return 0;
  }
  std::string line;
  while (getline(seeds_file, line)) {
    TRACE(PGR, 2, "Parsing seeds line: %s\n", line.c_str());
    if (parser.parse_seed_line(line)) ++count;
  }
  auto end = std::chrono::high_resolution_clock::now();
  TRACE(PGR, 1, "Read %d seed classes in %.1lf seconds\n", count,
        std::chrono::duration<double>(end - start).count());
  return count;
}
