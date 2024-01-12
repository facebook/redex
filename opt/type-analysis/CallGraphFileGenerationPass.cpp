/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CallGraphFileGenerationPass.h"

#include <fstream>

#include "CallGraph.h"
#include "ConcurrentContainers.h"
#include "ConfigFiles.h"
#include "MethodOverrideGraph.h"
#include "Show.h"
#include "Walkers.h"

namespace mog = method_override_graph;

namespace {

void gather_cg_information(
    const call_graph::Graph& cg,
    std::vector<call_graph::NodeId>* nodes,
    std::unordered_map<call_graph::NodeId, uint32_t>* nodes_to_ids,
    std::unordered_map<call_graph::NodeId, std::set<uint32_t>>*
        nodes_to_succs) {
  uint32_t method_num = 0;
  std::queue<call_graph::NodeId> node_queue;
  node_queue.push(cg.entry());
  auto add_node = [&nodes, &nodes_to_ids, &method_num](const auto& node) {
    // We don't write method <-> id information in binary file.
    // We write nodes vector in sequence and use the sequence to infer method
    // id, so idx of node in nodes vector should equal to its id.
    if (!nodes_to_ids->count(node)) {
      (*nodes_to_ids)[node] = method_num;
      nodes->emplace_back(node);
      ++method_num;
    }
  };
  while (!node_queue.empty()) {
    auto cur_node = node_queue.front();
    node_queue.pop();
    if (nodes_to_succs->count(cur_node)) {
      continue;
    }
    (*nodes_to_succs)[cur_node] = std::set<uint32_t>{};
    add_node(cur_node);
    for (const auto& edge : cur_node->callees()) {
      auto callee_node = edge->callee();
      node_queue.push(callee_node);
      add_node(callee_node);
      auto callee_id = nodes_to_ids->at(callee_node);
      (*nodes_to_succs)[cur_node].emplace(callee_id);
    }
  }
  auto cg_stats = call_graph::get_num_nodes_edges(cg);
  always_assert(cg_stats.num_nodes == nodes->size());
}

void gather_method_positions(
    const Scope& scope,
    ConcurrentMap<DexMethod*, std::string>* method_to_first_position) {
  walk::parallel::code(
      scope, [&method_to_first_position](DexMethod* method, IRCode& code) {
        for (const MethodItemEntry& mie : code) {
          if (mie.type == MFLOW_POSITION) {
            auto pos = mie.pos.get();
            std::ostringstream o;
            o << "{";
            if (pos->file == nullptr) {
              o << "Unknown source";
            } else {
              o << *pos->file;
            }
            o << ":" << pos->line << "}";
            method_to_first_position->emplace(method, o.str());
            break;
          }
        }
      });
}
/*
 * Binary file format:
 * magic number 0xfaceb000 (4 byte)
 * version number (4 byte)
 * number (m) of nodes (4 byte)
 * m* [ string size (4 byte), string (node descriptor), number (m) of succ
 *      edge (4 byte), n * [id of succ ((4 byte))] ]
 */
void write_out_callgraph(const Scope& scope,
                         const call_graph::Graph& cg,
                         const std::string& callgraph_filename) {
  std::vector<call_graph::NodeId> nodes;
  std::unordered_map<call_graph::NodeId, uint32_t> nodes_to_ids;
  std::unordered_map<call_graph::NodeId, std::set<uint32_t>> nodes_to_succs;
  gather_cg_information(cg, &nodes, &nodes_to_ids, &nodes_to_succs);
  ConcurrentMap<DexMethod*, std::string> method_to_first_position;
  gather_method_positions(scope, &method_to_first_position);
  size_t bit_32_size = sizeof(uint32_t);
  always_assert(nodes.size() <= std::numeric_limits<uint32_t>::max());
  uint32_t num_method = nodes.size();
  std::ofstream ofs(callgraph_filename.c_str(),
                    std::ofstream::out | std::ofstream::trunc);
  uint32_t magic = 0xfaceb000; // serves as endianess check
  ofs.write((const char*)&magic, bit_32_size);
  uint32_t version = 1;
  ofs.write((const char*)&version, bit_32_size);
  ofs.write((const char*)&num_method, bit_32_size);
  uint32_t cur_id = 0;
  for (const auto& node : nodes) {
    always_assert_log(cur_id == nodes_to_ids.at(node), "Node id mismatch");
    ++cur_id;
    std::string node_name;
    if (node->is_entry()) {
      node_name = "ENTRY{ENTRY}";
    } else if (node->is_exit()) {
      node_name = "EXIT{EXIT}";
    } else {
      DexMethod* method = const_cast<DexMethod*>(node->method());
      node_name = show(method);
      if (method->is_external()) {
        node_name += "{EXTERNAL}";
      } else if (is_native(method)) {
        node_name += "{NATIVE}";
      } else if (!method->get_code()) {
        node_name += "{NOCODE}";
      } else {
        node_name += (method_to_first_position.count(method)
                          ? method_to_first_position.at(method)
                          : "{NOPOSITION}");
      }
    }
    uint32_t ssize = node_name.size();
    ofs.write((const char*)&ssize, bit_32_size);
    ofs << node_name;
    const auto& succs = nodes_to_succs.at(node);
    always_assert(succs.size() <= std::numeric_limits<uint32_t>::max());
    uint32_t num_succs = succs.size();
    ofs.write((const char*)&num_succs, bit_32_size);
    for (auto succ : succs) {
      ofs.write((const char*)&succ, bit_32_size);
    }
  }
}
} // namespace

void CallGraphFileGenerationPass::bind_config() {
  bind("emit_graph", false, m_emit_graph);
}

void CallGraphFileGenerationPass::run_pass(DexStoresVector& stores,
                                           ConfigFiles& conf,
                                           PassManager& /* unused */) {
  if (!m_emit_graph) {
    return;
  }
  auto scope = build_class_scope(stores);
  auto method_override_graph = mog::build_graph(scope);
  const std::string& callgraph_filename = conf.metafile(CALL_GRAPH_FILE);
  call_graph::Graph cg =
      call_graph::complete_call_graph(*method_override_graph, scope);
  write_out_callgraph(scope, cg, callgraph_filename);
}
static CallGraphFileGenerationPass s_pass;
