/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RenameClassesV2.h"

#include <algorithm>
#include <arpa/inet.h>
#include <map>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

#include "Walkers.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "ReachableClasses.h"
#include "RedexResources.h"

#define MAX_DESCRIPTOR_LENGTH (1024)
#define MAX_IDENT_CHAR (62)
#define BASE MAX_IDENT_CHAR
#define MAX_IDENT (MAX_IDENT_CHAR * MAX_IDENT_CHAR * MAX_IDENT_CHAR)

#define METRIC_CLASSES_IN_SCOPE "num_classes_in_scope"
#define METRIC_RENAMED_CLASSES "**num_renamed**"
#define METRIC_DONT_RENAME_ANNOTATED "num_dont_rename_annotated"
#define METRIC_DONT_RENAME_ANNOTATIONS "num_dont_rename_annotations"
#define METRIC_DONT_RENAME_SPECIFIC "num_dont_rename_specific"
#define METRIC_DONT_RENAME_HIERARCHY "num_dont_rename_hierarchy"
#define METRIC_DONT_RENAME_RESOURCES "num_dont_rename_resources"
#define METRIC_DONT_RENAME_CLASS_FOR_NAME_LITERALS "num_dont_rename_class_for_name_literals"
#define METRIC_DONT_RENAME_CANARIES "num_dont_rename_canaries"
#define METRIC_DONT_RENAME_ANNOTATIONED "num_dont_rename_annotationed"
#define METRIC_DONT_RENAME_NATIVE_BINDINGS "num_dont_rename_native_bindings"

static RenameClassesPassV2 s_pass;

namespace {

void unpackage_private(Scope &scope) {
  walk_methods(scope,
      [&](DexMethod *method) {
        if (is_package_protected(method)) set_public(method);
      });
  walk_fields(scope,
      [&](DexField *field) {
        if (is_package_protected(field)) set_public(field);
      });
  for (auto clazz : scope) {
    if (!clazz->is_external()) {
      set_public(clazz);
    }
  }

  static DexType *dalvikinner =
    DexType::get_type("Ldalvik/annotation/InnerClass;");

  walk_annotations(scope, [&](DexAnnotation* anno) {
    if (anno->type() != dalvikinner) return;
    auto elems = anno->anno_elems();
    for (auto elem : elems) {
      // Fix access flags on all @InnerClass annotations
      if (!strcmp("accessFlags", elem.string->c_str())) {
        always_assert(elem.encoded_value->evtype() == DEVT_INT);
        elem.encoded_value->value((elem.encoded_value->value() & ~VISIBILITY_MASK) | ACC_PUBLIC);
        TRACE(RENAME, 3, "Fix InnerClass accessFlags %s => %08x\n", elem.string->c_str(), elem.encoded_value->value());
      }
    }
  });
}

static char getident(int num) {
  assert(num >= 0 && num < BASE);
  if (num < 10) {
    return '0' + num;
  } else if (num >= 10 && num < 36){
    return 'A' + num - 10;
  } else {
    return 'a' + num - 26 - 10;
  }
}

void get_next_ident(char *out, int num) {
  int low = num;
  int mid = (num / BASE);
  int top = (mid / BASE);
  always_assert_log(num <= MAX_IDENT,
                    "Bailing, Ident %d, greater than maximum\n", num);
  if (top) {
    *out++ = getident(top);
    low -= (top * BASE * BASE);
  }
  if (mid) {
    mid -= (top * BASE);
    *out++ = getident(mid);
    low -= (mid * BASE);
  }
  *out++ = getident(low);
  *out++ = '\0';
}

static int s_base_strings_size = 0;
static int s_ren_strings_size = 0;
static int s_sequence = 0;
static int s_padding = 0;

/**
 * Determine if the given dex item has the given annotation
 *
 * @param t The dex item whose annotations we'll examine
 * @param anno_type The annotatin we're looking for, expressed as DexType
 * @return true IFF dex item t is annotated with anno_type
 */
template<typename T>
bool has_anno(const T* t, const DexType* anno_type) {
  if (anno_type == nullptr) return false;
  if (t->get_anno_set() == nullptr) return false;
  for (const auto& anno : t->get_anno_set()->get_annotations()) {
    if (anno->type() == anno_type) {
      return true;
    }
  }
  return false;
}

// Find all the interfaces that extend 'intf'
bool gather_intf_extenders(const DexType* extender, const DexType* intf, std::unordered_set<const DexType*>& intf_extenders) {
  bool extends = false;
  const DexClass* extender_cls = type_class(extender);
  if (!extender_cls) return extends;
  if (extender_cls->get_access() & ACC_INTERFACE) {
    for (const auto& extends_intf : extender_cls->get_interfaces()->get_type_list()) {
      if (extends_intf == intf || gather_intf_extenders(extends_intf, intf, intf_extenders)) {
        intf_extenders.insert(extender);
        extends = true;
      }
    }
  }
  return extends;
}

void gather_intf_extenders(const Scope& scope, const DexType* intf, std::unordered_set<const DexType*>& intf_extenders) {
  for (const auto& cls : scope) {
    gather_intf_extenders(cls->get_type(), intf, intf_extenders);
  }
}

void get_all_implementors(const Scope& scope, const DexType* intf, std::unordered_set<const DexType*>& impls) {

  std::unordered_set<const DexType*> intf_extenders;
  gather_intf_extenders(scope, intf, intf_extenders);

  std::unordered_set<const DexType*> intfs;
  intfs.insert(intf);
  intfs.insert(intf_extenders.begin(), intf_extenders.end());

  for (auto cls : scope) {
    auto cur = cls;
    bool found = false;
    while (!found && cur != nullptr) {
      for (auto impl : cur->get_interfaces()->get_type_list()) {
        if (intfs.count(impl) > 0) {
          impls.insert(cls->get_type());
          found = true;
          break;
        }
      }
      cur = type_class(cur->get_super_class());
    }
  }
}

}

void RenameClassesPassV2::build_dont_rename_resources(PassManager& mgr, std::set<std::string>& dont_rename_resources) {
  const Json::Value& config = mgr.get_config();
  PassConfig pc(config);
  std::string apk_dir;
  pc.get("apk_dir", "", apk_dir);

  if (apk_dir.size()) {
    // Classes present in manifest
    std::string manifest = apk_dir + std::string("/AndroidManifest.xml");
    for (std::string classname : get_manifest_classes(manifest)) {
      TRACE(RENAME, 4, "manifest: %s\n", classname.c_str());
      dont_rename_resources.insert(classname);
    }

    // Classes present in XML layouts
    for (std::string classname : get_layout_classes(apk_dir)) {
      TRACE(RENAME, 4, "xml_layout: %s\n", classname.c_str());
      dont_rename_resources.insert(classname);
    }

    // Classnames present in native libraries (lib/*/*.so)
    for (std::string classname : get_native_classes(apk_dir)) {
      auto type = DexType::get_type(classname.c_str());
      if (type == nullptr) continue;
      TRACE(RENAME, 4, "native_lib: %s\n", classname.c_str());
      dont_rename_resources.insert(classname);
    }
  }
}

void RenameClassesPassV2::build_dont_rename_class_for_name_literals(
    Scope& scope, std::set<std::string>& dont_rename_class_for_name_literals) {
  // Gather Class.forName literals
  auto match = std::make_tuple(
    m::const_string(/* const-string {vX}, <any string> */),
    m::invoke_static(/* invoke-static {vX}, java.lang.Class;.forName */
      m::opcode_method(m::named<DexMethod>("forName") && m::on_class<DexMethod>("Ljava/lang/Class;"))
      && m::has_n_args(1))
  );

  walk_matching_opcodes(scope, match, [&](const DexMethod*, size_t, DexInstruction** insns){
    DexOpcodeString* const_string = (DexOpcodeString*)insns[0];
    DexOpcodeMethod* invoke_static = (DexOpcodeMethod*)insns[1];
    // Make sure that the registers agree
    if (const_string->dest() == invoke_static->src(0)) {
      std::string classname(const_string->get_string()->c_str());
      classname = "L" + classname + ";";
      std::replace(classname.begin(), classname.end(), '.', '/');
      TRACE(RENAME, 4, "Found Class.forName of: %s, marking %s reachable\n",
        const_string->get_string()->c_str(), classname.c_str());
      dont_rename_class_for_name_literals.insert(classname);
    }
  });
}

void RenameClassesPassV2::build_dont_rename_canaries(Scope& scope,std::set<std::string>& dont_rename_canaries) {
  // Gather canaries
  for(auto clazz: scope) {
    if(strstr(clazz->get_name()->c_str(), "/Canary")) {
      dont_rename_canaries.insert(std::string(clazz->get_name()->c_str()));
    }
  }
}

void RenameClassesPassV2::build_dont_rename_hierarchies(
    Scope& scope, std::unordered_map<const DexType*, std::string>& dont_rename_hierarchies) {
  for (const auto& base : m_dont_rename_hierarchies) {
    auto base_type = DexType::get_type(base.c_str());
    if (base_type != nullptr) {
      dont_rename_hierarchies[base_type] = base;
      always_assert_log(type_class(base_type), "%s has no class\n", SHOW(base_type));

      if (type_class(base_type)->get_access() & ACC_INTERFACE) {
        std::unordered_set<const DexType*> impls;
        get_all_implementors(scope, base_type, impls);
        for (auto impl = impls.begin() ; impl != impls.end() ; ++impl) {
          dont_rename_hierarchies[*impl] = base;
        }
      } else {
        TypeVector children;
        get_all_children(base_type, children);
        for ( auto child = children.begin() ; child != children.end() ; ++child ) {
          dont_rename_hierarchies[*child] = base;
        }
      }
    }
  }
}

void RenameClassesPassV2::build_dont_rename_native_bindings(
  Scope& scope,
  std::set<DexType*>& dont_rename_native_bindings) {
  // find all classes with native methods, and all types mentioned in protos of native methods
  for(auto clazz: scope) {
    for (auto meth : clazz->get_dmethods()) {
      if (meth->get_access() & ACC_NATIVE) {
        dont_rename_native_bindings.insert(clazz->get_type());
        auto proto = meth->get_proto();
        auto rtype = proto->get_rtype();
        dont_rename_native_bindings.insert(rtype);
        for (auto ptype : proto->get_args()->get_type_list()) {
          // TODO: techincally we should recurse for array types not just go one level
          if (is_array(ptype)) {
            dont_rename_native_bindings.insert(get_array_type(ptype));
          } else {
            dont_rename_native_bindings.insert(ptype);
          }
        }
      }
    }
    for (auto meth : clazz->get_vmethods()) {
      if (meth->get_access() & ACC_NATIVE) {
        dont_rename_native_bindings.insert(clazz->get_type());
        auto proto = meth->get_proto();
        auto rtype = proto->get_rtype();
        dont_rename_native_bindings.insert(rtype);
        for (auto ptype : proto->get_args()->get_type_list()) {
          // TODO: techincally we should recurse for array types not just go one level
          if (is_array(ptype)) {
            dont_rename_native_bindings.insert(get_array_type(ptype));
          } else {
            dont_rename_native_bindings.insert(ptype);
          }
        }
      }
    }
  }
}

void RenameClassesPassV2::build_dont_rename_annotated(std::set<DexType*>& dont_rename_annotated) {
  for (const auto& annotation : m_dont_rename_annotated) {
    DexType *anno = DexType::get_type(annotation.c_str());
    if (anno) {
      dont_rename_annotated.insert(anno);
    }
  }
}

void RenameClassesPassV2::rename_classes(
    Scope& scope,
    const std::string& path,
    bool rename_annotations,
    PassManager& mgr) {
  // Make everything public
  unpackage_private(scope);

  std::set<std::string> dont_rename_class_for_name_literals;
  std::set<std::string> dont_rename_canaries;
  std::set<std::string> dont_rename_resources;
  std::unordered_map<const DexType*, std::string> dont_rename_hierarchies;
  std::set<DexType*> dont_rename_native_bindings;
  std::set<DexType*> dont_rename_annotated;

  build_dont_rename_resources(mgr, dont_rename_resources);
  build_dont_rename_class_for_name_literals(scope, dont_rename_class_for_name_literals);
  build_dont_rename_canaries(scope, dont_rename_canaries);
  build_dont_rename_hierarchies(scope, dont_rename_hierarchies);
  build_dont_rename_native_bindings(scope, dont_rename_native_bindings);
  build_dont_rename_annotated(dont_rename_annotated);

  std::map<DexString*, DexString*> aliases;
  for(auto clazz: scope) {
    // Don't rename annotations
    if (!rename_annotations && is_annotation(clazz)) {
      mgr.incr_metric(METRIC_DONT_RENAME_ANNOTATIONS, 1);
      continue;
    }

    // Don't rename types annotated with anything in dont_rename_annotated
    bool annotated = false;
    for (const auto& anno : dont_rename_annotated) {
      if (has_anno(clazz, anno)) {
        mgr.incr_metric(std::string(METRIC_DONT_RENAME_ANNOTATED), 1);
        mgr.incr_metric(
          std::string(METRIC_DONT_RENAME_ANNOTATED)+"::"+std::string(anno->get_name()->c_str()),
          1);
        annotated = true;
        break;
      }
    }
    if (annotated) continue;

    const char* clsname = clazz->get_name()->c_str();

    // Don't rename anything mentioned in resources
    if (dont_rename_resources.count(clsname) > 0) {
      mgr.incr_metric(METRIC_DONT_RENAME_RESOURCES, 1);
      continue;
    }

    // Don't rename anythings in the direct name blacklist (hierarchy ignored)
    if (m_dont_rename_specific.count(clsname) > 0) {
      mgr.incr_metric(METRIC_DONT_RENAME_SPECIFIC, 1);
      continue;
    }

    if (dont_rename_class_for_name_literals.count(clsname) > 0) {
      mgr.incr_metric(METRIC_DONT_RENAME_CLASS_FOR_NAME_LITERALS, 1);
      continue;
    }

    if (dont_rename_canaries.count(clsname) > 0) {
      mgr.incr_metric(METRIC_DONT_RENAME_CANARIES, 1);
      continue;
    }

    // Don't rename things with native bindings
    if (dont_rename_native_bindings.count(clazz->get_type()) > 0) {
      mgr.incr_metric(METRIC_DONT_RENAME_NATIVE_BINDINGS, 1);
      continue;
    }

    if (dont_rename_hierarchies.count(clazz->get_type()) > 0) {
      std::string rule = dont_rename_hierarchies[clazz->get_type()];
      mgr.incr_metric(METRIC_DONT_RENAME_HIERARCHY, 1);
      mgr.incr_metric(std::string(METRIC_DONT_RENAME_HIERARCHY)+"::"+rule, 1);
      continue;
    }

    mgr.incr_metric(METRIC_RENAMED_CLASSES, 1);

    auto dtype = clazz->get_type();
    auto oldname = dtype->get_name();
    auto oldname_cstr = oldname->c_str();

    char clzname[4];
    const char* padding = "0000000000000";
    get_next_ident(clzname, s_sequence);
    // The X helps our hacked Dalvik classloader recognize that a
    // class name is the output of the redex renamer and thus will
    // never be found in the Android platform.
    char descriptor[MAX_DESCRIPTOR_LENGTH];
    always_assert((s_padding + strlen("LX/;") + 1) < MAX_DESCRIPTOR_LENGTH);
    sprintf(descriptor, "LX/%.*s%s;",
        (s_padding < (int)strlen(clzname)) ? 0 : s_padding - (int)strlen(clzname),
        padding,
        clzname);
    s_sequence++;

    auto exists  = DexString::get_string(descriptor);
    always_assert_log(!exists, "Collision on class %s (%s)", oldname_cstr, descriptor);

    auto dstring = DexString::make_string(descriptor);
    aliases[oldname] = dstring;
    dtype->assign_name_alias(dstring);
    std::string old_str(oldname->c_str());
    std::string new_str(descriptor);
//    proguard_map.update_class_mapping(old_str, new_str);
    s_base_strings_size += strlen(oldname->c_str());
    s_ren_strings_size += strlen(dstring->c_str());

    TRACE(RENAME, 2, "'%s' ->  %s'\n", oldname->c_str(), descriptor);
    while (1) {
     std::string arrayop("[");
      arrayop += oldname->c_str();
      oldname = DexString::get_string(arrayop.c_str());
      if (oldname == nullptr) {
        break;
      }
      auto arraytype = DexType::get_type(oldname);
      if (arraytype == nullptr) {
        break;
      }
      std::string newarraytype("[");
      newarraytype += dstring->c_str();
      dstring = DexString::make_string(newarraytype.c_str());

      aliases[oldname] = dstring;
      arraytype->assign_name_alias(dstring);
    }
  }

  /* Now we need to re-write the Signature annotations.  They use
   * Strings rather than Type's, so they have to be explicitly
   * handled.
   */

  /* Generics of the form Type<> turn into the Type string
   * sans the ';'.  So, we have to alias those if they
   * exist.  Signature annotations suck.
   */
  for (auto apair : aliases) {
    char buf[MAX_DESCRIPTOR_LENGTH];
    const char *sourcestr = apair.first->c_str();
    size_t sourcelen = strlen(sourcestr);
    if (sourcestr[sourcelen - 1] != ';') continue;
    strcpy(buf, sourcestr);
    buf[sourcelen - 1] = '\0';
    auto dstring = DexString::get_string(buf);
    if (dstring == nullptr) continue;
    strcpy(buf, apair.second->c_str());
    buf[strlen(apair.second->c_str()) - 1] = '\0';
    auto target = DexString::make_string(buf);
    aliases[dstring] = target;
  }
  static DexType *dalviksig =
    DexType::get_type("Ldalvik/annotation/Signature;");
  walk_annotations(scope, [&](DexAnnotation* anno) {
    if (anno->type() != dalviksig) return;
    auto elems = anno->anno_elems();
    for (auto elem : elems) {
      auto ev = elem.encoded_value;
      if (ev->evtype() != DEVT_ARRAY) continue;
      auto arrayev = static_cast<DexEncodedValueArray*>(ev);
      auto const& evs = arrayev->evalues();
      for (auto strev : *evs) {
        if (strev->evtype() != DEVT_STRING) continue;
        auto stringev = static_cast<DexEncodedValueString*>(strev);
        if (aliases.count(stringev->string())) {
          TRACE(RENAME, 5, "Rewriting Signature from '%s' to '%s'\n",
              stringev->string()->c_str(),
              aliases[stringev->string()]->c_str());
          stringev->string(aliases[stringev->string()]);
        }
      }
    }
  });

  if (!path.empty()) {
    FILE* fd = fopen(path.c_str(), "w");
    always_assert_log(fd, "Error writing rename file");
    for (const auto &it : aliases) {
      // record for later processing and back map generation
      fprintf(fd, "%s -> %s\n",it.first->c_str(),
      it.second->c_str());
    }
    fclose(fd);
  }

  for (auto clazz : scope) {
    clazz->get_vmethods().sort(compare_dexmethods);
    clazz->get_dmethods().sort(compare_dexmethods);
    clazz->get_sfields().sort(compare_dexfields);
    clazz->get_ifields().sort(compare_dexfields);
  }
}

void RenameClassesPassV2::run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  auto scope = build_class_scope(stores);
  int total_classes = scope.size();

  s_base_strings_size = 0;
  s_ren_strings_size = 0;
  s_sequence = 0;
  // encode the whole sequence as base 62, [0 - 9 + a - z + A - Z]
  s_padding = std::ceil(std::log(total_classes) / std::log(BASE));

  rename_classes(scope, m_path, m_rename_annotations, mgr);

  mgr.incr_metric(METRIC_CLASSES_IN_SCOPE, total_classes);

  TRACE(RENAME, 1, "Total classes in scope for renaming: %d chosen padding: %d\n", total_classes, s_padding);
  TRACE(RENAME, 1, "String savings, at least %d-%d = %d bytes \n",
      s_base_strings_size, s_ren_strings_size, s_base_strings_size - s_ren_strings_size);
}
