// Copyright 2004-present Facebook. All Rights Reserved.

#include "StripDebugInfo.h"

#include <string>
#include <vector>
#include <unordered_map>

#include "DexClass.h"
#include "DexUtil.h"
#include "Walkers.h"

namespace {

static int num_matches = 0;
static int num_pos_dropped = 0;
static int num_var_dropped = 0;

bool should_strip(const char* str,
    std::vector<std::string>& cls_patterns) {
  for (auto p : cls_patterns) {
    auto substr = strstr(str, p.c_str());
    if (substr != nullptr) {
      return true;
    }
  }
  return false;
}

void strip_src_files(Scope& scope, bool drop_src_files) {
  if (drop_src_files) {
    TRACE(DBGSTRIP, 1, "dropping src file strings\n");
    for (auto& dex : scope) {
      dex->set_source_file(nullptr);
    }
  }
}

void strip_debug_info(Scope& scope,
    bool use_whitelist,
    std::vector<std::string>& cls_patterns,
    std::vector<std::string>& meth_patterns,
    bool drop_all_dbg_info,
    bool drop_local_variables,
    bool drop_line_numbers) {
  walk_methods(
      scope,
      [&](DexMethod* meth) {
        if (!meth->get_code()) return;
        if (!use_whitelist ||
          should_strip(meth->get_class()->get_name()->c_str(), cls_patterns) ||
          should_strip(meth->get_name()->c_str(), meth_patterns)) {
          if (drop_all_dbg_info) {
            meth->get_code()->remove_debug_item();
            num_matches++;
          } else if (drop_local_variables || drop_line_numbers) {
            auto& dbg_item = meth->get_code()->get_debug_item();
            if (dbg_item) {
              dbg_item->remove_parameter_names();
              auto& dbg_entries = dbg_item->get_entries();
              std::vector<DexDebugEntry> filtered_list;
              for (auto& entry : dbg_entries) {
                if (entry.type == DexDebugEntryType::Position &&
                  !drop_line_numbers) {
                  filtered_list.push_back(std::move(entry));
                } else if (entry.type == DexDebugEntryType::Instruction &&
                  !drop_local_variables) {
                  filtered_list.push_back(std::move(entry));
                } else if (entry.type == DexDebugEntryType::Position &&
                  drop_line_numbers) {
                  num_pos_dropped++;
                } else if (entry.type == DexDebugEntryType::Instruction &&
                  drop_local_variables) {
                  num_var_dropped++;
                }
              }
              dbg_item->set_entries(std::move(filtered_list));
            }
          }
        }
      });
}

}

void StripDebugInfoPass::run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  auto scope = build_class_scope(stores);
  strip_debug_info(scope,
      m_use_whitelist,
      m_cls_patterns,
      m_meth_patterns,
      m_drop_all_dbg_info,
      m_drop_local_variables,
      m_drop_line_nrs);
  TRACE(DBGSTRIP, 1, "matched on %d methods. Removed %d dbg line entries and %d dbg local var entries\n",
      num_matches,
      num_pos_dropped,
      num_var_dropped);
  strip_src_files(scope, m_drop_src_files);
}

static StripDebugInfoPass s_pass;
