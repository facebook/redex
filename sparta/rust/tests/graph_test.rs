/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

mod common;

use common::graph::SimpleGraph;
use sparta::graph::SuccessorNodes;

#[test]
fn test_graph_successors() {
    let mut g = SimpleGraph::new(1, 5);
    g.add_edge(1, 2);
    g.add_edge(1, 3);
    g.add_edge(3, 4);
    g.add_edge(2, 5);
    g.add_edge(4, 5);

    let mut succ_nodes = g.get_succ_nodes(1);
    succ_nodes.sort();
    assert_eq!(succ_nodes.into_vec(), vec![2, 3]);

    let mut succ_nodes = g.get_succ_nodes(4);
    succ_nodes.sort();
    assert_eq!(succ_nodes.into_vec(), vec![5]);
}
