/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::collections::HashMap;
use std::collections::HashSet;

use smallvec::SmallVec;
use sparta::graph::Graph;
use sparta::graph::SuccessorNodes;
use sparta::graph::DEFAULT_GRAPH_SUCCS_NUM;

type NodeId = u32;
type EdgeId = u32;

pub struct Edge(NodeId, NodeId);

// A naive graph for testing.
#[derive(Default)]
pub struct SimpleGraph {
    edges: HashMap<NodeId, HashSet<EdgeId>>,
    pred_edges: HashMap<NodeId, HashSet<EdgeId>>,
    edge_interner: Vec<Edge>,
    enter: NodeId,
    exit: NodeId,
}

impl SimpleGraph {
    pub fn new(enter: NodeId, exit: NodeId) -> Self {
        Self {
            enter,
            exit,
            ..Default::default()
        }
    }

    pub fn add_edge(&mut self, source: NodeId, target: NodeId) {
        // Duplicate edges will be counted too.
        self.edge_interner.push(Edge(source, target));
        let edge_id = self.edge_interner.len() - 1;
        self.edges.entry(source).or_default().insert(edge_id as u32);
        self.pred_edges
            .entry(target)
            .or_default()
            .insert(edge_id as u32);
    }
}

impl Graph for SimpleGraph {
    type NodeId = NodeId;
    type EdgeId = EdgeId;

    fn entry(&self) -> Self::NodeId {
        self.enter
    }

    fn exit(&self) -> Self::NodeId {
        self.exit
    }

    // The edge is in the format of (source, target), so the predecessor
    // is still edge.0 instead of edge.1.
    fn predecessors(&self, n: Self::NodeId) -> SmallVec<[Self::EdgeId; DEFAULT_GRAPH_SUCCS_NUM]> {
        self.pred_edges
            .get(&n)
            .map(|v| v.iter().copied().collect())
            .unwrap_or_else(|| SmallVec::new())
    }

    fn successors(&self, n: Self::NodeId) -> SmallVec<[Self::EdgeId; DEFAULT_GRAPH_SUCCS_NUM]> {
        self.edges
            .get(&n)
            .map(|v| v.iter().copied().collect())
            .unwrap_or_else(|| SmallVec::new())
    }

    fn source(&self, e: Self::EdgeId) -> Self::NodeId {
        self.edge_interner[e as usize].0
    }

    fn target(&self, e: Self::EdgeId) -> Self::NodeId {
        self.edge_interner[e as usize].1
    }
}

impl SuccessorNodes for SimpleGraph {
    type NodeId = NodeId;

    fn get_succ_nodes(&self, n: NodeId) -> SmallVec<[Self::NodeId; DEFAULT_GRAPH_SUCCS_NUM]> {
        self.successors(n)
            .iter()
            .map(move |&edge_idx| self.edge_interner[edge_idx as usize].1)
            .collect()
    }
}
