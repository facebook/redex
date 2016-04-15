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

#include "walkers.h"
#include "DexClass.h"
#include "Predicate.h"
#include "RedexResources.h"

namespace {

std::unordered_set<DexField*> do_not_strip_fields;
std::unordered_set<DexMethod*> do_not_strip_methods;
std::unordered_set<DexClass*> do_not_strip_classes;

// Note: this method will return nullptr if the dotname refers to an unknown
// type.
DexType* get_dextype_from_dotname(const char* dotname) {
  if (dotname == nullptr) {
    return nullptr;
  }
  std::string buf;
  buf.reserve(strlen(dotname) + 2);
  buf += 'L';
  buf += dotname;
  buf += ';';
  std::replace(buf.begin(), buf.end(), '.', '/');
  return DexType::get_type(buf.c_str());
}

/**
 * Class is used directly in code (As opposed to used via reflection)
 *
 * For example, it could be used by one of these instructions:
 *   check-cast
 *   new-instance
 *   const-class
 *   instance-of
 */
void mark_reachable_directly(DexClass* dclass) {
  if (dclass == nullptr) return;
  dclass->rstate.ref_by_type();
  // When we mark a class as reachable, we also mark all fields and methods as
  // reachable.  Eventually we will be smarter about this, which will allow us
  // to remove unused methods and fields.
  for (DexMethod* dmethod : dclass->get_dmethods()) {
    dmethod->rstate.ref_by_type();
  }
  for (DexMethod* vmethod : dclass->get_vmethods()) {
    vmethod->rstate.ref_by_type();
  }
  for (DexField* sfield : dclass->get_sfields()) {
    sfield->rstate.ref_by_type();
  }
  for (DexField* ifield : dclass->get_ifields()) {
    ifield->rstate.ref_by_type();
  }
}

void mark_reachable_directly(DexType* dtype) {
  if (dtype == nullptr) return;
  mark_reachable_directly(type_class_internal(dtype));
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
      DexString::get_string(classname.c_str(), classname.size());
  DexType* dtype = DexType::get_type(dstring);
  if (dtype == nullptr) return;
  DexClass* dclass = type_class_internal(dtype);
  mark_reachable_by_classname(dclass, from_code);
}

void mark_reachable_by_seed(DexClass* dclass) {
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
      do_not_strip_classes.insert(cls);
    }
    for (auto const& m : cls->get_dmethods()) {
      if (anno_set_contains(m, keep_annotations)) {
        do_not_strip_methods.insert(m);
      }
    }
    for (auto const& m : cls->get_vmethods()) {
      if (anno_set_contains(m, keep_annotations)) {
        do_not_strip_methods.insert(m);
      }
    }
    for (auto const& m : cls->get_sfields()) {
      if (anno_set_contains(m, keep_annotations)) {
        do_not_strip_fields.insert(m);
      }
    }
    for (auto const& m : cls->get_ifields()) {
      if (anno_set_contains(m, keep_annotations)) {
        do_not_strip_fields.insert(m);
      }
    }
  }
}

void keep_packages(
  const Scope& scope,
  const std::vector<std::string>& keep_pkgs
) {
  for (auto const& cls : scope) {
    auto name = cls->get_type()->get_name()->c_str();
    for (auto const& pkg : keep_pkgs) {
      if (strstr(name, pkg.c_str())) {
        do_not_strip_classes.insert(cls);
        for (auto const& m : cls->get_dmethods()) {
          do_not_strip_methods.insert(m);
        }
        for (auto const& m : cls->get_vmethods()) {
          do_not_strip_methods.insert(m);
        }
        for (auto const& f : cls->get_sfields()) {
          do_not_strip_fields.insert(f);
        }
        for (auto const& f : cls->get_ifields()) {
          do_not_strip_fields.insert(f);
        }
        break;
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
    folly::dynamic& config,
    const std::vector<KeepRule>& proguard_rules,
    const std::unordered_set<DexType*>& no_optimizations_anno) {

  std::string apk_dir;
  std::vector<std::string> reflected_package_names;

  auto config_apk_dir = config.find("apk_dir");
  if (config_apk_dir != config.items().end()) {
    apk_dir = toStdString(config_apk_dir->second.asString());
  }

  auto config_reflected_package_names = config.find("reflected_packages");
  if (config_reflected_package_names != config.items().end()) {
    for (auto config_pkg_name : config_reflected_package_names->second) {
      std::string pkg_name = toStdString(config_pkg_name.asString());
      reflected_package_names.push_back(pkg_name);
    }
  }

  std::unordered_set<DexType*> keep_annotations;
  auto config_keep_annotations = config.find("keep_annotations");
  if (config_keep_annotations != config.items().end()) {
    for (auto const& config_anno_name : config_keep_annotations->second) {
      std::string anno_name = toStdString(config_anno_name.asString());
      DexType* anno = DexType::get_type(anno_name.c_str());
      if (anno) keep_annotations.insert(anno);
    }
  }
  for (const auto& anno : no_optimizations_anno) {
    keep_annotations.insert(anno);
  }
  keep_annotated_classes(scope, keep_annotations);

  std::vector<std::string> keep_pkgs;
  auto config_keep_packages = config.find("keep_packages");
  if (config_keep_packages != config.items().end()) {
    for (auto const& config_pkg : config_keep_packages->second) {
      auto pkg_name = toStdString(config_pkg.asString());
      keep_pkgs.push_back(pkg_name);
    }
  }
  keep_packages(scope, keep_pkgs);

  if (apk_dir.size()) {
    // Classes present in manifest
    std::string manifest = apk_dir + std::string("/AndroidManifest.xml");
    for (std::string classname : get_manifest_classes(manifest)) {
      mark_reachable_by_classname(classname, false);
    }

    // Classes present in XML layouts
    for (std::string classname : get_layout_classes(apk_dir)) {
      mark_reachable_by_classname(classname, false);
    }

    // Classnames present in native libraries (lib/*/*.so)
    for (std::string classname : get_native_classes(apk_dir)) {
      mark_reachable_by_classname(classname, false);
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
      mark_reachable_by_classname(clazz, false);
    }
  }

  /* Do only keep class rules for now.
   * '*' and '**' rules are skipped,
   * because those are matching on something else,
   * which we haven't implemented yet.
   * Rules can be "*" or "**" on classname and match
   * on some other attribute. We don't match against
   * all attributes at once, so this prevents us
   * from matching everything.
   */
  std::vector<std::string> cls_patterns;
  for (auto const& r : proguard_rules) {
    if (r.classname != nullptr &&
        r.class_type == keeprules::ClassType::CLASS &&
          strlen(r.classname) > 2) {
      std::string cls_pattern(r.classname);
      std::replace(cls_pattern.begin(), cls_pattern.end(), '.', '/');
      auto prep_pat = 'L' + cls_pattern;
      TRACE(PGR, 1, "adding pattern %s \n", prep_pat.c_str());
      cls_patterns.push_back(prep_pat);
    }
  }
  size_t pg_marked_classes = 0;
  for (auto clazz : scope) {
    auto cname = clazz->get_type()->get_name()->c_str();
    auto cls_len = strlen(cname);
    for (auto const& pat : cls_patterns) {
        int pat_len = pat.size();
        if (type_matches(pat.c_str(), cname, pat_len, cls_len)) {
          mark_reachable_directly(clazz);
          TRACE(PGR, 2, "matched cls %s against pattern %s \n",
              cname, pat.c_str());
          pg_marked_classes++;
          break;
      }
    }
  }
  TRACE(PGR, 1, "matched on %lu classes with CLASS KEEP proguard rules \n",
      pg_marked_classes);
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
  for (auto clazz : scope) {
    clazz->rstate.clear_if_compute();
  }

  std::unordered_set<DexString*> maybetypes;
  walk_annotations(scope,
      [&](DexAnnotation* anno) {
        static DexType* dalviksig =
            DexType::get_type("Ldalvik/annotation/Signature;");
        // Signature annotations contain strings that Jackson uses
        // to construct the underlying types.  We capture the
        // full list here, and mark them later.  (There are many
        // duplicates, so de-duping before we look it up as a
        // class makes sense)
        if (anno->type() == dalviksig) {
          auto elems = anno->anno_elems();
          for (auto const& elem : elems) {
            auto ev = elem.encoded_value;
            if (ev->evtype() != DEVT_ARRAY) continue;
            auto arrayev = static_cast<DexEncodedValueArray*>(ev);
            auto const& evs = arrayev->evalues();
            for (auto strev : *evs) {
              if (strev->evtype() != DEVT_STRING) continue;
              auto stringev =
              static_cast<DexEncodedValueString*>(strev);
              maybetypes.insert((DexString*)stringev->string());
            }
          }
          return;
        }
        // Class literals in annotations.
        //
        // Example:
        //    @JsonDeserialize(using=MyJsonDeserializer.class)
        //                                   ^^^^
        if (anno->runtime_visible()) {
          auto elems = anno->anno_elems();
          for (auto const& dae : elems) {
            auto evalue = dae.encoded_value;
            std::vector<DexType*> ltype;
            evalue->gather_types(ltype);
            if (ltype.size()) {
              for (auto dextype : ltype) {
                mark_reachable_directly(dextype);
              }
            }
          }
        }
      });

  // Now we process the strings that were in the signature
  // annotations.
  // Note: We do not mark these as ref'd by string, because
  // these cases are handleable for renaming.
  for (auto dstring : maybetypes) {
    const char* cstr = dstring->c_str();
    int len = strlen(cstr);
    if (len < 3) continue;
    if (cstr[0] != 'L') continue;
    if (cstr[len - 1] == ';') {
      auto dtype = DexType::get_type(dstring);
      mark_reachable_directly(dtype);
      continue;
    }
    std::string buf(cstr);
    buf += ';';
    auto dtype = DexType::get_type(buf.c_str());
    mark_reachable_directly(dtype);
  }

  // Matches methods marked as native
  walk_methods(scope,
               [&](DexMethod* meth) {
                 if (meth->get_access() & DexAccessFlags::ACC_NATIVE) {
                   mark_reachable_by_classname(meth->get_class(), true);
                 }
               });

  walk_code(scope,
            [](DexMethod*) { return true; },
            [&](DexMethod* meth, DexCode* code) {
              auto opcodes = code->get_instructions();
              for (const auto& opcode : opcodes) {
                // Matches any stringref that name-aliases a type.
                if (opcode->has_strings()) {
                  auto stringop = static_cast<DexOpcodeString*>(opcode);
                  DexString* dsclzref = stringop->get_string();
                  DexType* dtexclude =
                      get_dextype_from_dotname(dsclzref->c_str());
                  if (dtexclude == nullptr) continue;
                  mark_reachable_by_classname(dtexclude, true);
                }
                if (opcode->has_types()) {
                  // Matches the following instructions (from most to least
                  // common):
                  // check-cast, new-instance, const-class, instance-of
                  // new-instance should not be on this list, and
                  // we should not allow these to operate on themselves.
                  // TODO(snay/dalves)
                  auto typeop = static_cast<DexOpcodeType*>(opcode);
                  mark_reachable_directly(typeop->get_type());
                }
              }
            });
}

void reportReachableClasses(const Scope& scope, std::string reportFileName) {
  TRACE(PGR, 4, "Total numner of classes: %d\n", scope.size());
  // First report keep annotations
  std::ofstream reportFileDNS(reportFileName + ".dns");
  for (auto const& dns : do_not_strip_classes) {
    reportFileDNS << "Do not strip class: " <<
        dns->get_type()->get_name()->c_str() << "\n";
  }
  reportFileDNS.close();
  // Report classes that the reflection filter says can't be deleted.
  std::ofstream reportFileCanDelete(reportFileName + ".cant_delete");
  for (auto const& cls : scope) {
    if (!can_delete(cls)) {
      reportFileCanDelete << cls->get_name()->c_str() << "\n";
    }
  }
  reportFileCanDelete.close();
  // Report classes that the reflection filter says can't be renamed.
  std::ofstream reportFileCanRename(reportFileName + ".cant_rename");
  for (auto const& cls : scope) {
    if (!can_rename(cls)) {
      reportFileCanRename << cls->get_name()->c_str() << "\n";
    }
  }
  reportFileCanRename.close();
  // Report classes marked for keep from ProGuard flat file list.
  std::ofstream reportFileKeep(reportFileName + ".must_keep");
  for (auto const& cls : scope) {
    if (is_seed(cls)) {
      reportFileKeep << cls->get_name()->c_str() << "\n";
    }
  }
  reportFileKeep.close();
}

void init_reachable_classes(
    const Scope& scope,
    folly::dynamic& config,
    const std::vector<KeepRule>& proguard_rules,
    const std::unordered_set<DexType*>& no_optimizations_anno) {
  // Find classes that are reachable in such a way that none of the redex
  // passes will cause them to be no longer reachable.  For example, if a
  // class is referenced from the manifest.
  init_permanently_reachable_classes(
      scope, config, proguard_rules, no_optimizations_anno);

  // Classes that are reachable in ways that could change as Redex runs. For
  // example, a class might be instantiated from a method, but if that method
  // is later deleted then it might no longer be reachable.
  recompute_classes_reachable_from_code(scope);
}

void init_seed_classes(const std::string seeds_filename) {
    TRACE(PGR, 1, "Reading seed classes from %s\n", seeds_filename.c_str());
    auto start = std::chrono::high_resolution_clock::now();
    std::ifstream seeds_file(seeds_filename);
    uint count = 0;
    if (!seeds_file) {
      TRACE(PGR, 1, "Seeds file %s was not found (ignoring error).",
          seeds_filename.c_str());
    } else {
      std::string line;
      while (getline(seeds_file, line)) {
        if (line.find(":") == std::string::npos && line.find("$") ==
            std::string::npos) {
          auto dex_type = get_dextype_from_dotname(line.c_str());
          if (dex_type != nullptr) {
            mark_reachable_by_seed(dex_type);
            count++;
          } else {
            TRACE(PGR, 1,
                "Seed file contains class for which "
                "Dex type can't be found: %s\n",
                line.c_str());
          }
        }
      }
      seeds_file.close();
    }
    auto end = std::chrono::high_resolution_clock::now();
    TRACE(PGR, 1, "Read %d seed classes in %.1lf seconds\n", count,
          std::chrono::duration<double>(end - start).count());
}

/**
 * Note: The do_not_strip() methods here effectively form a separate reachable
 * class/field/method filter. This should be combined into the main
 * can_delete() filter.
 */
bool do_not_strip(DexField* f) {
  return do_not_strip_fields.count(f) != 0;
}

bool do_not_strip(DexMethod* m) {
  return do_not_strip_methods.count(m) != 0;
}

bool do_not_strip(DexClass* c) {
  return do_not_strip_classes.count(c) != 0;
}
