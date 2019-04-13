/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RenameClasses.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

#include "Walkers.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "DexUtil.h"
#include "ReachableClasses.h"
#include "ClassHierarchy.h"

#define MAX_DESCRIPTOR_LENGTH (1024)
#define MAX_IDENT_CHAR (52)
#define MAX_IDENT (MAX_IDENT_CHAR * MAX_IDENT_CHAR * MAX_IDENT_CHAR)

#define METRIC_CLASSES_IN_SCOPE "num_classes_in_scope"
#define METRIC_RENAMED_CLASSES "**num_renamed**"
#define METRIC_CANT_RENAME_ANNOTATION "num_cant_rename_annotations"
#define METRIC_CANT_RENAME_UNTOUCHABLE "num_cant_rename_untouchable"
#define METRIC_CANT_RENAME_AND_CANT_DELETE "num_cant_rename_and_cant_delete"
#define METRIC_NOT_WHITELISTED "num_not_whitelisted"

int match_short = 0;
int match_long = 0;
int match_inner = 0;

int base_strings_size = 0;
int ren_strings_size = 0;

static char getident(int num) {
  if (num < 26) {
    return 'A' + num;
  } else {
    return 'a' + num - 26;
  }
}

void get_next_ident(char *out, int &num) {
  // *sigh* re-write when not tired.
  int low = num;
  int mid = (num / 52);
  int top = (mid / 52);
  always_assert_log(num <= MAX_IDENT,
                    "Bailing, Ident %d, greater than maximum\n", num);
  if (top) {
    *out++ = getident(top - 1);
    low -= (top *52*52);
  }
  if (mid) {
    mid -= (top * 52);
    *out++ = getident(mid);
    low -= (mid * 52);
  }
  *out++ = getident(low);
  *out++ = '\0';
  num++;
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
    if (is_package_protected(clazz)) set_public(clazz);
  }
}

bool should_rename(DexClass *clazz,
    std::vector<std::string>& pre_patterns,
    std::vector<std::string>& post_patterns,
    std::unordered_set<const DexType*>& untouchables,
    bool rename_annotations,
    PassManager& mgr) {
  if (!rename_annotations && is_annotation(clazz)) {
    mgr.incr_metric(METRIC_CANT_RENAME_ANNOTATION, 1);
    return false;
  }
  if (untouchables.count(clazz->get_type())) {
    mgr.incr_metric(METRIC_CANT_RENAME_UNTOUCHABLE, 1);
    return false;
  }
  auto chstring = clazz->get_type()->get_name()->c_str();
  /* We're assuming anonymous classes are safe always safe to rename. */
  auto last_cash = strrchr(chstring, '$');
  if (last_cash != nullptr) {
    auto val = *++last_cash;
    if (val >= '0' && val <= '9') {
      match_inner++;
      return true;
    }
  }
  /* Check for more aggressive, but finer grained filters first */
  for (auto p : pre_patterns) {
    auto substr = strstr(chstring, p.c_str());
    if (substr != nullptr) {
      if (p.length() > 1) {
        match_long++;
      } else {
        match_short++;
      }
      return true;
    }
  }
  if (!can_rename(clazz) && !can_delete(clazz)) {
    mgr.incr_metric(METRIC_CANT_RENAME_AND_CANT_DELETE, 1);
    return false;
  }
  /* Check for wider, less precise filters */
  for (auto p : post_patterns) {
    auto substr = strstr(chstring, p.c_str());
    if (substr != nullptr) {
      if (p.length() > 1) {
        match_long++;
      } else {
        match_short++;
      }
      return true;
    }
  }
  mgr.incr_metric(METRIC_NOT_WHITELISTED, 1);
  return false;
}

void rename_classes(
    Scope& scope,
    std::vector<std::string>& pre_whitelist_patterns,
    std::vector<std::string>& post_whitelist_patterns,
    std::unordered_set<const DexType*>& untouchables,
    bool rename_annotations,
    PassManager& mgr) {
  unpackage_private(scope);
  int clazz_ident = 0;
  std::map<DexString*, DexString*, dexstrings_comparator> aliases;
  for(auto clazz: scope) {
    if (!should_rename(
        clazz, pre_whitelist_patterns, post_whitelist_patterns,
        untouchables, rename_annotations, mgr)) {
      continue;
    }
    char clzname[4];
    get_next_ident(clzname, clazz_ident);

    auto dtype = clazz->get_type();
    auto oldname = dtype->get_name();


    // The X helps our hacked Dalvik classloader recognize that a
    // class name is the output of the redex renamer and thus will
    // never be found in the Android platform.
    // The $ indicates that the class was originally an inner class.
    // Some code, most notably android instrumentation runner, uses
    // this information to decide whether or not to classload the class.
    bool inner = strrchr(oldname->c_str(), '$');
    char descriptor[10];
    sprintf(descriptor, "LX%s%s;", inner ? "$" : "", clzname);
    auto dstring = DexString::make_string(descriptor);
    aliases[oldname] = dstring;
    dtype->set_name(dstring);
    std::string old_str(oldname->c_str());
    std::string new_str(descriptor);
    base_strings_size += strlen(oldname->c_str());
    base_strings_size += strlen(dstring->c_str());
    TRACE(RENAME, 4, "'%s'->'%s'\n", oldname->c_str(), descriptor);
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
      arraytype->set_name(dstring);
    }
  }
  mgr.incr_metric(METRIC_RENAMED_CLASSES, match_short + match_long + match_inner);
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
  walk::annotations(scope, [&](DexAnnotation* anno) {
    static DexType *dalviksig =
      DexType::get_type("Ldalvik/annotation/Signature;");
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
}

void RenameClassesPass::run_pass(DexStoresVector& stores,
                                 ConfigFiles& conf,
                                 PassManager& mgr) {
  const JsonWrapper& json_cfg = conf.get_json_config();
  if (json_cfg.get("emit_name_based_locators", false)) {
    // TODO: Purge the old RenameClassesPass entirely everywhere.
    fprintf(stderr,
            "[RenameClassesPass] error: Configuration option "
            "emit_locator_strings is not compatible with RenameClassesPass. "
            "Upgrade to RenameClassesPassV2 instead.\n");
    exit(EXIT_FAILURE);
  }

  auto scope = build_class_scope(stores);
  ClassHierarchy ch = build_type_hierarchy(scope);
  std::unordered_set<const DexType*> untouchables;
  for (const auto& base : m_untouchable_hierarchies) {
    auto base_type = DexType::get_type(base.c_str());
    if (base_type != nullptr) {
      untouchables.insert(base_type);
      TypeSet children;
      get_all_children(ch, base_type, children);
      untouchables.insert(children.begin(), children.end());
    }
  }
  mgr.incr_metric(METRIC_CLASSES_IN_SCOPE, scope.size());
  rename_classes(
      scope, m_pre_filter_whitelist, m_post_filter_whitelist,
      untouchables, m_rename_annotations, mgr);
  TRACE(RENAME, 1,
      "renamed classes: %d anon classes, %d from single char patterns, "
      "%d from multi char patterns\n",
      match_inner,
      match_short,
      match_long);
  TRACE(RENAME, 1, "String savings, at least %d bytes \n",
      base_strings_size - ren_strings_size);
}

static RenameClassesPass s_pass;
