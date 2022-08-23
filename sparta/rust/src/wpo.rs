/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::collections::BTreeSet;
use std::collections::HashMap;
use std::collections::HashSet;
use std::fmt::Debug;
use std::hash::Hash;
use std::iter::FromIterator;

use petgraph::unionfind::UnionFind;

use crate::graph::SuccessorNodes;

pub type WpoIdx = u32;

pub struct WpoNodeData<NodeId: Copy + Hash + Ord> {
    node: NodeId,
    size: usize,
    successors: BTreeSet<WpoIdx>,
    predessors: BTreeSet<WpoIdx>,
    num_outer_preds: HashMap<WpoIdx, u32>,
}

impl<NodeId> WpoNodeData<NodeId>
where
    NodeId: Copy + Hash + Ord,
{
    pub fn new(node: NodeId, size: usize) -> Self {
        Self {
            node,
            size,
            successors: BTreeSet::new(),
            predessors: BTreeSet::new(),
            num_outer_preds: HashMap::new(),
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum WpoNodeType {
    Head,
    Plain,
    Exit,
}

pub struct WpoNode<NodeId: Copy + Ord + Hash> {
    ty: WpoNodeType,
    data: WpoNodeData<NodeId>,
}

impl<NodeId> WpoNode<NodeId>
where
    NodeId: Copy + Ord + Hash + Debug,
{
    pub fn plain(node: NodeId, size: usize) -> Self {
        Self {
            ty: WpoNodeType::Plain,
            data: WpoNodeData::new(node, size),
        }
    }

    pub fn head(node: NodeId, size: usize) -> Self {
        Self {
            ty: WpoNodeType::Head,
            data: WpoNodeData::new(node, size),
        }
    }

    pub fn exit(node: NodeId, size: usize) -> Self {
        Self {
            ty: WpoNodeType::Exit,
            data: WpoNodeData::new(node, size),
        }
    }

    pub fn new(ty: WpoNodeType, node: NodeId, size: usize) -> Self {
        Self {
            ty,
            data: WpoNodeData::new(node, size),
        }
    }

    pub fn is_plain(&self) -> bool {
        self.ty == WpoNodeType::Plain
    }

    pub fn is_head(&self) -> bool {
        self.ty == WpoNodeType::Head
    }

    pub fn is_exit(&self) -> bool {
        self.ty == WpoNodeType::Exit
    }

    pub fn get_node(&self) -> NodeId {
        self.data.node
    }

    pub fn get_successors(&self) -> &BTreeSet<WpoIdx> {
        &self.data.successors
    }

    pub fn get_predecessors(&self) -> &BTreeSet<WpoIdx> {
        &self.data.predessors
    }

    pub fn get_num_preds(&self) -> usize {
        self.get_predecessors().len()
    }

    pub fn get_num_outer_preds(&self) -> &HashMap<WpoIdx, u32> {
        assert_eq!(
            self.ty,
            WpoNodeType::Exit,
            "Node {:#?} is not Exit",
            self.data.node
        );
        &self.data.num_outer_preds
    }

    pub fn get_size(&self) -> usize {
        self.data.size
    }

    fn add_successor(&mut self, idx: WpoIdx) {
        self.data.successors.insert(idx);
    }

    fn add_predecessor(&mut self, idx: WpoIdx) {
        self.data.predessors.insert(idx);
    }

    fn is_successor(&self, idx: WpoIdx) -> bool {
        self.get_successors().contains(&idx)
    }

    pub fn inc_num_outer_preds(&mut self, idx: WpoIdx) {
        assert_eq!(
            self.ty,
            WpoNodeType::Exit,
            "Node {:#?} is not Exit",
            self.data.node
        );
        *self.data.num_outer_preds.entry(idx).or_default() += 1;
    }
}

pub struct WeakPartialOrdering<NodeId: Copy + Hash + Ord> {
    /// All nodes under WPO.
    nodes: Vec<WpoNode<NodeId>>,
    /// All top level nodes.
    toplevel: Vec<WpoIdx>,
    /// Post depth first numbering for each node.
    post_dfn: HashMap<NodeId, u32>,
}

impl<NodeId> WeakPartialOrdering<NodeId>
where
    NodeId: Copy + Hash + Ord + Debug,
{
    pub fn new<SN>(root: NodeId, size: usize, successors_nodes: &SN) -> Self
    where
        SN: SuccessorNodes<NodeId = NodeId>,
    {
        if successors_nodes.get_succ_nodes(root).is_empty() {
            let mut wpo = Self {
                nodes: vec![],
                toplevel: vec![],
                post_dfn: HashMap::new(),
            };
            wpo.nodes.push(WpoNode::plain(root, 1));
            wpo.toplevel.push(0);
            wpo.post_dfn.insert(root, 1);
            wpo
        } else {
            WeakPartialOrderingImpl::new().build(size, root, successors_nodes)
        }
    }

    pub fn size(&self) -> usize {
        self.nodes.len()
    }

    pub fn get_entry(&self) -> WpoIdx {
        (self.nodes.len() - 1) as WpoIdx
    }

    pub fn get_successors(&self, idx: WpoIdx) -> &BTreeSet<WpoIdx> {
        self.nodes[idx as usize].get_successors()
    }

    pub fn get_predecessors(&self, idx: WpoIdx) -> &BTreeSet<WpoIdx> {
        self.nodes[idx as usize].get_predecessors()
    }

    pub fn get_num_preds(&self, idx: WpoIdx) -> usize {
        self.nodes[idx as usize].get_num_preds()
    }

    pub fn get_num_outer_preds(&self, exit: WpoIdx) -> &HashMap<WpoIdx, u32> {
        self.nodes[exit as usize].get_num_outer_preds()
    }

    pub fn get_head_of_exit(&self, exit: WpoIdx) -> WpoIdx {
        exit + 1
    }

    pub fn get_exit_of_head(&self, head: WpoIdx) -> WpoIdx {
        head - 1
    }

    pub fn get_node(&self, idx: WpoIdx) -> NodeId {
        self.nodes[idx as usize].get_node()
    }

    pub fn is_plain(&self, idx: WpoIdx) -> bool {
        self.nodes[idx as usize].is_plain()
    }

    pub fn is_head(&self, idx: WpoIdx) -> bool {
        self.nodes[idx as usize].is_head()
    }

    pub fn is_exit(&self, idx: WpoIdx) -> bool {
        self.nodes[idx as usize].is_exit()
    }

    pub fn is_from_outside(&self, head: NodeId, pred: NodeId) -> bool {
        self.get_post_dfn(head) < self.get_post_dfn(pred)
    }

    fn get_post_dfn(&self, n: NodeId) -> u32 {
        // If the key does not exist, meaning that node is not
        // finished yet, return default value 0.
        self.post_dfn.get(&n).copied().unwrap_or_default()
    }
}

// This private type is only used to build the actual WPO.
struct WeakPartialOrderingImpl<NodeId: Copy + Hash + Ord> {
    nodes: Vec<WpoNode<NodeId>>,
    toplevel: Vec<WpoIdx>,
    post_dfn: HashMap<NodeId, u32>,
    // A map from NodeId to post DFN.
    dfn: HashMap<NodeId, u32>,
    dfn_to_node: Vec<NodeId>,
    cross_fwd_edges: HashMap<u32, Vec<(u32, u32)>>,
    back_preds: HashMap<u32, Vec<u32>>,
    // Tree edges (map from node to its predecessors).
    non_back_preds: HashMap<u32, Vec<u32>>,
    next_dfn: u32,
    // Map from dfn to WpoIdx
    dfn_to_wpo_idx: Vec<WpoIdx>,
    // Next WpoIdx to assign
    next_idx: WpoIdx,
}

impl<NodeId> WeakPartialOrderingImpl<NodeId>
where
    NodeId: Copy + Hash + Ord + Debug,
{
    pub fn new() -> Self {
        // I really don't want to add `Default` bound to `NodeId`, so let's
        // have a bit tedious code here to give user side more flexibility.
        Self {
            next_dfn: 1u32,
            nodes: vec![],
            toplevel: vec![],
            post_dfn: HashMap::new(),
            dfn: HashMap::new(),
            dfn_to_node: vec![],
            cross_fwd_edges: HashMap::new(),
            back_preds: HashMap::new(),
            non_back_preds: HashMap::new(),
            dfn_to_wpo_idx: vec![],
            next_idx: 0,
        }
    }

    fn add_node(&mut self, dfn_i: u32, vertex: u32, sz: u32, ty: WpoNodeType) {
        self.dfn_to_wpo_idx[dfn_i as usize] = self.next_idx;
        self.next_idx += 1;
        self.nodes.push(WpoNode::new(
            ty,
            // dfn reserves 0, so should subtract 1 here.
            self.dfn_to_node[vertex as usize - 1],
            sz as usize,
        ));
    }

    fn node_of(&mut self, dfn_i: u32) -> &mut WpoNode<NodeId> {
        let idx = self.index_of(dfn_i) as usize;
        &mut self.nodes[idx]
    }

    fn index_of(&self, dfn_i: u32) -> u32 {
        self.dfn_to_wpo_idx[dfn_i as usize]
    }

    fn add_successor(
        &mut self,
        from: u32,
        to: u32,
        exit: u32,
        outer_pred: bool,
        for_outer_preds: &mut Vec<(WpoIdx, WpoIdx)>,
    ) {
        let from_idx = self.index_of(from);
        let to_idx = self.index_of(to);
        if !self.nodes[from_idx as usize].is_successor(to_idx) {
            if outer_pred {
                for_outer_preds.push((to_idx, self.index_of(exit)));
            }
            self.nodes[from_idx as usize].add_successor(to_idx);
            self.nodes[to_idx as usize].add_predecessor(from_idx);
        }
    }

    fn build_auxilary<SN>(&mut self, size: usize, root: NodeId, successors_nodes: &SN)
    where
        SN: SuccessorNodes<NodeId = NodeId>,
    {
        // Since 0 is reserved for undiscovered nodes, the total number of nodes
        // would be size + 1.
        let mut dft_dsets = UnionFind::<u32>::new(size + 1);
        let mut stack = Vec::new();
        let mut next_post_dfn = 1u32;
        let mut visited = HashMap::new();
        let mut ancestor = HashMap::new();

        let get_dfn = |n: NodeId, dfn: &HashMap<NodeId, u32>| {
            // If the key does not exist, meaning that node is not
            // discovered yet, return default value 0.
            dfn.get(&n).copied().unwrap_or_default()
        };
        let set_dfn = |n: NodeId, num: u32, dfn: &mut HashMap<NodeId, u32>| {
            dfn.insert(n, num);
        };

        stack.push((root, false, 0u32));

        while let Some((node, finished, pred)) = stack.pop() {
            if finished {
                self.post_dfn.insert(node, next_post_dfn);
                next_post_dfn += 1;

                let vertex = get_dfn(node, &self.dfn);
                visited.insert(vertex, true);

                dft_dsets.union(vertex, pred);
                ancestor.insert(dft_dsets.find_mut(pred), pred);
            } else {
                if get_dfn(node, &self.dfn) != 0 {
                    // Skip forward edges.
                    continue;
                }

                let vertex = self.next_dfn;
                self.next_dfn += 1;
                self.dfn_to_node.push(node);
                set_dfn(node, vertex, &mut self.dfn);
                ancestor.insert(vertex, vertex);

                stack.push((node, true, pred));

                let successors = successors_nodes.get_succ_nodes(node);
                for &succ_node in successors.iter().rev() {
                    let succ = get_dfn(succ_node, &self.dfn);
                    if 0 == succ {
                        stack.push((succ_node, false, vertex));
                    } else if visited.get(&succ).copied().unwrap_or_default() {
                        let lca = ancestor.get(&dft_dsets.find_mut(succ)).copied().unwrap();
                        self.cross_fwd_edges
                            .entry(lca)
                            .or_default()
                            .push((vertex, succ));
                    } else {
                        self.back_preds.entry(succ).or_default().push(vertex);
                    }
                }

                if pred != 0 {
                    self.non_back_preds.entry(vertex).or_default().push(pred);
                }
            }
        }

        // Number of dfn should be equal or smaller (if there is unreachable node)
        // than grpah size + 1 (number 0 for undiscovered).
        assert!(self.next_dfn as usize <= size + 1);
    }

    fn build<SN>(
        mut self,
        size: usize,
        root: NodeId,
        successors_nodes: &SN,
    ) -> WeakPartialOrdering<NodeId>
    where
        SN: SuccessorNodes<NodeId = NodeId>,
    {
        // Step 1: construct auxilary data structures, including
        // classifying edges, finding lowest common ancestors
        // of cross/forward edges.
        self.build_auxilary(size, root, successors_nodes);

        // Step 2: start constructing WPO.
        let mut dsets = UnionFind::<u32>::new(self.next_dfn as usize);
        // Union find does not guarantee that the root of a subset has
        // always the minimum DFN, so we need to maintain this information.
        // Used for creating exit nodes.
        let mut exit_next_dfn = self.next_dfn;
        // Initializaiton.
        let mut rep: Vec<u32> = (0..self.next_dfn).collect();
        let mut exit: Vec<u32> = (0..self.next_dfn).collect();
        let mut origin: Vec<Vec<(u32, u32)>> = (0..self.next_dfn)
            .map(|v| {
                self.non_back_preds.get(&v).map_or_else(
                    || vec![],
                    |non_back_preds_v| non_back_preds_v.iter().map(|&p| (p, v)).collect(),
                )
            })
            .collect();

        self.dfn_to_wpo_idx.resize(2 * self.next_dfn as usize, 0);
        let mut for_outer_preds = Vec::<(WpoIdx, WpoIdx)>::new();
        let mut components_sizes = vec![0u32; self.next_dfn as usize];
        let mut parent = HashMap::<WpoIdx, WpoIdx>::new();

        // In descending order, excluding 0 which is for undiscovered.
        for h in (1..self.next_dfn).rev() {
            // Restore cross/forward edges
            if let Some(edges) = self.cross_fwd_edges.get(&h) {
                for &(u, v) in edges {
                    let rep_v = rep[dsets.find(v) as usize];
                    self.non_back_preds.entry(rep_v).or_default().push(u);
                    origin[rep_v as usize].push((u, v));
                }
            }

            // Find nested SCCs.
            let mut is_scc = false;
            let mut backpreds_h = HashSet::<u32>::new();
            if let Some(preds) = self.back_preds.get(&h) {
                for &v in preds {
                    if v != h {
                        backpreds_h.insert(rep[dsets.find(v) as usize]);
                    } else {
                        is_scc = true;
                    }
                }
            }

            if !backpreds_h.is_empty() {
                is_scc = true;
            }

            let mut nested_sccs_h = backpreds_h.clone();
            let mut worklist_h = Vec::from_iter(backpreds_h.iter().copied());
            while let Some(v) = worklist_h.pop() {
                if let Some(preds) = self.non_back_preds.get(&v) {
                    for &p in preds {
                        let rep_p = rep[dsets.find(p) as usize];
                        if !nested_sccs_h.contains(&rep_p) && rep_p != h {
                            worklist_h.push(rep_p);
                            nested_sccs_h.insert(rep_p);
                        }
                    }
                }
            }

            // h represents a trivial SCC.
            if !is_scc {
                components_sizes[h as usize] = 1;
                self.add_node(h, h, 1, WpoNodeType::Plain);
                continue;
            }

            // Initialize size to 2 for head and exit.
            let mut sz_h = 2;
            for &v in nested_sccs_h.iter() {
                sz_h += components_sizes[v as usize];
            }
            components_sizes[h as usize] = sz_h;

            // Add new exit.
            let x_h = exit_next_dfn;
            exit_next_dfn += 1;
            self.add_node(x_h, h, sz_h, WpoNodeType::Exit);
            // Wpo index of head is then exit + 1 for the same component.
            self.add_node(h, h, sz_h, WpoNodeType::Head);

            if backpreds_h.is_empty() {
                // Scheduling constraints from h to x_h.
                self.add_successor(h, x_h, x_h, false, &mut for_outer_preds);
            } else {
                for p in backpreds_h {
                    self.add_successor(exit[p as usize], x_h, x_h, false, &mut for_outer_preds);
                }
            }

            // Scheduling constraints between WPOs for nested SCCs.
            for &v in nested_sccs_h.iter() {
                for &(u, vv) in origin[v as usize].iter() {
                    let x_u = exit[rep[dsets.find(u) as usize] as usize];
                    let x_v = exit[v as usize];
                    self.add_successor(x_u, vv, x_v, x_v != v, &mut for_outer_preds);
                }
            }

            // Merging all reps in nested SCCs to h
            for &v in nested_sccs_h.iter() {
                dsets.union(v, h);
                rep[dsets.find(v) as usize] = h;
                parent.insert(self.index_of(v), self.index_of(h));
            }

            exit[h as usize] = x_h;
        }

        // Scheduling constraints between WPOs for maximal SCCs.
        self.toplevel.reserve(self.next_dfn as usize);
        for v in 1..self.next_dfn {
            if rep[dsets.find(v) as usize] == v {
                let v_idx = self.index_of(v);
                self.toplevel.push(v_idx);
                parent.insert(v_idx, v_idx);

                for &(u, vv) in origin[v as usize].iter() {
                    let x_u = exit[rep[dsets.find(u) as usize] as usize];
                    let x_v = exit[v as usize];
                    self.add_successor(x_u, vv, x_v, x_v != v, &mut for_outer_preds);
                }
            }
        }

        // Compute num_outer_preds.
        for &(v, x_max) in for_outer_preds.iter() {
            let mut h = if self.nodes[v as usize].is_head() {
                v
            } else {
                *parent.get(&v).unwrap()
            };
            let mut x = h - 1;
            while x != x_max {
                self.nodes[x as usize].inc_num_outer_preds(v);
                h = *parent.get(&h).unwrap();
                x = h - 1;
            }
            self.nodes[x as usize].inc_num_outer_preds(v);
        }

        WeakPartialOrdering {
            nodes: self.nodes,
            toplevel: self.toplevel,
            post_dfn: self.post_dfn,
        }
    }
}
