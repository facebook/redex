/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NopperPass.h"

#include <cmath>
#include <random>

#include "ConfigFiles.h"
#include "DexStoreUtil.h"
#include "DexStructure.h"
#include "DexUtil.h"
#include "Nopper.h"
#include "PassManager.h"
#include "RClass.h"
#include "Walkers.h"

namespace {

constexpr const char* SPLIT_CLASS_NAME_PREFIX = "Lcom/redex/Nopper$";

nopper_impl::AuxiliaryDefs create_auxiliary_defs(DexStore* store,
                                                 uint32_t index) {
  std::string name = SPLIT_CLASS_NAME_PREFIX;
  if (!store->is_root_store()) {
    auto store_id = get_unique_store_id(store->get_name().c_str());
    name += std::to_string(store_id);
    name += "$";
  }
  name += std::to_string(index);
  name += ";";
  auto nopper_type = DexType::make_type(name);
  auto res = nopper_impl::create_auxiliary_defs(nopper_type);
  store->get_dexen().at(index).push_back(res.cls);
  res.cls->add_field(res.int_field);
  res.cls->add_method(res.clinit);
  res.cls->add_method(res.fib_method);
  return res;
}

} // namespace

void NopperPass::eval_pass(DexStoresVector&, ConfigFiles&, PassManager& mgr) {
  if (m_probability == 0 || !m_complex) {
    return;
  }

  m_reserved_refs_handle = mgr.reserve_refs(name(),
                                            ReserveRefsInfo(/* frefs */ 1,
                                                            /* trefs */ 2,
                                                            /* mrefs */ 2));
}

void NopperPass::run_pass(DexStoresVector& stores,
                          ConfigFiles& conf,
                          PassManager& mgr) {
  mgr.record_running_nopper();

  if (m_probability == 0) {
    return;
  }

  always_assert(m_reserved_refs_handle);
  mgr.release_reserved_refs(*m_reserved_refs_handle);
  m_reserved_refs_handle = std::nullopt;

  auto scope = build_class_scope(stores);

  std::unordered_map<DexMethod*, const DexClasses*> method_to_dexes;
  std::unordered_map<const DexClasses*, nopper_impl::AuxiliaryDefs>
      auxiliary_defs;
  if (m_complex) {
    for (auto& store : stores) {
      const auto& dexen = store.get_dexen();
      for (size_t i = 0; i < dexen.size(); i++) {
        walk::code(dexen[i], [&](DexMethod* method, IRCode&) {
          method_to_dexes.emplace(method, &dexen[i]);
        });
        auto ad = create_auxiliary_defs(&store, i);
        auxiliary_defs.emplace(&dexen[i], ad);
      }
    }
  }

  InsertOnlyConcurrentMap<DexMethod*, std::vector<cfg::Block*>>
      gathered_noppable_blocks;
  resources::RClassReader r_class_reader(conf.get_global_config());
  walk::parallel::code(scope, [&](DexMethod* method, IRCode&) {
    if (m_complex) {
      if (r_class_reader.is_r_class(type_class(method->get_class()))) {
        // The NopperPass may run before certain resource optimizations, and we
        // don't want to interfere with or degrade them.
        return;
      }
      auto ii = InstructionIterable(method->get_code()->cfg());
      if (std::any_of(ii.begin(), ii.end(), [](auto& mie) {
            return opcode::is_a_monitor(mie.insn->opcode());
          })) {
        // Monitor instruction have special rules on where we can insert
        // throwing code around them, so we avoid this complication altogether.
        return;
      }
    }
    gathered_noppable_blocks.emplace(
        method, nopper_impl::get_noppable_blocks(method->get_code()->cfg()));
  });

  std::vector<std::pair<DexMethod*, cfg::Block*>> noppable_blocks_vec;
  for (auto&& [method, blocks] : gathered_noppable_blocks) {
    for (auto* block : blocks) {
      noppable_blocks_vec.emplace_back(method, block);
    }
  }
  gathered_noppable_blocks.clear();

  std::mt19937 gen;
  gen.seed(0);
  std::shuffle(noppable_blocks_vec.begin(), noppable_blocks_vec.end(), gen);
  auto end = (size_t)std::lround(noppable_blocks_vec.size() * m_probability);
  noppable_blocks_vec.resize(end);

  std::unordered_map<DexMethod*, std::unordered_set<cfg::Block*>>
      noppable_blocks;
  for (auto&& [method, block] : noppable_blocks_vec) {
    noppable_blocks[method].insert(block);
  }

  std::atomic<size_t> nops_inserted{0};
  std::atomic<size_t> blocks{0};
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    blocks.fetch_add(code.cfg().num_blocks());
    auto it = noppable_blocks.find(method);
    if (it == noppable_blocks.end()) {
      return;
    }
    nopper_impl::AuxiliaryDefs* ad{nullptr};
    if (m_complex) {
      ad = &auxiliary_defs.at(method_to_dexes.at(method));
    }
    auto local_nops_inserted =
        nopper_impl::insert_nops(code.cfg(), it->second, ad);
    nops_inserted.fetch_add(local_nops_inserted);
  });

  mgr.set_metric("nops_inserted", nops_inserted.load());
  mgr.set_metric("blocks", blocks.load());
  TRACE(NOP,
        1,
        "%zu nops_inserted across %zu blocks",
        nops_inserted.load(),
        blocks.load());
}

static NopperPass s_pass;
