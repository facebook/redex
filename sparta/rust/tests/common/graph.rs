/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::collections::BTreeMap;
use std::collections::BTreeSet;
use std::collections::HashMap;
use std::collections::HashSet;
use std::iter::FromIterator;

use smallvec::SmallVec;
use sparta::graph::Graph;
use sparta::graph::DEFAULT_GRAPH_SUCCS_NUM;

type NodeId = u32;
type EdgeId = u32;

pub struct Edge(NodeId, NodeId);

// A naive graph for testing.
#[derive(Default)]
pub struct SimpleGraph {
    // We must use BTreeSet here to make the test consistent (we assumed
    // the order of successor edges in WPO tests.)
    edges: HashMap<NodeId, BTreeSet<EdgeId>>,
    pred_edges: BTreeMap<NodeId, BTreeSet<EdgeId>>,
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

    fn size(&self) -> usize {
        // Ideally the graph structure should maintain some stats of nodes and edges,
        // but for this testing code let's keep it simple. Note that we only provide
        // function `add_edge` to record edges, we don't keep information of nodes
        // separately, so this function will not give correct answer for graph with only
        // nodes but not edges.
        let mut nodes: HashSet<NodeId> = HashSet::from_iter(self.edges.keys().copied());
        nodes.extend(self.pred_edges.keys().copied());
        nodes.len()
    }
}
