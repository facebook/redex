/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

mod common;

use common::graph::SimpleGraph;
use sparta::graph::ReverseGraph;
use sparta::graph::SuccessorNodes;

fn create_graph() -> SimpleGraph {
    let mut g = SimpleGraph::new(1, 5);
    g.add_edge(1, 2);
    g.add_edge(1, 3);
    g.add_edge(3, 4);
    g.add_edge(2, 5);
    g.add_edge(4, 5);
    g
}

#[test]
fn test_graph_successors() {
    let g = create_graph();

    let mut succ_nodes = g.get_succ_nodes(1);
    succ_nodes.sort();
    assert_eq!(succ_nodes.into_vec(), vec![2, 3]);

    let mut succ_nodes = g.get_succ_nodes(4);
    succ_nodes.sort();
    assert_eq!(succ_nodes.into_vec(), vec![5]);
}

#[test]
fn test_reversed_graph() {
    let graph = create_graph();
    let reversed_g = graph.into_rev();

    let mut succ_nodes = reversed_g.get_succ_nodes(5);
    succ_nodes.sort();
    assert_eq!(succ_nodes.into_vec(), vec![2, 4]);

    let mut succ_nodes = reversed_g.get_succ_nodes(1);
    succ_nodes.sort();
    assert_eq!(succ_nodes.into_vec(), vec![]);
}
