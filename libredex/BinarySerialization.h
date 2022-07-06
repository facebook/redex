/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/bimap/bimap.hpp>
#include <boost/bimap/unordered_set_of.hpp>
#include <limits>
#include <ostream>
#include <type_traits>

#include "Debug.h"

/*
 * Utilities for serializing binary data.
 */

namespace binary_serialization {

template <class V>
std::enable_if_t<std::is_integral<V>::value> write(std::ostream& os,
                                                   const V& value) {
  os.write((const char*)&value, sizeof(value));
}

/*
 * Serialize an array by emitting its length first, followed by the elements
 * in the array.
 */
template <class Container>
void write_array(std::ostream& os, const Container& container) {
  always_assert(container.size() <= std::numeric_limits<uint32_t>::max());
  write<uint32_t>(os, container.size());
  for (const auto& item : container) {
    write(os, item);
  }
}

/*
 * Write a simple header. Ideally we should use a single header format across
 * all our binary files.
 */
inline void write_header(std::ostream& os, uint32_t version) {
  uint32_t magic = 0xfaceb000; // serves as endianess check
  write(os, magic);
  write(os, version);
}

/*
 * Serialize a graph as an adjacency list. For a graph with N nodes, we will
 * emit N lines of the form
 *
 *   <serialized label for node><E1><E2>...<Em>
 *
 * The node on line n has ID n. E1 ... Em are the IDs of its neighbors.
 */
template <class Node, class NodeHash = std::hash<Node>>
class GraphWriter {

  struct Id {};

  using NodeIdBiMap = boost::bimaps::bimap<
      boost::bimaps::unordered_set_of<boost::bimaps::tagged<Node, Node>,
                                      NodeHash>,
      boost::bimaps::unordered_set_of<boost::bimaps::tagged<uint32_t, Id>>>;

  using NodeIdPair = typename NodeIdBiMap::value_type;
  using SuccessorFunction = std::function<std::vector<Node>(const Node&)>;
  using NodeWriter = std::function<void(std::ostream&, const Node&)>;

 public:
  /*
   * node_writer is responsible for generating the label for each node.
   */
  GraphWriter(NodeWriter node_writer, SuccessorFunction successors)
      : m_node_writer(node_writer), m_successors(successors) {}

  template <class NodeContainer>
  void write(std::ostream& os, const NodeContainer& nodes) {
    // Give each node a unique ID.
    for (const auto& node : nodes) {
      number_node_recursive(node);
    }
    // Emit the node label and adjacency list.
    uint32_t nodes_count = m_node_ids.size();
    binary_serialization::write(os, nodes_count);
    for (uint32_t i = 0; i < nodes_count; ++i) {
      const auto& node = m_node_ids.template by<Id>().at(i);
      m_node_writer(os, node);
      std::vector<uint32_t> succ_ids;
      for (const auto& succ : m_successors(node)) {
        succ_ids.emplace_back(m_node_ids.template by<Node>().at(succ));
      }
      write_array(os, succ_ids);
    }
  }

 private:
  void number_node_recursive(const Node& node) {
    if (m_node_ids.template by<Node>().count(node)) {
      return;
    }
    m_node_ids.insert(NodeIdPair(node, m_node_ids.size()));
    for (const auto& succ : m_successors(node)) {
      number_node_recursive(succ);
    }
  }

  NodeIdBiMap m_node_ids;
  NodeWriter m_node_writer;
  SuccessorFunction m_successors;
};

} // namespace binary_serialization
