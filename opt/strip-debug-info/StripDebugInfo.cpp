/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "StripDebugInfo.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "DexClass.h"
#include "DexUtil.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_NUM_MATCHES = "num_method_matches";
constexpr const char* METRIC_POS_DROPPED = "num_pos_dropped";
constexpr const char* METRIC_VAR_DROPPED = "num_var_dropped";
constexpr const char* METRIC_PROLOGUE_DROPPED = "num_prologue_dropped";
constexpr const char* METRIC_EPILOGUE_DROPPED = "num_epilogue_dropped";
constexpr const char* METRIC_EMPTY_DROPPED = "num_empty_dropped";

bool pattern_matches(const char* str,
                     const std::vector<std::string>& patterns) {
  for (const auto& p : patterns) {
    auto substr = strstr(str, p.c_str());
    if (substr != nullptr) {
      return true;
    }
  }
  return false;
}

bool is_debug_entry(const MethodItemEntry& mie) {
  return mie.type == MFLOW_DEBUG || mie.type == MFLOW_POSITION;
}

} // namespace

bool StripDebugInfoPass::method_passes_filter(DexMethod* meth) const {
  return !m_use_whitelist ||
         pattern_matches(meth->get_class()->get_name()->c_str(),
                         m_cls_patterns) ||
         pattern_matches(meth->get_name()->c_str(), m_meth_patterns);
}

bool StripDebugInfoPass::should_remove(const MethodItemEntry& mei) {
  bool remove = false;
  if (mei.type == MFLOW_DEBUG) {
    auto op = mei.dbgop->opcode();
    switch (op) {
    case DBG_START_LOCAL:
    case DBG_START_LOCAL_EXTENDED:
    case DBG_END_LOCAL:
    case DBG_RESTART_LOCAL:
      if (drop_local_variables()) {
        ++m_num_var_dropped;
        remove = true;
      }
      break;
    case DBG_SET_PROLOGUE_END:
      if (drop_prologue()) {
        ++m_num_prologue_dropped;
        remove = true;
      }
      break;
    case DBG_SET_EPILOGUE_BEGIN:
      if (drop_epilogue()) {
        ++m_num_epilogue_dropped;
        remove = true;
      }
      break;
    default:
      break;
    }
  } else if (mei.type == MFLOW_POSITION) {
    if (drop_line_numbers()) {
      ++m_num_pos_dropped;
      remove = true;
    }
  }
  return remove;
}

/*
 * Debug info in static methods is often not terribly useful.  Bridge and
 * accessor methods seem to have their line numbers point to the top of their
 * class definition; setting drop_synth_conservative will remove debug info for
 * these methods.
 *
 * Some code-generating annotations have their code point to the annotation
 * site, which I suppose is mildly useful, but we can often figure that out
 * from the class name anyway. However, conducting a comprehensive analysis of
 * all synthetic methods is hard, so it's hard to be sure that stripping all of
 * them of debug info is safe -- hence I'm gating their removal under the
 * drop_synth_aggressive flag.
 */
bool StripDebugInfoPass::should_drop_for_synth(const DexMethod* method) const {
  if (!is_synthetic(method) && !is_bridge(method)) {
    return false;
  }

  if (m_drop_synth_aggressive) {
    return true;
  }

  return m_drop_synth_conservative &&
         (is_bridge(method) ||
          strstr(method->get_name()->c_str(), "access$") != nullptr);
}

void StripDebugInfoPass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& cfg,
                                  PassManager& mgr) {
  m_num_matches = 0;
  m_num_pos_dropped = 0;
  m_num_var_dropped = 0;
  m_num_prologue_dropped = 0;
  m_num_epilogue_dropped = 0;
  m_num_empty_dropped = 0;
  auto scope = build_class_scope(stores);
  walk::code(scope, [&](DexMethod* meth, IRCode& code) {
    if (!method_passes_filter(meth)) return;
    ++m_num_matches;
    bool debug_info_empty = true;
    bool force_discard = m_drop_all_dbg_info || should_drop_for_synth(meth);

    for (auto it = code.begin(); it != code.end();) {
      const auto& mei = *it;
      if (should_remove(mei) || (force_discard && is_debug_entry(mei))) {
        // Even though force_discard will drop the debug item below, preventing
        // any of the debug entries for :meth to be output, we still want to
        // erase those entries here so that transformations like inlining won't
        // move these entries into a method that does have a debug item.
        it = code.erase(it);
      } else {
        switch (mei.type) {
        case MFLOW_DEBUG:
          // Any debug information op other than an end sequence means
          // we have debug info.
          if (mei.dbgop->opcode() != DBG_END_SEQUENCE) debug_info_empty = false;
          break;
        case MFLOW_POSITION:
          // Any line position entry means we have debug info.
          debug_info_empty = false;
          break;
        default:
          break;
        }
        ++it;
      }
    }

    if (m_drop_all_dbg_info ||
        (debug_info_empty && m_drop_all_dbg_info_if_empty) || force_discard) {
      ++m_num_empty_dropped;
      code.release_debug_item();
    }
  });

  TRACE(DBGSTRIP,
        1,
        "matched on %d methods. Removed %d dbg line entries, %d dbg local var "
        "entries, %d dbg prologue start entries, %d "
        "epilogue end entries and %u empty dbg tables.\n",
        m_num_matches,
        m_num_pos_dropped,
        m_num_var_dropped,
        m_num_prologue_dropped,
        m_num_epilogue_dropped,
        m_num_empty_dropped);

  mgr.incr_metric(METRIC_NUM_MATCHES, m_num_matches);
  mgr.incr_metric(METRIC_POS_DROPPED, m_num_pos_dropped);
  mgr.incr_metric(METRIC_VAR_DROPPED, m_num_var_dropped);
  mgr.incr_metric(METRIC_PROLOGUE_DROPPED, m_num_prologue_dropped);
  mgr.incr_metric(METRIC_EPILOGUE_DROPPED, m_num_epilogue_dropped);
  mgr.incr_metric(METRIC_EMPTY_DROPPED, m_num_empty_dropped);

  if (m_drop_src_files) {
    TRACE(DBGSTRIP, 1, "dropping src file strings\n");
    for (auto& dex : scope)
      dex->set_source_file(nullptr);
  }
}

static StripDebugInfoPass s_pass;
