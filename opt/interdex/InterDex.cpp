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
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "PgoFiles.h"
#include "Transform.h"
#include "walkers.h"

namespace {

typedef std::unordered_set<DexMethod*> mrefs_t;
typedef std::unordered_set<DexField*> frefs_t;

size_t global_dmeth_cnt;
size_t global_smeth_cnt;
size_t global_vmeth_cnt;
size_t global_methref_cnt;
size_t global_fieldref_cnt;
size_t global_cls_cnt;

static void gather_mrefs(DexClass* cls, mrefs_t& mrefs, frefs_t& frefs) {
  std::vector<DexMethod*> method_refs;
  std::vector<DexField*> field_refs;
  cls->gather_methods(method_refs);
  cls->gather_fields(field_refs);
  mrefs.insert(method_refs.begin(), method_refs.end());
  frefs.insert(field_refs.begin(), field_refs.end());
}

#ifdef GINGER_BREAD
static const int kMaxLinearAlloc = (2600 * 1024);
#else
static const int kMaxLinearAlloc = (11600 * 1024);
#endif
static const int kMaxMethodRefs = ((64 * 1024) - 1);
static const int kMaxFieldRefs = 64 * 1024 - 1;
static const char* kCanaryPrefix = "Lsecondary/dex";
static const char* kCanaryClassFormat = "Lsecondary/dex%02d/Canary;";
static const int kCanaryClassBufsize = strlen(kCanaryClassFormat) + 1;
static const int kMaxDexNum = 99;

struct dex_emit_tracker {
  int la_size;
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
    kMaxLinearAlloc,
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
  /* Find the Canary class and add it in. */
  int dexnum = ((int)outdex.size());
  char buf[kCanaryClassBufsize];
  always_assert_log(dexnum <= kMaxDexNum,
                    "Bailing, Max dex number surpassed %d\n", dexnum);
  snprintf(buf, sizeof(buf), kCanaryClassFormat, dexnum);
  std::string canaryname(buf);
  auto it = det.clookup.find(canaryname);
  if (it == det.clookup.end()) {
    fprintf(stderr, "Warning, no canary class %s found\n", buf);
    ClassCreator cc(DexType::make_type(canaryname.c_str()));
    cc.set_access(ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
    cc.set_super(get_object_type());
    det.outs.push_back(cc.create());
  } else {
    auto clazz = it->second;
    det.outs.push_back(clazz);
  }

  /* Now emit our outs list... */
  flush_out_dex(det, outdex, mrefs_size, frefs_size);
}

static void flush_out_secondary(
    dex_emit_tracker &det,
    DexClassesVector &outdex) {
  flush_out_secondary(det, outdex, det.mrefs.size(), det.frefs.size());
}

static bool is_canary(DexClass* clazz) {
  const char* cname = clazz->get_type()->get_name()->c_str();
  if (strncmp(cname, kCanaryPrefix, sizeof(kCanaryPrefix) - 1) == 0)
    return true;
  return false;
}

static void emit_class(dex_emit_tracker &det, DexClassesVector &outdex,
    DexClass *clazz, bool is_primary) {
  if(det.emitted.count(clazz) != 0)
    return;
  if(is_canary(clazz))
    return;
  int laclazz = estimate_linear_alloc(clazz);
  auto mrefs_size = det.mrefs.size();
  auto frefs_size = det.frefs.size();
  gather_mrefs(clazz, det.mrefs, det.frefs);
  if ((det.la_size + laclazz) > kMaxLinearAlloc ||
      det.mrefs.size() >= kMaxMethodRefs ||
      det.frefs.size() >= kMaxFieldRefs) {
    /* Emit out list */
    always_assert_log(!is_primary,
      "would have to do an early flush on the primary dex\n"
      "la %d:%d , mrefs %lu:%d frefs %lu:%d\n",
      det.la_size + laclazz,
      kMaxLinearAlloc,
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

static DexClassesVector run_interdex(
  const DexClassesVector& dexen,
  PgoFiles& pgo,
  bool allow_cutting_off_dex
) {
  global_dmeth_cnt = 0;
  global_smeth_cnt = 0;
  global_vmeth_cnt = 0;
  global_methref_cnt = 0;
  global_fieldref_cnt = 0;
  global_cls_cnt = 0;

  auto interdexorder = pgo.get_coldstart_classes();
  dex_emit_tracker det;
  dex_emit_tracker primary_det;
  for (auto const& dex : dexen) {
    for (auto const& clazz : dex) {
      std::string clzname(clazz->get_type()->get_name()->c_str());
      det.clookup[clzname] = clazz;
    }
  }

  DexClassesVector outdex;

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
    emit_class(det, outdex, clazz);
  }

  Scope scope = build_class_scope(dexen);

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
  return outdex;
}

}

void InterDexPass::run_pass(DexClassesVector& dexen, PgoFiles& pgo) {

  auto first_attempt = run_interdex(dexen, pgo, true);
  if (first_attempt.size() > dexen.size()) {
    fprintf(stderr, "Warning, Interdex grew the number of dexes from %lu to %lu! \n \
        Retrying without cutting off interdex dexes. \n", dexen.size(), first_attempt.size());
    dexen = run_interdex(dexen, pgo, false);
  } else {
    dexen = std::move(first_attempt);
  }
}
