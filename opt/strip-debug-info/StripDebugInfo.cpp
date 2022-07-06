/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StripDebugInfo.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "DexClass.h"
#include "DexUtil.h"
#include "PassManager.h"
#include "Trace.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_NUM_MATCHES = "num_method_matches";
constexpr const char* METRIC_POS_DROPPED = "num_pos_dropped";
constexpr const char* METRIC_VAR_DROPPED = "num_var_dropped";
constexpr const char* METRIC_PROLOGUE_DROPPED = "num_prologue_dropped";
constexpr const char* METRIC_EPILOGUE_DROPPED = "num_epilogue_dropped";
constexpr const char* METRIC_EMPTY_DROPPED = "num_empty_dropped";
constexpr const char* METRIC_SKIPPED_INLINE = "num_skipped_due_to_inlining";

bool is_debug_entry(const MethodItemEntry& mie) {
  return mie.type == MFLOW_DEBUG || mie.type == MFLOW_POSITION;
}

} // namespace

namespace strip_debug_info_impl {

Stats& Stats::operator+=(const Stats& other) {
  num_matches += other.num_matches;
  num_pos_dropped += other.num_pos_dropped;
  num_var_dropped += other.num_var_dropped;
  num_prologue_dropped += other.num_prologue_dropped;
  num_epilogue_dropped += other.num_epilogue_dropped;
  num_empty_dropped += other.num_empty_dropped;
  num_skipped_due_to_inlining += other.num_skipped_due_to_inlining;
  return *this;
}

bool StripDebugInfo::should_remove(const MethodItemEntry& mei, Stats& stats) {
  bool remove = false;
  if (mei.type == MFLOW_DEBUG) {
    auto op = mei.dbgop->opcode();
    switch (op) {
    case DBG_START_LOCAL:
    case DBG_START_LOCAL_EXTENDED:
    case DBG_END_LOCAL:
    case DBG_RESTART_LOCAL:
      if (drop_local_variables()) {
        ++stats.num_var_dropped;
        remove = true;
      }
      break;
    case DBG_SET_PROLOGUE_END:
      if (drop_prologue()) {
        ++stats.num_prologue_dropped;
        remove = true;
      }
      break;
    case DBG_SET_EPILOGUE_BEGIN:
      if (drop_epilogue()) {
        ++stats.num_epilogue_dropped;
        remove = true;
      }
      break;
    default:
      break;
    }
  } else if (mei.type == MFLOW_POSITION) {
    if (drop_line_numbers()) {
      ++stats.num_pos_dropped;
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
bool StripDebugInfo::should_drop_for_synth(const DexMethod* method) const {
  if (!is_synthetic(method) && !is_bridge(method)) {
    return false;
  }

  if (m_config.drop_synth_aggressive) {
    return true;
  }

  return m_config.drop_synth_conservative &&
         (is_bridge(method) ||
          strstr(method->get_name()->c_str(), "access$") != nullptr);
}

Stats StripDebugInfo::run(const Scope& scope) {
  Stats stats;
  walk::code(scope, [&](DexMethod* meth, IRCode& code) {
    stats += run(code, should_drop_for_synth(meth));
  });
  return stats;
}

Stats StripDebugInfo::run(IRCode& code, bool should_drop_synth) {
  Stats stats;
  ++stats.num_matches;
  bool debug_info_empty = true;
  bool force_discard = m_config.drop_all_dbg_info || should_drop_synth;

  for (auto it = code.begin(); it != code.end();) {
    const auto& mie = *it;
    if (should_remove(mie, stats) || (force_discard && is_debug_entry(mie))) {
      // Even though force_discard will drop the debug item below, preventing
      // any of the debug entries for :meth to be output, we still want to
      // erase those entries here so that transformations like inlining won't
      // move these entries into a method that does have a debug item.
      it = code.erase(it);
    } else {
      switch (mie.type) {
      case MFLOW_DEBUG:
        // Any debug information op other than an end sequence means
        // we have debug info.
        if (mie.dbgop->opcode() != DBG_END_SEQUENCE) debug_info_empty = false;
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

  if (m_config.drop_all_dbg_info ||
      (debug_info_empty && m_config.drop_all_dbg_info_if_empty) ||
      force_discard) {
    ++stats.num_empty_dropped;
    code.release_debug_item();
  }
  return stats;
}

} // namespace strip_debug_info_impl

void StripDebugInfoPass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& /* conf */,
                                  PassManager& mgr) {
  auto scope = build_class_scope(stores);
  strip_debug_info_impl::StripDebugInfo impl(m_config);
  auto stats = impl.run(scope);
  TRACE(DBGSTRIP,
        1,
        "Matched on %d methods. Removed %d dbg line entries, %d dbg local var "
        "entries, %d dbg prologue start entries, %d "
        "epilogue end entries, %u empty dbg tables, "
        "%d skipped due to inlining",
        stats.num_matches,
        stats.num_pos_dropped,
        stats.num_var_dropped,
        stats.num_prologue_dropped,
        stats.num_epilogue_dropped,
        stats.num_empty_dropped,
        stats.num_skipped_due_to_inlining);

  mgr.incr_metric(METRIC_NUM_MATCHES, stats.num_matches);
  mgr.incr_metric(METRIC_POS_DROPPED, stats.num_pos_dropped);
  mgr.incr_metric(METRIC_VAR_DROPPED, stats.num_var_dropped);
  mgr.incr_metric(METRIC_PROLOGUE_DROPPED, stats.num_prologue_dropped);
  mgr.incr_metric(METRIC_EPILOGUE_DROPPED, stats.num_epilogue_dropped);
  mgr.incr_metric(METRIC_EMPTY_DROPPED, stats.num_empty_dropped);
  mgr.incr_metric(METRIC_SKIPPED_INLINE, stats.num_skipped_due_to_inlining);

  if (m_config.drop_src_files) {
    TRACE(DBGSTRIP, 1, "dropping src file strings");
    for (auto& dex : scope)
      dex->set_source_file(nullptr);
  }
}

static StripDebugInfoPass s_pass;
