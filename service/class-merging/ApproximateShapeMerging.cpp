/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ApproximateShapeMerging.h"

#include <algorithm>
#include <climits>

#include "ConfigFiles.h"

using namespace class_merging;

using Shape = MergerType::Shape;

namespace {

const std::string SHAPE_GRAPH_FILE = "approx_shape_graph.dot";
const std::string SHAPE_MERGE_GRAPH_FILE = "approx_shape_merge_graph_file.dot";

/**
 * A "distance" is defined only between two shapes of which one includes the
 * other. It is the difference of number fields. It is not a distance in a
 * mathematical sense.
 */
int distance(const Shape& lhs, const Shape& rhs) {
  always_assert(lhs.includes(rhs));
  return ((lhs.string_fields - rhs.string_fields) +
          (lhs.reference_fields - rhs.reference_fields) +
          (lhs.bool_fields - rhs.bool_fields) +
          (lhs.int_fields - rhs.int_fields) +
          (lhs.long_fields - rhs.long_fields) +
          (lhs.double_fields - rhs.double_fields) +
          (lhs.float_fields - rhs.float_fields));
}

/**
 * Merge two shapes
 **/
void merge_shapes(const Shape& from_shape,
                  const Shape& to_shape,
                  MergerType::ShapeCollector& shapes) {
  always_assert(from_shape.field_count() < to_shape.field_count());
  always_assert(to_shape.includes(from_shape));
  auto from_it = shapes.find(from_shape);
  auto to_it = shapes.find(to_shape);

  if (from_it != shapes.end() && to_it != shapes.end()) {
    // Make sure shapes have not been broken into groups yet.
    always_assert(from_it->second.groups.empty());
    always_assert(to_it->second.groups.empty());
    TRACE(CLMG, 9, "            - Merge shape %s into %s",
          from_it->first.to_string().c_str(), to_it->first.to_string().c_str());
    to_it->second.types.insert(from_it->second.types.begin(),
                               from_it->second.types.end());
    shapes.erase(from_it);
    always_assert(shapes.find(from_shape) == shapes.end());
  }
}

/**
 * Build shape DAG.
 * The DAG represents the `be included` relationship between shapes: each vertex
 * is a shape and an edge A -> B means B includes A.
 *
 * `mergeable_count` is the total
 * mergeables of a shape A and its immediate predecessors. This is the mergeable
 * count if all its predecessors are merged into it.
 */
void build_DAG(MergerType::ShapeCollector& shapes,
               size_t max_distance,
               std::unordered_map<Shape, std::unordered_set<Shape>>& pred_map,
               std::unordered_map<Shape, std::unordered_set<Shape>>& succ_map,
               std::unordered_map<Shape, size_t>& mergeable_count) {
  TRACE(CLMG, 5, "[approx] Building Shape DAG");
  for (const auto& lhs : shapes) {
    for (const auto& rhs : shapes) {
      if (lhs.first == rhs.first) {
        continue;
      }
      if (lhs.first.includes(rhs.first)) {
        size_t dist = distance(lhs.first, rhs.first);
        if (dist > max_distance) {
          continue;
        }
        TRACE(CLMG, 9, "         - Edge: %s -> %s, dist = %zu",
              rhs.first.to_string().c_str(), lhs.first.to_string().c_str(),
              dist);
        // if lhs shape includes rhs, add lhs to rhs's succ_map
        succ_map[rhs.first].insert(lhs.first);
        // and add rhs to lhs's pred_map
        pred_map[lhs.first].insert(rhs.first);
        // initialize mergeable counts
        if (mergeable_count.find(lhs.first) == mergeable_count.end()) {
          mergeable_count[lhs.first] = lhs.second.types.size();
        }
        mergeable_count[lhs.first] += rhs.second.types.size();
      }
    }
  }
}

/**
 * Remove a node from DAG
 */
void remove_from_DAG(
    const Shape& shape,
    std::unordered_map<Shape, std::unordered_set<Shape>>& pred_map,
    std::unordered_map<Shape, std::unordered_set<Shape>>& succ_map) {
  // remove this node from the predecessor lists of its successors
  if (succ_map.find(shape) != succ_map.end()) {
    for (const auto& succ : succ_map[shape]) {
      always_assert(pred_map.find(succ) != pred_map.end());
      always_assert(pred_map[succ].find(shape) != pred_map[succ].end());
      pred_map[succ].erase(shape);
    }
    succ_map.erase(shape);
  }
  // remove this node from successor lists of its predecessors
  if (pred_map.find(shape) != pred_map.end()) {
    for (const auto& pred : pred_map[shape]) {
      always_assert(succ_map.find(pred) != succ_map.end());
      always_assert(succ_map[pred].find(shape) != succ_map[pred].end());
      succ_map[pred].erase(shape);
    }
    pred_map.erase(shape);
  }
}

enum DFSStatus { Unvisited = 0, Partial, Visited };

/**
 * (reverse) Topological sort of shape DAG using DFS
 */
void topological_sort_visit(
    const Shape& shape,
    const std::unordered_map<Shape, std::unordered_set<Shape>>& succ_map,
    std::unordered_map<Shape, DFSStatus>& visited,
    std::vector<Shape>& sorted) {
  // assert that there is no cycle
  always_assert(visited[shape] != Partial);
  if (visited[shape] == Visited) {
    return;
  }
  // mark this shape temporarily
  visited[shape] = Partial;
  if (succ_map.find(shape) != succ_map.end()) {
    for (const auto& succ : succ_map.at(shape)) {
      topological_sort_visit(succ, succ_map, visited, sorted);
    }
  }
  // mark permanently
  visited[shape] = Visited;
  sorted.push_back(shape);
}

std::vector<Shape> topological_sort(
    const std::vector<Shape>& shape_list,
    const std::unordered_map<Shape, std::unordered_set<Shape>>& succ_map) {
  std::vector<Shape> sorted;
  std::unordered_map<Shape, DFSStatus> visited;
  for (const auto& shape : shape_list) {
    visited[shape] = Unvisited;
  }

  for (const auto& shape : shape_list) {
    if (visited[shape] == Unvisited) {
      topological_sort_visit(shape, succ_map, visited, sorted);
    }
  }
  return sorted;
}

/**
 * Drop shapes with large number of mergeables.
 *
 * Remove all outgoing edges if a shape has more than a given threshold
 * mergeables. This disallows such shape being merged into other shapes, while
 * still allows other shapes merge into it.
 */
void drop_shape_with_many_mergeables(
    const size_t threshold,
    const MergerType::ShapeCollector& shapes,
    std::unordered_map<Shape, std::unordered_set<Shape>>& pred_map,
    std::unordered_map<Shape, std::unordered_set<Shape>>& succ_map) {
  for (const auto& shape_it : shapes) {
    if (shape_it.second.types.size() > threshold &&
        succ_map.find(shape_it.first) != succ_map.end()) {
      TRACE(CLMG, 7,
            "         shape %s has %zu mergeables > %zu, can't merge it into "
            "others",
            shape_it.first.to_string().c_str(), shape_it.second.types.size(),
            threshold);
      // Remove its outgoing edges
      for (const auto& succ : succ_map.at(shape_it.first)) {
        // Remove this node from its successors' predecessor lists
        always_assert(pred_map.find(succ) != pred_map.end());
        always_assert(pred_map.at(succ).find(shape_it.first) !=
                      pred_map.at(succ).end());
        pred_map.at(succ).erase(shape_it.first);
        if (pred_map.at(succ).empty()) {
          pred_map.erase(succ);
        }
      }
      succ_map.erase(shape_it.first);
    }
  }
}

/**
 * Write the shape graph in Graphviz dot format
 **/
void print_edge(const Shape& from_shape,
                const Shape& to_shape,
                const std::unordered_map<Shape, size_t>& num_mergeables,
                std::ostream& os) {
  always_assert(num_mergeables.find(from_shape) != num_mergeables.end() &&
                num_mergeables.find(to_shape) != num_mergeables.end());

  os << "    "
     << "\"" << from_shape.to_string() << "\\n"
     << num_mergeables.at(from_shape) << "\""
     << " -> "
     << "\"" << to_shape.to_string() << "\\n"
     << num_mergeables.at(to_shape) << "\""
     << "  [label=\"dist=" << distance(to_shape, from_shape) << "\"]"
     << std::endl;
}

void write_shape_graph(
    const ConfigFiles& conf,
    const std::string& graph_file_name,
    const std::unordered_map<Shape, std::unordered_set<Shape>>& pred_map,
    const std::unordered_map<Shape, size_t>& num_mergeables) {
  std::string file_name = conf.metafile(graph_file_name);
  std::ofstream os(file_name, std::ios::app);
  if (!os.is_open()) {
    TRACE(CLMG, 5, "         Cannot open file.");
    return;
  }
  os << "digraph G {" << std::endl;
  for (const auto& it : pred_map) {
    for (const auto& from_shape : it.second) {
      print_edge(from_shape, it.first, num_mergeables, os);
    }
  }
  os << "}" << std::endl;
  os.close();
}

} // namespace

namespace class_merging {

/**
 * A very simple greedy algorithm to merge shapes.
 *
 * We define a distance between two shapes as the difference of total number of
 * fields. This algorithm sorts shapes by their total number of fields and
 * greedily merges shapes as long as they are within a user-defined maximum
 * distance. Each shape can be merged at most once.
 */
void simple_greedy_approximation(const JsonWrapper& specs,
                                 MergerType::ShapeCollector& shapes,
                                 ApproximateStats& stats) {
  size_t max_distance;
  specs.get("distance", 0, max_distance);
  TRACE(CLMG, 3, "[approx] Using simple greedy algorithm.");
  TRACE(CLMG, 3, "         distance = %ld.", max_distance);

  // Sort shapes by the number of fields.
  std::vector<Shape> shapes_list;
  for (const auto& shape_it : shapes) {
    shapes_list.push_back(shape_it.first);
  }
  std::sort(shapes_list.begin(), shapes_list.end(),
            [&](const Shape& lhs, const Shape& rhs) {
              return lhs.field_count() > rhs.field_count();
            });

  TRACE(CLMG, 3, "[approx] Finding approximation:");
  // From the beginining of the list, try all pairs of shapes.
  while (!shapes_list.empty()) {
    Shape s0 = shapes_list.front();
    shapes_list.erase(shapes_list.begin());

    auto it = shapes_list.begin();
    while (it != shapes_list.end()) {
      if (s0.includes(*it)) {
        size_t dist = distance(s0, *it);
        if (dist > max_distance) {
          ++it;
          continue;
        }

        always_assert(shapes.find(*it) != shapes.end());
        TRACE(CLMG, 9, "          - distance between %s and %s = %zu",
              s0.to_string().c_str(), it->to_string().c_str(), dist);
        stats.shapes_merged++;
        stats.mergeables += shapes[*it].types.size();
        stats.fields_added += shapes[*it].types.size() * dist;
        merge_shapes(*it, s0, shapes);
        it = shapes_list.erase(it);
      } else {
        ++it;
      }
    }
  }
}

/**
 * Greedily select a group of shapes to merge together such that total
 * mergeables in that group is maximized.
 */
void max_mergeable_greedy(const JsonWrapper& specs,
                          const ConfigFiles& conf,
                          MergerType::ShapeCollector& shapes,
                          ApproximateStats& stats) {
  size_t max_distance;
  specs.get("distance", 0, max_distance);
  size_t max_mergeable_threshold = 0;
  specs.get("max_mergeable_threshold", 0, max_mergeable_threshold);

  TRACE(CLMG, 3, "[approx] Using max-mergeable greedy algorithm.");
  std::unordered_map<Shape, std::unordered_set<Shape>> succ_map;
  std::unordered_map<Shape, std::unordered_set<Shape>> pred_map;
  // mergealbe_count[A] = mergeables in shape A + the sum of mergeables in all
  //                      predecessors of A

  // The difference is mergeable_count contains the sum of number of mergeables
  // from the shape itself and all its predecessors. whereas num_mergeables
  // contains the number of mergeables only from the shape itself.
  std::unordered_map<Shape, size_t> mergeable_count;
  std::unordered_map<Shape, size_t> num_mergeables;

  for (const auto& shape_it : shapes) {
    num_mergeables[shape_it.first] = shape_it.second.types.size();
  }

  build_DAG(shapes, max_distance, pred_map, succ_map, mergeable_count);

  if (max_mergeable_threshold > 0) {
    drop_shape_with_many_mergeables(max_mergeable_threshold, shapes, pred_map,
                                    succ_map);
  }
  write_shape_graph(conf, SHAPE_GRAPH_FILE, pred_map, num_mergeables);

  // Get a list of target shapes that has predecessors
  std::vector<Shape> target_list;
  target_list.reserve(pred_map.size());
  for (const auto& s_pair : pred_map) {
    target_list.push_back(s_pair.first);
  }

  std::unordered_map<Shape, std::unordered_set<Shape>> merge_map;

  // Greedily select a group of shapes that can be merged into one
  // (target_shape) in terms of total mergeable count in that group. target_list
  // acts as a priority queue. Because std::priority_queue does not support
  // priority updates, we have to implement one.
  while (!target_list.empty()) {
    // Find the target shape with the largest mergeable_count.
    auto max_elem =
        std::max_element(target_list.begin(), target_list.end(),
                         [&](const Shape& lhs, const Shape& rhs) {
                           return mergeable_count[lhs] < mergeable_count[rhs];
                         });

    Shape to_shape = *max_elem;
    target_list.erase(max_elem);

    if (pred_map.find(to_shape) == pred_map.end()) {
      continue;
    }

    TRACE(CLMG, 5, "        Merging %zu mergeables into one shape",
          mergeable_count[to_shape]);

    // Update mergeable_count of to_shape's successors
    if (succ_map.find(to_shape) != succ_map.end()) {
      for (const auto& succ : succ_map[to_shape]) {
        mergeable_count[succ] -= shapes[to_shape].types.size();
      }
    }

    // Merge all its predecessors into it
    while (!pred_map[to_shape].empty()) {
      Shape from_shape = *pred_map[to_shape].begin();
      // Before merging from_shape into to_shape, remove from_shape from the
      // DAG.
      always_assert(succ_map.find(from_shape) != succ_map.end());
      for (const auto& succ : succ_map[from_shape]) {
        mergeable_count[succ] -= shapes[from_shape].types.size();
      }
      remove_from_DAG(from_shape, pred_map, succ_map);
      // stats
      stats.shapes_merged++;
      stats.mergeables += shapes[from_shape].types.size();
      stats.fields_added +=
          shapes[from_shape].types.size() * distance(to_shape, from_shape);
      // Actual merge
      merge_map[to_shape].insert(from_shape);
      merge_shapes(from_shape, to_shape, shapes);
    }
    // Remove to_shape from the DAG
    remove_from_DAG(to_shape, pred_map, succ_map);
  }

  write_shape_graph(conf, SHAPE_MERGE_GRAPH_FILE, merge_map, num_mergeables);
}

/**
 * A greedy algorithm that merges the most number of shapes
 *
 * We first build a DAG in the same way as the max_mergeable_greedy algorithm.
 * The list of shapes are then topologically sorted according to the DAG. In the
 * sorted order, merge a shape to a successor with the most predecessors.
 */
void max_shape_merged_greedy(const JsonWrapper& specs,
                             const ConfigFiles& conf,
                             MergerType::ShapeCollector& shapes,
                             ApproximateStats& stats) {
  size_t max_distance;
  specs.get("distance", 0, max_distance);
  size_t max_mergeable_threshold = 0;
  specs.get("max_mergeable_threshold", 0, max_mergeable_threshold);

  TRACE(CLMG, 3, "[approx] Using max-shape-merged greedy algorithm.");
  std::unordered_map<Shape, std::unordered_set<Shape>> succ_map;
  std::unordered_map<Shape, std::unordered_set<Shape>> pred_map;
  std::unordered_map<Shape, size_t> mergeable_count;
  std::unordered_map<Shape, size_t> num_mergeables;

  for (const auto& shape_it : shapes) {
    num_mergeables[shape_it.first] = shape_it.second.types.size();
  }

  build_DAG(shapes, max_distance, pred_map, succ_map, mergeable_count);

  if (max_mergeable_threshold > 0) {
    drop_shape_with_many_mergeables(max_mergeable_threshold, shapes, pred_map,
                                    succ_map);
  }
  write_shape_graph(conf, SHAPE_GRAPH_FILE, pred_map, num_mergeables);

  std::vector<Shape> shapes_list;
  for (const auto& shape_it : shapes) {
    shapes_list.push_back(shape_it.first);
  }

  // Topological sort of the shapes based on the DAG. Note that the
  // topological_sort is reversed sort, we pass pred_map instead of succ_map to
  // get the right order
  std::vector<Shape> sorted_list = topological_sort(shapes_list, pred_map);

  // Indicates a shape has been merged into.
  std::unordered_set<Shape> merged;
  std::unordered_map<Shape, std::unordered_set<Shape>> merge_map;

  // Process shapes in the topological order.
  for (auto it = sorted_list.begin(); it != sorted_list.end(); ++it) {
    if (merged.count(*it) > 0) {
      remove_from_DAG(*it, pred_map, succ_map);
      continue;
    }
    /**
     * The heuristic is to find a target shape with the most predecessors. For
     * example:
     *
     *     +-->D<--+    E
     *     |   ^   |    ^
     *     |   |   |    |
     *     +   +   +    |
     *     A   B   C+---+
     *
     * If we want to choose a target for C to merge into, take D instead of E
     * since D has more predecessors.
     */
    if (succ_map.find(*it) != succ_map.end()) {
      auto max = std::max_element(
          succ_map.at(*it).begin(), succ_map.at(*it).end(),
          [&](const Shape& a, const Shape& b) {
            size_t l =
                pred_map.find(a) == pred_map.end() ? 0 : pred_map.at(a).size();
            size_t r =
                pred_map.find(b) == pred_map.end() ? 0 : pred_map.at(b).size();
            return l < r;
          });
      // Stats
      stats.shapes_merged++;
      stats.mergeables += shapes[*it].types.size();
      stats.fields_added += shapes[*it].types.size() * distance(*max, *it);
      merge_map[*max].insert(*it);
      merge_shapes(*it, *max, shapes);
      merged.insert(*max);
    }
  }

  write_shape_graph(conf, SHAPE_MERGE_GRAPH_FILE, merge_map, num_mergeables);
}

} // namespace class_merging
