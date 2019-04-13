/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RenameClassesV2.h"

#include <algorithm>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/regex.hpp>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DexClass.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "ReachableClasses.h"
#include "RedexResources.h"
#include "Walkers.h"
#include "Warning.h"

#include <locator.h>
using facebook::Locator;

#define MAX_DESCRIPTOR_LENGTH (1024)

static const char* METRIC_CLASSES_IN_SCOPE = "num_classes_in_scope";
static const char* METRIC_RENAMED_CLASSES = "**num_renamed**";
static const char* METRIC_FORCE_RENAMED_CLASSES = "num_force_renamed";
static const char* METRIC_REWRITTEN_CONST_STRINGS =
    "num_rewritten_const_strings";
static const char* METRIC_MISSING_HIERARCHY_TYPES =
    "num_missing_hierarchy_types";
static const char* METRIC_MISSING_HIERARCHY_CLASSES =
    "num_missing_hierarchy_classes";

static RenameClassesPassV2 s_pass;

namespace {

const char* dont_rename_reason_to_metric(DontRenameReasonCode reason) {
  switch (reason) {
    case DontRenameReasonCode::Annotated:
      return "num_dont_rename_annotated";
    case DontRenameReasonCode::Annotations:
      return "num_dont_rename_annotations";
    case DontRenameReasonCode::Specific:
      return "num_dont_rename_specific";
    case DontRenameReasonCode::Packages:
      return "num_dont_rename_packages";
    case DontRenameReasonCode::Hierarchy:
      return "num_dont_rename_hierarchy";
    case DontRenameReasonCode::Resources:
      return "num_dont_rename_resources";
    case DontRenameReasonCode::ClassNameLiterals:
      return "num_dont_rename_class_name_literals";
    case DontRenameReasonCode::Canaries:
      return "num_dont_rename_canaries";
    case DontRenameReasonCode::NativeBindings:
      return "num_dont_rename_native_bindings";
    case DontRenameReasonCode::ClassForTypesWithReflection:
      return "num_dont_rename_class_for_types_with_reflection";
    case DontRenameReasonCode::ProguardCantRename:
      return "num_dont_rename_pg_cant_rename";
    case DontRenameReasonCode::SerdeRelationships:
      return "num_dont_rename_serde_relationships";
    default:
      always_assert_log(false, "Unexpected DontRenameReasonCode: %d", reason);
  }
}

bool dont_rename_reason_to_metric_per_rule(DontRenameReasonCode reason) {
  switch (reason) {
    case DontRenameReasonCode::Annotated:
    case DontRenameReasonCode::Packages:
    case DontRenameReasonCode::Hierarchy:
      // Set to true to add more detailed metrics for renamer if needed
      return false;
    default:
      return false;
  }
}

void unpackage_private(Scope &scope) {
  walk::methods(scope,
      [&](DexMethod *method) {
        if (is_package_protected(method)) set_public(method);
      });
  walk::fields(scope,
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

  walk::annotations(scope, [&](DexAnnotation* anno) {
    if (anno->type() != dalvikinner) return;
    auto elems = anno->anno_elems();
    for (auto elem : elems) {
      // Fix access flags on all @InnerClass annotations
      if (!strcmp("accessFlags", elem.string->c_str())) {
        always_assert(elem.encoded_value->evtype() == DEVT_INT);
        elem.encoded_value->value(
            (elem.encoded_value->value() & ~VISIBILITY_MASK) | ACC_PUBLIC);
        TRACE(RENAME, 3, "Fix InnerClass accessFlags %s => %08x\n",
            elem.string->c_str(), elem.encoded_value->value());
      }
    }
  });
}

} // namespace

// Returns idx of the vector of packages if the given class name matches, or -1
// if not found.
ssize_t find_matching_package(
    const std::string& classname,
    const std::vector<std::string>& allowed_packages) {
  for (size_t i = 0; i < allowed_packages.size(); i++) {
    if (classname.rfind("L" + allowed_packages[i]) == 0) {
      return i;
    }
  }
  return -1;
}

bool referenced_by_layouts(const DexClass* clazz) {
  return clazz->rstate.is_referenced_by_resource_xml();
}

// Returns true if this class is a layout, and allowed for renaming via config.
bool is_allowed_layout_class(
  const DexClass* clazz,
  const std::vector<std::string>& allow_layout_rename_packages) {
  always_assert(referenced_by_layouts(clazz));
  auto idx = find_matching_package(clazz->get_name()->str(),
                                   allow_layout_rename_packages);
  return idx != -1;
}

std::unordered_set<std::string>
RenameClassesPassV2::build_dont_rename_resources(
    PassManager& mgr,
    std::unordered_map<const DexType*, std::string>& force_rename_classes) {
  std::unordered_set<std::string> dont_rename_resources;
  if (m_apk_dir.size()) {
    // Classnames present in native libraries (lib/*/*.so)
    for (std::string classname : get_native_classes(m_apk_dir)) {
      auto type = DexType::get_type(classname);
      if (type == nullptr) continue;
      TRACE(RENAME, 4, "native_lib: %s\n", classname.c_str());
      dont_rename_resources.insert(classname);
    }
  }
  return dont_rename_resources;
}

std::unordered_set<std::string>
RenameClassesPassV2::build_dont_rename_class_name_literals(Scope& scope) {
  using namespace boost::algorithm;
  std::vector<DexString*> all_strings;
  for (auto clazz : scope) {
    clazz->gather_strings(all_strings);
  }
  sort_unique(all_strings);
  std::unordered_set<std::string> result;
  boost::regex external_name_regex{
      "((org)|(com)|(android(x|\\.support)))\\."
      "([a-zA-Z][a-zA-Z\\d_$]*\\.)*"
      "[a-zA-Z][a-zA-Z\\d_$]*"};
  for (auto dex_str : all_strings) {
    const std::string& s = dex_str->str();
    if (!ends_with(s, ".java") && boost::regex_match(s, external_name_regex)) {
      const std::string& internal_name = JavaNameUtil::external_to_internal(s);
      auto cls = type_class(DexType::get_type(internal_name));
      if (cls != nullptr && !cls->is_external()) {
        result.insert(internal_name);
        TRACE(RENAME, 4, "Found %s in string pool before renaming\n",
              s.c_str());
      }
    }
  }
  return result;
}

std::unordered_set<std::string>
RenameClassesPassV2::build_dont_rename_for_types_with_reflection(
    Scope& scope, const ProguardMap& pg_map) {
  std::unordered_set<std::string> dont_rename_class_for_types_with_reflection;
  std::unordered_set<DexType*> refl_map;
  for (auto const& refl_type_str : m_dont_rename_types_with_reflection) {
    auto deobf_cls_string = pg_map.translate_class(refl_type_str);
    TRACE(RENAME,
          4,
          "%s got translated to %s\n",
          refl_type_str.c_str(),
          deobf_cls_string.c_str());
    if (deobf_cls_string == "") {
      deobf_cls_string = refl_type_str;
    }
    DexType* type_with_refl = DexType::get_type(deobf_cls_string.c_str());
    if (type_with_refl != nullptr) {
      TRACE(RENAME, 4, "got DexType %s\n", SHOW(type_with_refl));
      refl_map.insert(type_with_refl);
    }
  }

  walk::opcodes(scope,
      [](DexMethod*) { return true; },
      [&](DexMethod* m, IRInstruction* insn) {
        if (insn->has_method()) {
          auto callee = insn->get_method();
          if (callee == nullptr || !callee->is_concrete()) return;
          auto callee_method_cls = callee->get_class();
          if (refl_map.count(callee_method_cls) == 0) return;
          std::string classname = m->get_class()->get_name()->str();
          TRACE(RENAME, 4,
            "Found %s with known reflection usage. marking reachable\n",
            classname.c_str());
          dont_rename_class_for_types_with_reflection.insert(classname);
        }
  });
  return dont_rename_class_for_types_with_reflection;
}

std::unordered_set<std::string> RenameClassesPassV2::build_dont_rename_canaries(
    Scope& scope) {
  std::unordered_set<std::string> dont_rename_canaries;
  // Gather canaries
  for (auto clazz : scope) {
    if (strstr(clazz->get_name()->c_str(), "/Canary")) {
      dont_rename_canaries.insert(clazz->get_name()->str());
    }
  }
  return dont_rename_canaries;
}

std::unordered_map<const DexType*, std::string>
RenameClassesPassV2::build_force_rename_hierarchies(
    PassManager& mgr, Scope& scope, const ClassHierarchy& class_hierarchy) {
  std::unordered_map<const DexType*, std::string> force_rename_hierarchies;
  std::vector<DexClass*> base_classes;
  for (const auto& base : m_force_rename_hierarchies) {
    // skip comments
    if (base.c_str()[0] == '#') continue;
    auto base_type = DexType::get_type(base.c_str());
    if (base_type != nullptr) {
      DexClass* base_class = type_class(base_type);
      if (!base_class) {
        TRACE(RENAME, 2, "Can't find class for force_rename_hierachy rule %s\n",
              base.c_str());
        mgr.incr_metric(METRIC_MISSING_HIERARCHY_CLASSES, 1);
      } else {
        base_classes.emplace_back(base_class);
      }
    } else {
      TRACE(RENAME, 2, "Can't find type for force_rename_hierachy rule %s\n",
            base.c_str());
      mgr.incr_metric(METRIC_MISSING_HIERARCHY_TYPES, 1);
    }
  }
  for (const auto& base_class : base_classes) {
    auto base_name = base_class->get_name()->c_str();
    force_rename_hierarchies[base_class->get_type()] = base_name;
    TypeSet children_and_implementors;
    get_all_children_or_implementors(
        class_hierarchy, scope, base_class, children_and_implementors);
    for (const auto& cls : children_and_implementors) {
      force_rename_hierarchies[cls] = base_name;
    }
  }
  return force_rename_hierarchies;
}

std::unordered_map<const DexType*, std::string>
RenameClassesPassV2::build_dont_rename_hierarchies(
    PassManager& mgr, Scope& scope, const ClassHierarchy& class_hierarchy) {
  std::unordered_map<const DexType*, std::string> dont_rename_hierarchies;
  std::vector<DexClass*> base_classes;
  for (const auto& base : m_dont_rename_hierarchies) {
    // skip comments
    if (base.c_str()[0] == '#') continue;
    auto base_type = DexType::get_type(base.c_str());
    if (base_type != nullptr) {
      DexClass* base_class = type_class(base_type);
      if (!base_class) {
        TRACE(RENAME, 2, "Can't find class for dont_rename_hierachy rule %s\n",
              base.c_str());
        mgr.incr_metric(METRIC_MISSING_HIERARCHY_CLASSES, 1);
      } else {
        base_classes.emplace_back(base_class);
      }
    } else {
      TRACE(RENAME, 2, "Can't find type for dont_rename_hierachy rule %s\n",
            base.c_str());
      mgr.incr_metric(METRIC_MISSING_HIERARCHY_TYPES, 1);
    }
  }
  for (const auto& base_class : base_classes) {
    auto base_name = base_class->get_name()->c_str();
    dont_rename_hierarchies[base_class->get_type()] = base_name;
    TypeSet children_and_implementors;
    get_all_children_or_implementors(
        class_hierarchy, scope, base_class, children_and_implementors);
    for (const auto& cls : children_and_implementors) {
      dont_rename_hierarchies[cls] = base_name;
    }
  }
  return dont_rename_hierarchies;
}

std::unordered_set<const DexType*>
RenameClassesPassV2::build_dont_rename_serde_relationships(Scope& scope) {
  std::unordered_set<const DexType*> dont_rename_serde_relationships;
  for (const auto& cls : scope) {
    ClassSerdes cls_serdes = get_class_serdes(cls);
    const char* rawname = cls->get_name()->c_str();
    std::string name = std::string(rawname);
    name.pop_back();

    // Look for a class that matches one of the two deserializer patterns
    DexType* deser = cls_serdes.get_deser();
    DexType* flatbuf_deser = cls_serdes.get_flatbuf_deser();
    bool has_deser_finder = false;

    if (deser || flatbuf_deser) {
      for (const auto& method : cls->get_dmethods()) {
        if (!strcmp("$$getDeserializerClass", method->get_name()->c_str())) {
          has_deser_finder = true;
          break;
        }
      }
    }

    // Look for a class that matches one of the two serializer patterns
    DexType* ser = cls_serdes.get_ser();
    DexType* flatbuf_ser = cls_serdes.get_flatbuf_ser();
    bool has_ser_finder = false;

    if (ser || flatbuf_ser) {
      for (const auto& method : cls->get_dmethods()) {
        if (!strcmp("$$getSerializerClass", method->get_name()->c_str())) {
          has_ser_finder = true;
          break;
        }
      }
    }

    bool dont_rename =
      ((deser || flatbuf_deser) && !has_deser_finder) ||
      ((ser || flatbuf_ser) && !has_ser_finder);

    if (dont_rename) {
      dont_rename_serde_relationships.insert(cls->get_type());
      if (deser) dont_rename_serde_relationships.insert(deser);
      if (flatbuf_deser) dont_rename_serde_relationships.insert(flatbuf_deser);
      if (ser) dont_rename_serde_relationships.insert(ser);
      if (flatbuf_ser) dont_rename_serde_relationships.insert(flatbuf_ser);
    }
  }

  return dont_rename_serde_relationships;
}

std::unordered_set<const DexType*>
RenameClassesPassV2::build_dont_rename_native_bindings(Scope& scope) {
  std::unordered_set<const DexType*> dont_rename_native_bindings;
  // find all classes with native methods, and all types mentioned
  // in protos of native methods
  for(auto clazz: scope) {
    for (auto meth : clazz->get_dmethods()) {
      if (is_native(meth)) {
        dont_rename_native_bindings.insert(clazz->get_type());
        auto proto = meth->get_proto();
        auto rtype = proto->get_rtype();
        dont_rename_native_bindings.insert(rtype);
        for (auto ptype : proto->get_args()->get_type_list()) {
          // TODO: techincally we should recurse for array types
          // not just go one level
          if (is_array(ptype)) {
            dont_rename_native_bindings.insert(get_array_type(ptype));
          } else {
            dont_rename_native_bindings.insert(ptype);
          }
        }
      }
    }
    for (auto meth : clazz->get_vmethods()) {
      if (is_native(meth)) {
        dont_rename_native_bindings.insert(clazz->get_type());
        auto proto = meth->get_proto();
        auto rtype = proto->get_rtype();
        dont_rename_native_bindings.insert(rtype);
        for (auto ptype : proto->get_args()->get_type_list()) {
          // TODO: techincally we should recurse for array types
          // not just go one level
          if (is_array(ptype)) {
            dont_rename_native_bindings.insert(get_array_type(ptype));
          } else {
            dont_rename_native_bindings.insert(ptype);
          }
        }
      }
    }
  }
  return dont_rename_native_bindings;
}

std::unordered_set<const DexType*>
RenameClassesPassV2::build_dont_rename_annotated() {
  std::unordered_set<const DexType*> dont_rename_annotated;
  for (const auto& annotation : m_dont_rename_annotated) {
    DexType* anno = DexType::get_type(annotation);
    if (anno) {
      dont_rename_annotated.insert(anno);
    }
  }
  return dont_rename_annotated;
}

class AliasMap {
  std::map<DexString*, DexString*, dexstrings_comparator> m_class_name_map;
  std::map<DexString*, DexString*, dexstrings_comparator> m_extras_map;
 public:
  void add_class_alias(DexClass* cls, DexString* alias) {
    m_class_name_map.emplace(cls->get_name(), alias);
  }
  void add_alias(DexString* original, DexString* alias) {
    m_extras_map.emplace(original, alias);
  }
  bool has(DexString* key) const {
    return m_class_name_map.count(key) || m_extras_map.count(key);
  }
  DexString* at(DexString* key) const {
    auto it = m_class_name_map.find(key);
    if (it != m_class_name_map.end()) {
      return it->second;
    }
    return m_extras_map.at(key);
  }
  const std::map<DexString*, DexString*, dexstrings_comparator>& get_class_map()
      const {
    return m_class_name_map;
  }
};

static void sanity_check(const Scope& scope, const AliasMap& aliases) {
  std::unordered_set<std::string> external_names;
  // Class.forName() expects strings of the form "foo.bar.Baz". We should be
  // very suspicious if we see these strings in the string pool that
  // correspond to the old name of a class that we have renamed...
  for (const auto& it : aliases.get_class_map()) {
    external_names.emplace(
        JavaNameUtil::internal_to_external(it.first->c_str()));
  }
  std::vector<DexString*> all_strings;
  for (auto clazz : scope) {
    clazz->gather_strings(all_strings);
  }
  sort_unique(all_strings);
  int sketchy_strings = 0;
  for (auto s : all_strings) {
    if (external_names.find(s->str()) != external_names.end() ||
        aliases.has(s)) {
      TRACE(RENAME, 2, "Found %s in string pool after renaming\n", s->c_str());
      sketchy_strings++;
    }
  }
  if (sketchy_strings > 0) {
    fprintf(stderr,
            "WARNING: Found a number of sketchy class-like strings after class "
            "renaming. Re-run with TRACE=RENAME:2 for more details.\n");
  }
}

/* In Signature annotations, parameterized types of the form Foo<Bar> get
 * represented as the strings
 *   "Lcom/baz/Foo" "<" "Lcom/baz/Bar;" ">"
 *   or
 *   "Lcom/baz/Foo<" "Lcom/baz/Bar;" ">"
 *
 * Note that "Lcom/baz/Foo" lacks a trailing semicolon.
 * Signature annotations suck.
 *
 * This method transforms the input to the form expected by the alias map:
 *   "Lcom/baz/Foo;"
 * looks that up in the map, then transforms back to the form of the input.
 */
DexString* lookup_signature_annotation(const AliasMap& aliases,
                                       DexString* anno) {
  bool has_bracket = false;
  bool added_semicolon = false;
  std::string anno_str = anno->str();
  // anno_str looks like one of these
  // Lcom/baz/Foo<
  // Lcom/baz/Foo;
  // Lcom/baz/Foo
  if (anno_str.back() == '<') {
    anno_str.pop_back();
    has_bracket = true;
  }
  // anno_str looks like one of these
  // Lcom/baz/Foo;
  // Lcom/baz/Foo
  if (anno_str.back() != ';') {
    anno_str.push_back(';');
    added_semicolon = true;
  }
  // anno_str looks like this
  // Lcom/baz/Foo;

  // Use get_string because if it's in the map, then it must also already exist
  DexString* transformed_anno = DexString::get_string(anno_str);
  if (transformed_anno != nullptr && aliases.has(transformed_anno)) {
    DexString* obfu = aliases.at(transformed_anno);
    if (!added_semicolon && !has_bracket) {
      return obfu;
    }
    std::string obfu_str = obfu->str();
    // We need to transform back to the original format of the input
    if (added_semicolon) {
      always_assert(obfu_str.back() == ';');
      obfu_str.pop_back();
    }
    if (has_bracket) {
      always_assert(obfu_str.back() != '<');
      obfu_str.push_back('<');
    }
    return DexString::make_string(obfu_str);
  }
  return nullptr;
}

void RenameClassesPassV2::eval_classes(Scope& scope,
                                       const ClassHierarchy& class_hierarchy,
                                       ConfigFiles& conf,
                                       bool rename_annotations,
                                       PassManager& mgr) {
  auto force_rename_hierarchies =
      build_force_rename_hierarchies(mgr, scope, class_hierarchy);

  auto dont_rename_serde_relationships = build_dont_rename_serde_relationships(scope);
  auto dont_rename_resources =
    build_dont_rename_resources(mgr, force_rename_hierarchies);
  auto dont_rename_class_name_literals = build_dont_rename_class_name_literals(scope);
  auto dont_rename_class_for_types_with_reflection =
      build_dont_rename_for_types_with_reflection(scope,
                                                  conf.get_proguard_map());
  auto dont_rename_canaries = build_dont_rename_canaries(scope);
  auto dont_rename_hierarchies =
      build_dont_rename_hierarchies(mgr, scope, class_hierarchy);
  auto dont_rename_native_bindings = build_dont_rename_native_bindings(scope);
  auto dont_rename_annotated = build_dont_rename_annotated();

  std::string norule = "";

  for(auto clazz: scope) {
    // Short circuit force renames
    if (force_rename_hierarchies.count(clazz->get_type())) {
      m_force_rename_classes.insert(clazz);
      continue;
    }

    // Don't rename annotations
    if (!rename_annotations && is_annotation(clazz)) {
      m_dont_rename_reasons[clazz] =
          { DontRenameReasonCode::Annotations, norule };
      continue;
    }

    // Don't rename types annotated with anything in dont_rename_annotated
    bool annotated = false;
    for (const auto& anno : dont_rename_annotated) {
      if (has_anno(clazz, anno)) {
        m_dont_rename_reasons[clazz] =
            { DontRenameReasonCode::Annotated, anno->str() };
        annotated = true;
        break;
      }
    }
    if (annotated) continue;

    const char* clsname = clazz->get_name()->c_str();
    std::string strname = std::string(clsname);

    // Don't rename anything mentioned in resources. Two variants of checks here
    // to cover both configuration options (either we're relying on aapt to
    // compute resource reachability, or we're doing it ourselves).
    if (dont_rename_resources.count(clsname) ||
          (referenced_by_layouts(clazz)
            && !is_allowed_layout_class(clazz, m_allow_layout_rename_packages))) {
      m_dont_rename_reasons[clazz] =
          { DontRenameReasonCode::Resources, norule };
      continue;
    }

    // Don't rename anythings in the direct name blacklist (hierarchy ignored)
    if (m_dont_rename_specific.count(clsname)) {
      m_dont_rename_reasons[clazz] =
          { DontRenameReasonCode::Specific, strname };
      continue;
    }

    // Don't rename anything if it falls in a blacklisted package
    bool package_blacklisted = false;
    for (const auto& pkg : m_dont_rename_packages) {
      if (strname.rfind("L"+pkg) == 0) {
        TRACE(RENAME, 2, "%s blacklisted by pkg rule %s\n",
            clsname, pkg.c_str());
        m_dont_rename_reasons[clazz] = { DontRenameReasonCode::Packages, pkg };
        package_blacklisted = true;
        break;
      }
    }
    if (package_blacklisted) continue;

    if (dont_rename_class_name_literals.count(clsname)) {
      m_dont_rename_reasons[clazz] =
          { DontRenameReasonCode::ClassNameLiterals, norule };
      continue;
    }

    if (dont_rename_class_for_types_with_reflection.count(clsname)) {
      m_dont_rename_reasons[clazz] =
          { DontRenameReasonCode::ClassForTypesWithReflection, norule };
      continue;
    }

    if (dont_rename_canaries.count(clsname)) {
      m_dont_rename_reasons[clazz] = { DontRenameReasonCode::Canaries, norule };
      continue;
    }

    if (dont_rename_native_bindings.count(clazz->get_type())) {
      m_dont_rename_reasons[clazz] =
          { DontRenameReasonCode::NativeBindings, norule };
      continue;
    }

    if (dont_rename_hierarchies.count(clazz->get_type())) {
      std::string rule = dont_rename_hierarchies[clazz->get_type()];
      m_dont_rename_reasons[clazz] = { DontRenameReasonCode::Hierarchy, rule };
      continue;
    }

    if (dont_rename_serde_relationships.count(clazz->get_type())) {
      m_dont_rename_reasons[clazz] =
          { DontRenameReasonCode::SerdeRelationships, norule };
      continue;
    }

    if (!can_rename_if_ignoring_blanket_keepnames(clazz)) {
      m_dont_rename_reasons[clazz] =
          { DontRenameReasonCode::ProguardCantRename, norule };
      continue;
    }
  }
}

/**
 * We re-evaluate a number of config rules again at pass running time.
 * The reason is that the types specified in those rules can be created in
 * previous Redex passes and did not exist when the initial evaluation happened.
 */
void RenameClassesPassV2::eval_classes_post(
    Scope& scope,
    const ClassHierarchy& class_hierarchy,
    PassManager& mgr) {
  auto dont_rename_hierarchies =
      build_dont_rename_hierarchies(mgr, scope, class_hierarchy);
  std::string norule = "";

  for (auto clazz : scope) {
    if (m_dont_rename_reasons.find(clazz) != m_dont_rename_reasons.end()) {
      continue;
    }

    const char* clsname = clazz->get_name()->c_str();
    std::string strname = std::string(clsname);

    // Don't rename anythings in the direct name blacklist (hierarchy ignored)
    if (m_dont_rename_specific.count(clsname)) {
      m_dont_rename_reasons[clazz] = {DontRenameReasonCode::Specific, strname};
      continue;
    }

    // Don't rename anything if it falls in a blacklisted package
    bool package_blacklisted = false;
    for (const auto& pkg : m_dont_rename_packages) {
      if (strname.rfind("L" + pkg) == 0) {
        TRACE(
            RENAME, 2, "%s blacklisted by pkg rule %s\n", clsname, pkg.c_str());
        m_dont_rename_reasons[clazz] = {DontRenameReasonCode::Packages, pkg};
        package_blacklisted = true;
        break;
      }
    }
    if (package_blacklisted) continue;

    if (dont_rename_hierarchies.count(clazz->get_type())) {
      std::string rule = dont_rename_hierarchies[clazz->get_type()];
      m_dont_rename_reasons[clazz] = {DontRenameReasonCode::Hierarchy, rule};
      continue;
    }

    // Don't rename anything if something changed and the class cannot be
    // renamed anymore.
    if (!can_rename_if_ignoring_blanket_keepnames(clazz)) {
      m_dont_rename_reasons[clazz] =
        { DontRenameReasonCode::ProguardCantRename, norule };
    }
  }
}

void RenameClassesPassV2::eval_pass(DexStoresVector& stores,
                                    ConfigFiles& conf,
                                    PassManager& mgr) {
  auto json = conf.get_json_config();
  json.get("apk_dir", "", m_apk_dir);
  TRACE(RENAME, 3, "APK Dir: %s\n", m_apk_dir.c_str());
  auto scope = build_class_scope(stores);
  ClassHierarchy class_hierarchy = build_type_hierarchy(scope);
  eval_classes(scope, class_hierarchy, conf, m_rename_annotations, mgr);
}

void RenameClassesPassV2::rename_classes(Scope& scope,
                                         ConfigFiles& conf,
                                         bool rename_annotations,
                                         PassManager& mgr) {
  // Make everything public
  unpackage_private(scope);

  AliasMap aliases;
  uint32_t sequence = 0;
  for (auto clazz : scope) {
    auto dtype = clazz->get_type();
    auto oldname = dtype->get_name();

    if (m_force_rename_classes.count(clazz)) {
      mgr.incr_metric(METRIC_FORCE_RENAMED_CLASSES, 1);
      TRACE(RENAME, 2, "Forced renamed: '%s'\n", oldname->c_str());
    } else if (m_dont_rename_reasons.find(clazz) !=
               m_dont_rename_reasons.end()) {
      auto reason = m_dont_rename_reasons[clazz];
      std::string metric = dont_rename_reason_to_metric(reason.code);
      mgr.incr_metric(metric, 1);
      if (dont_rename_reason_to_metric_per_rule(reason.code)) {
        std::string str = metric + "::" + std::string(reason.rule);
        mgr.incr_metric(str, 1);
        TRACE(RENAME, 2, "'%s' NOT RENAMED due to %s'\n", oldname->c_str(),
              str.c_str());
      } else {
        TRACE(RENAME, 2, "'%s' NOT RENAMED due to %s'\n", oldname->c_str(),
              metric.c_str());
      }
      sequence++;
      continue;
    }

    mgr.incr_metric(METRIC_RENAMED_CLASSES, 1);

    char descriptor[Locator::encoded_global_class_index_max];
    always_assert(sequence != Locator::invalid_global_class_index);
    Locator::encodeGlobalClassIndex(sequence, m_digits, descriptor);
    always_assert_log(facebook::Locator::decodeGlobalClassIndex(descriptor) ==
                          sequence,
                      "global class index didn't roundtrip; %s generated from "
                      "%u parsed to %u",
                      descriptor, sequence,
                      facebook::Locator::decodeGlobalClassIndex(descriptor));

    TRACE(RENAME, 2, "'%s' ->  %s (%u)'\n", oldname->c_str(), descriptor,
          sequence);
    sequence++;

    auto exists = DexString::get_string(descriptor);
    always_assert_log(!exists, "Collision on class %s (%s)", oldname->c_str(),
                      descriptor);

    auto dstring = DexString::make_string(descriptor);
    aliases.add_class_alias(clazz, dstring);
    dtype->set_name(dstring);
    std::string old_str(oldname->c_str());
    std::string new_str(descriptor);
    //    proguard_map.update_class_mapping(old_str, new_str);
    m_base_strings_size += strlen(oldname->c_str());
    m_ren_strings_size += strlen(dstring->c_str());

    while (1) {
      std::string arrayop("[");
      arrayop += oldname->c_str();
      oldname = DexString::get_string(arrayop);
      if (oldname == nullptr) {
        break;
      }
      auto arraytype = DexType::get_type(oldname);
      if (arraytype == nullptr) {
        break;
      }
      std::string newarraytype("[");
      newarraytype += dstring->c_str();
      dstring = DexString::make_string(newarraytype);

      aliases.add_alias(oldname, dstring);
      arraytype->set_name(dstring);
    }
  }

  /* Now rewrite all const-string strings for force renamed classes. */
  auto match = std::make_tuple(m::const_string());

  walk::matching_opcodes(
      scope, match,
      [&](const DexMethod*, const std::vector<IRInstruction*>& insns) {
        IRInstruction* insn = insns[0];
        DexString* str = insn->get_string();
        // get_string instead of make_string here because if the string doesn't
        // already exist, then there's no way it can match a class
        // that was renamed
        DexString* internal_str = DexString::get_string(
            JavaNameUtil::external_to_internal(str->c_str()).c_str());
        // Look up both str and intternal_str in the map; maybe str was
        // internal to begin with?
        DexString* alias_from = nullptr;
        DexString* alias_to = nullptr;
        if (aliases.has(internal_str)) {
          alias_from = internal_str;
          alias_to = aliases.at(internal_str);
          // Since we matched on external form, we need to map internal alias
          // back.
          // make_string here because the external form of the name may not be
          // present in the string table
          alias_to = DexString::make_string(
              JavaNameUtil::internal_to_external(alias_to->str()));
        } else if (aliases.has(str)) {
          alias_from = str;
          alias_to = aliases.at(str);
        }
        if (alias_to) {
          DexType* alias_from_type = DexType::get_type(alias_from);
          DexClass* alias_from_cls = type_class(alias_from_type);
          if (m_force_rename_classes.count(alias_from_cls)) {
            mgr.incr_metric(METRIC_REWRITTEN_CONST_STRINGS, 1);
            insn->set_string(alias_to);
            TRACE(RENAME, 3, "Rewrote const-string \"%s\" to \"%s\"\n",
                str->c_str(), alias_to->c_str());
          }
        }
      });

  /* Now we need to re-write the Signature annotations.  They use
   * Strings rather than Type's, so they have to be explicitly
   * handled.
   */
  static DexType *dalviksig =
    DexType::get_type("Ldalvik/annotation/Signature;");
  walk::annotations(scope, [&](DexAnnotation* anno) {
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
        DexString* old_str = stringev->string();
        DexString* new_str = lookup_signature_annotation(aliases, old_str);
        if (new_str != nullptr) {
          TRACE(RENAME, 5, "Rewriting Signature from '%s' to '%s'\n",
                old_str->c_str(), new_str->c_str());
          stringev->string(new_str);
        }
      }
    }
  });

  rename_classes_in_layouts(aliases, mgr);

  sanity_check(scope, aliases);
}

void RenameClassesPassV2::rename_classes_in_layouts(
  const AliasMap& aliases,
  PassManager& mgr) {
  // Sync up ResStringPool entries in XML layouts. Class names should appear in
  // their "external" name, i.e. java.lang.String instead of Ljava/lang/String;
  std::map<std::string, std::string> aliases_for_layouts;
  for (const auto& apair : aliases.get_class_map()) {
    aliases_for_layouts.emplace(
      JavaNameUtil::internal_to_external(apair.first->str()),
      JavaNameUtil::internal_to_external(apair.second->str()));
  }
  ssize_t layout_bytes_delta = 0;
  size_t num_layout_renamed = 0;
  auto xml_files = get_xml_files(m_apk_dir + "/res");
  for (const auto& path : xml_files) {
    if (is_raw_resource(path)) {
      continue;
    }
    size_t num_renamed = 0;
    ssize_t out_delta = 0;
    TRACE(RENAME, 6, "Begin rename Views in layout %s\n", path.c_str());
    rename_classes_in_layout(path, aliases_for_layouts, &num_renamed, &out_delta);
    TRACE(
      RENAME,
      3,
      "Renamed %zu ResStringPool entries in layout %s\n",
      num_renamed,
      path.c_str());
    layout_bytes_delta += out_delta;
    num_layout_renamed += num_renamed;
  }
  mgr.incr_metric("layout_bytes_delta", layout_bytes_delta);
  TRACE(
    RENAME,
    2,
    "Renamed %zu ResStringPool entries, delta %zi bytes\n",
    num_layout_renamed,
    layout_bytes_delta);
}

void RenameClassesPassV2::run_pass(DexStoresVector& stores,
                                   ConfigFiles& conf,
                                   PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(RENAME, 1,
          "RenameClassesPassV2 not run because no ProGuard configuration was "
          "provided.");
    return;
  }
  auto scope = build_class_scope(stores);
  ClassHierarchy class_hierarchy = build_type_hierarchy(scope);
  eval_classes_post(scope, class_hierarchy, mgr);

  always_assert_log(scope.size() < std::pow(Locator::global_class_index_digits_base, Locator::global_class_index_digits_max),
                    "scope size %uz too large", scope.size());
  int total_classes = scope.size();

  // encode the whole sequence as base 62: [0 - 9], [A - Z], [a - z]
  m_digits = std::ceil(std::log(total_classes) / std::log(Locator::global_class_index_digits_base));
  TRACE(RENAME, 1,
        "Total classes in scope for renaming: %d chosen number of digits: %d\n",
        total_classes, m_digits);

  rename_classes(scope, conf, m_rename_annotations, mgr);

  mgr.incr_metric(METRIC_CLASSES_IN_SCOPE, total_classes);

  TRACE(RENAME, 1, "String savings, at least %d-%d = %d bytes \n",
        m_base_strings_size, m_ren_strings_size,
        m_base_strings_size - m_ren_strings_size);
}
