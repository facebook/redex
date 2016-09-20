/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "InterDex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <unordered_set>

#include "Creators.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "ConfigFiles.h"
#include "ReachableClasses.h"
#include "StringUtil.h"
#include "Transform.h"
#include "Walkers.h"

namespace {

typedef std::unordered_set<DexMethod*> mrefs_t;
typedef std::unordered_set<DexField*> frefs_t;

size_t global_dmeth_cnt;
size_t global_smeth_cnt;
size_t global_vmeth_cnt;
size_t global_methref_cnt;
size_t global_fieldref_cnt;
size_t global_cls_cnt;
size_t cls_skipped_in_primary = 0;
size_t cls_skipped_in_secondary = 0;
size_t cold_start_set_dex_count = 1000;

bool emit_canaries = false;
static int64_t linear_alloc_limit;

static void gather_mrefs(DexClass* cls, mrefs_t& mrefs, frefs_t& frefs) {
  std::vector<DexMethod*> method_refs;
  std::vector<DexField*> field_refs;
  cls->gather_methods(method_refs);
  cls->gather_fields(field_refs);
  mrefs.insert(method_refs.begin(), method_refs.end());
  frefs.insert(field_refs.begin(), field_refs.end());
}

static const int kMaxMethodRefs = ((64 * 1024) - 1);
static const int kMaxFieldRefs = 64 * 1024 - 1;
static const char kCanaryPrefix[] = "Lsecondary/dex";
static const char kCanaryClassFormat[] = "Lsecondary/dex%02d/Canary;";
static const size_t kCanaryClassBufsize = sizeof(kCanaryClassFormat);
static const int kMaxDexNum = 99;

struct dex_emit_tracker {
  unsigned la_size;
  mrefs_t mrefs;
  frefs_t frefs;
  std::vector<DexClass*> outs;
  std::unordered_set<DexClass*> emitted;
  std::unordered_map<std::string, DexClass*> clookup;
};

static void update_dex_stats (
    size_t cls_cnt,
    size_t methrefs_cnt,
    size_t frefs_cnt) {
  global_cls_cnt += cls_cnt;
  global_methref_cnt += methrefs_cnt;
  global_fieldref_cnt += frefs_cnt;
}

static void update_class_stats(DexClass* clazz) {
  int cnt_smeths = 0;
  for (auto const m : clazz->get_dmethods()) {
    if (is_static(m)) {
      cnt_smeths++;
    }
  }
  global_smeth_cnt += cnt_smeths;
  global_dmeth_cnt += clazz->get_dmethods().size();
  global_vmeth_cnt += clazz->get_vmethods().size();
}

static void flush_out_dex(
    dex_emit_tracker& det,
    DexClassesVector& outdex,
    size_t mrefs_size,
    size_t frefs_size) {
  DexClasses dc(det.outs.size());
  for (size_t i = 0; i < det.outs.size(); i++) {
    dc.insert_at(det.outs[i], i);
  }
  outdex.emplace_back(std::move(dc));
  // print out stats
  TRACE(IDEX, 1,
    "terminating dex at classes %lu, lin alloc %d:%d, mrefs %lu:%d, frefs "
    "%lu:%d\n",
    det.outs.size(),
    det.la_size,
    linear_alloc_limit,
    mrefs_size,
    kMaxMethodRefs,
    frefs_size,
    kMaxFieldRefs);

  update_dex_stats(det.outs.size(), mrefs_size, frefs_size);

  det.la_size = 0;
  det.mrefs.clear();
  det.frefs.clear();
  det.outs.clear();
}

static void flush_out_dex(
    dex_emit_tracker& det,
    DexClassesVector& outdex) {
  flush_out_dex(det, outdex, det.mrefs.size(), det.frefs.size());
}

static void flush_out_secondary(
    dex_emit_tracker &det,
    DexClassesVector &outdex,
    size_t mrefs_size,
    size_t frefs_size) {
  // don't emit dex if we don't have any classes
  if (!det.outs.size()) {
    return;
  }
  /* Find the Canary class and add it in. */
  if (emit_canaries) {
    int dexnum = ((int)outdex.size());
    char buf[kCanaryClassBufsize];
    always_assert_log(dexnum <= kMaxDexNum,
                      "Bailing, Max dex number surpassed %d\n", dexnum);
    snprintf(buf, sizeof(buf), kCanaryClassFormat, dexnum);
    std::string canaryname(buf);
    auto it = det.clookup.find(canaryname);
    if (it == det.clookup.end()) {
      TRACE(IDEX, 2, "Warning, no canary class %s found\n", buf);
      auto canary_type = DexType::make_type(canaryname.c_str());
      auto canary_cls = type_class(canary_type);
      if (canary_cls == nullptr) {
        // class doesn't exist, we have to create it
        // this can happen if we grow the number of dexes
        ClassCreator cc(canary_type);
        cc.set_access(ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
        cc.set_super(get_object_type());
        canary_cls = cc.create();
      }
      det.outs.push_back(canary_cls);
    } else {
      auto clazz = it->second;
      det.outs.push_back(clazz);
    }
  }

  /* Now emit our outs list... */
  flush_out_dex(det, outdex, mrefs_size, frefs_size);
}

static void flush_out_secondary(
    dex_emit_tracker &det,
    DexClassesVector &outdex) {
  flush_out_secondary(
      det,
      outdex,
      det.mrefs.size(),
      det.frefs.size());
}

static bool is_canary(DexClass* clazz) {
  const char* cname = clazz->get_type()->get_name()->c_str();
  if (strncmp(cname, kCanaryPrefix, sizeof(kCanaryPrefix) - 1) == 0)
    return true;
  return false;
}

struct PenaltyPattern {
  const char* suffix;
  unsigned penalty;

  PenaltyPattern(const char* str, unsigned pen)
    : suffix(str),
      penalty(pen)
  {}
};

static const PenaltyPattern kPatterns[] = {
  { "Layout;", 1500 },
  { "View;", 1500 },
  { "ViewGroup;", 1800 },
  { "Activity;", 1500 },
};

const unsigned kObjectVtable = 48;
const unsigned kMethodSize = 52;
const unsigned kInstanceFieldSize = 16;
const unsigned kVtableSlotSize = 4;

static bool matches_penalty(const char* str, unsigned& penalty) {
  for (auto const& pattern : kPatterns) {
    if (ends_with(str, pattern.suffix)) {
      penalty = pattern.penalty;
      return true;
    }
  }
  return false;
}

/**
 * Estimates the linear alloc space consumed by the class at runtime.
 */
static unsigned estimate_linear_alloc(DexClass* clazz) {
  unsigned lasize = 0;
  /*
   * VTable guestimate.  Technically we could do better here,
   * but only so much.  Try to stay bug-compatible with
   * DalvikStatsTool.
   */
  if (!(clazz->get_access() & DEX_ACCESS_INTERFACE)) {
    unsigned vtablePenalty = kObjectVtable;
    if (!matches_penalty(clazz->get_type()->get_name()->c_str(), vtablePenalty)
        && clazz->get_super_class() != nullptr) {
      /* what?, we could be redexing object some day... :) */
      matches_penalty(
          clazz->get_super_class()->get_name()->c_str(), vtablePenalty);
    }
    lasize += vtablePenalty;
    lasize += clazz->get_vmethods().size() * kVtableSlotSize;
  }
  /* Dmethods... */
  lasize += clazz->get_dmethods().size() * kMethodSize;
  /* Vmethods... */
  lasize += clazz->get_vmethods().size() * kMethodSize;
  /* Instance Fields */
  lasize += clazz->get_ifields().size() * kInstanceFieldSize;
  return lasize;
}

static void emit_class(dex_emit_tracker &det, DexClassesVector &outdex,
    DexClass *clazz, bool is_primary) {
  if(det.emitted.count(clazz) != 0)
    return;
  if(is_canary(clazz))
    return;
  unsigned laclazz = estimate_linear_alloc(clazz);
  auto mrefs_size = det.mrefs.size();
  auto frefs_size = det.frefs.size();
  gather_mrefs(clazz, det.mrefs, det.frefs);
  if ((det.la_size + laclazz) > linear_alloc_limit ||
      det.mrefs.size() >= kMaxMethodRefs ||
      det.frefs.size() >= kMaxFieldRefs) {
    /* Emit out list */
    always_assert_log(!is_primary,
      "would have to do an early flush on the primary dex\n"
      "la %d:%d , mrefs %lu:%d frefs %lu:%d\n",
      det.la_size + laclazz,
      linear_alloc_limit,
      det.mrefs.size(),
      kMaxMethodRefs,
      det.frefs.size(),
      kMaxFieldRefs);
    flush_out_secondary(det, outdex, mrefs_size, frefs_size);
    gather_mrefs(clazz, det.mrefs, det.frefs);
  }
  det.la_size += laclazz;
  det.outs.push_back(clazz);
  det.emitted.insert(clazz);
  update_class_stats(clazz);
}

static void emit_class(dex_emit_tracker &det, DexClassesVector &outdex,
    DexClass *clazz) {
  emit_class(det, outdex, clazz, false);
}

static std::unordered_set<const DexClass*> find_unrefenced_coldstart_classes(
  const Scope& scope,
  dex_emit_tracker& det,
  const std::vector<std::string>& interdexorder,
  bool static_prune_classes) {
  int old_no_ref = -1;
  int new_no_ref = 0;
  std::unordered_set<DexClass*> coldstart_classes;
  std::unordered_set<const DexClass*> cold_cold_references;
  std::unordered_set<const DexClass*> unreferenced_classes;
  Scope input_scope = scope;

  // don't do analysis if we're not going doing pruning
  if (!static_prune_classes) {
    return unreferenced_classes;
  }

  for (auto const& class_string : interdexorder) {
    if (det.clookup.count(class_string)) {
      coldstart_classes.insert(det.clookup[class_string]);
    }
  }

  while (old_no_ref != new_no_ref) {
    old_no_ref = new_no_ref;
    new_no_ref = 0;
    cold_cold_references.clear();
    walk_code(
      input_scope,
      [&](DexMethod* meth) {
        if (coldstart_classes.count(type_class(meth->get_class())) > 0) {
          return true;
        }
        return false;
      },
      [&](DexMethod* meth, const DexCode& code) {
        auto base_cls = type_class(meth->get_class());
        for (auto const& inst : code.get_instructions()) {
          DexClass* called_cls = nullptr;
          if (inst->has_methods()) {
            auto method_access = static_cast<DexOpcodeMethod*>(inst);
            called_cls = type_class(method_access->get_method()->get_class());
          } else if (inst->has_fields()) {
            auto field_access = static_cast<DexOpcodeField*>(inst);
            called_cls = type_class(field_access->field()->get_class());
          } else if (inst->has_types()) {
            auto type_access = static_cast<DexOpcodeType*>(inst);
            called_cls = type_class(type_access->get_type());
          }
          if (called_cls != nullptr &&
            base_cls != called_cls &&
            coldstart_classes.count(called_cls) > 0) {
              cold_cold_references.insert(called_cls);
          }
        }
      }
    );
    for (const auto& cls: scope) {
      // make sure we don't drop classes which
      // might be called from native code
      if (!can_rename(cls)) {
        cold_cold_references.insert(cls);
      }
    }
    // get all classes in the reference
    // set, even if they are not referenced
    // by opcodes directly
    for (const auto& cls: input_scope) {
      if (cold_cold_references.count(cls)) {
        std::vector<DexType*> types;
        cls->gather_types(types);
        for (const auto& type: types) {
          auto cls = type_class(type);
          cold_cold_references.insert(cls);
        }
      }
    }
    Scope output_scope;
    for (auto& cls : coldstart_classes) {
      if (can_rename(cls) && cold_cold_references.count(cls) == 0) {
        new_no_ref++;
        unreferenced_classes.insert(cls);
      } else {
        output_scope.push_back(cls);
      }
    }
    TRACE(IDEX, 1, "found %d classes in coldstart with no references\n", new_no_ref);
    input_scope = output_scope;
  }
  return unreferenced_classes;
}

static DexClassesVector run_interdex(
  const DexClassesVector& dexen,
  ConfigFiles& cfg,
  bool allow_cutting_off_dex,
  bool static_prune_classes,
  bool normal_primary_dex) {

  global_dmeth_cnt = 0;
  global_smeth_cnt = 0;
  global_vmeth_cnt = 0;
  global_methref_cnt = 0;
  global_fieldref_cnt = 0;
  global_cls_cnt = 0;

  cls_skipped_in_primary = 0;
  cls_skipped_in_secondary = 0;

  auto interdexorder = cfg.get_coldstart_classes();
  dex_emit_tracker det;
  dex_emit_tracker primary_det;
  for (auto const& dex : dexen) {
    for (auto const& clazz : dex) {
      std::string clzname(clazz->get_type()->get_name()->c_str());
      det.clookup[clzname] = clazz;
    }
  }

  auto scope = build_class_scope(dexen);

  auto unreferenced_classes = find_unrefenced_coldstart_classes(
      scope,
      det,
      interdexorder,
      static_prune_classes);

  DexClassesVector outdex;

  // We have a bunch of special logic for the primary dex
  // which we only use if we can't touch the primary dex.
  if (!normal_primary_dex) {
    // build a separate lookup table for the primary dex,
    // since we have to make sure we keep all classes
    // in the same dex
    auto const& primary_dex = dexen[0];
    for (auto const& clazz : primary_dex) {
      std::string clzname(clazz->get_type()->get_name()->c_str());
      primary_det.clookup[clzname] = clazz;
    }

    /* First emit just the primary dex, but sort it
     * according to interdex order
     **/
    primary_det.la_size = 0;
    // first add the classes in the interdex list
    auto coldstart_classes_in_primary = 0;
    for (auto& entry : interdexorder) {
      auto it = primary_det.clookup.find(entry);
      if (it == primary_det.clookup.end()) {
        TRACE(IDEX, 4, "No such entry %s\n", entry.c_str());
        continue;
      }
      auto clazz = it->second;
      if (unreferenced_classes.count(clazz)) {
        TRACE(IDEX, 3, "%s no longer linked to coldstart set.\n", SHOW(clazz));
        cls_skipped_in_primary++;
        continue;
      }
      emit_class(primary_det, outdex, clazz, true);
      coldstart_classes_in_primary++;
    }
    // now add the rest
    for (auto const& clazz : primary_dex) {
      emit_class(primary_det, outdex, clazz, true);
    }
    TRACE(IDEX, 1,
        "%d out of %lu classes in primary dex in interdex list\n",
        coldstart_classes_in_primary,
        primary_det.outs.size());
    flush_out_dex(primary_det, outdex);
    // record the primary dex classes in the main emit tracker,
    // so we don't emit those classes again. *cough*
    for (auto const& clazz : primary_dex) {
      det.emitted.insert(clazz);
    }
  }

  det.la_size = 0;

  for (auto& entry : interdexorder) {
    auto it = det.clookup.find(entry);
    if (it == det.clookup.end()) {
      TRACE(IDEX, 4, "No such entry %s\n", entry.c_str());
      if (entry.find("DexEndMarker") != std::string::npos) {
        TRACE(IDEX, 1, "Terminating dex due to DexEndMarker\n");
        flush_out_secondary(det, outdex);
      }
      continue;
    }
    auto clazz = it->second;
    if (unreferenced_classes.count(clazz)) {
      TRACE(IDEX, 3, "%s no longer linked to coldstart set.\n", SHOW(clazz));
      cls_skipped_in_secondary++;
      continue;
    }
    emit_class(det, outdex, clazz);
  }

  /* Now emit the classes we omitted from the original
   * coldstart set
   */
  for (auto& entry : interdexorder) {
    auto it = det.clookup.find(entry);
    if (it == det.clookup.end()) {
      TRACE(IDEX, 4, "No such entry %s\n", entry.c_str());
      continue;
    }
    auto clazz = it->second;
    if (unreferenced_classes.count(clazz)) {
      emit_class(det, outdex, clazz);
    }
  }

  // -1 because we're not counting the primary dex
  cold_start_set_dex_count = outdex.size();

  /* Now emit the kerf that wasn't specified in the head
   * or primary list.
   */
  for (auto clazz : scope) {
    emit_class(det, outdex, clazz);
  }

  /* Finally, emit the "left-over" det.outs */
  if (det.outs.size()) {
    flush_out_secondary(det, outdex);
  }
  TRACE(IDEX, 1,
        "InterDex secondary dex count %d\n", (int)(outdex.size() - 1));
  TRACE(IDEX, 1,
    "global stats: %lu mrefs, %lu frefs, %lu cls, %lu dmeth, %lu smeth, %lu vmeth\n",
    global_methref_cnt,
    global_fieldref_cnt,
    global_cls_cnt,
    global_dmeth_cnt,
    global_smeth_cnt,
    global_vmeth_cnt);
  TRACE(IDEX, 1,
    "removed %d classes from coldstart list in primary dex, \
%d in secondary dexes due to static analysis\n",
    cls_skipped_in_primary,
    cls_skipped_in_secondary);
  return outdex;
}

} // End namespace


void InterDexPass::run_pass(DexClassesVector& dexen, ConfigFiles& cfg, PassManager& mgr) {
  emit_canaries = m_emit_canaries;
  linear_alloc_limit = m_linear_alloc_limit;
  dexen = run_interdex(dexen, cfg, true, m_static_prune, m_normal_primary_dex);
  mgr.incr_metric(METRIC_COLD_START_SET_DEX_COUNT, cold_start_set_dex_count);
}

void InterDexPass::run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  for (auto& store : stores) {
    if (store.get_name() == "classes") {
      run_pass(store.get_dexen(), cfg, mgr);
    }
  }
}

static InterDexPass s_pass;
