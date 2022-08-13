/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::collections::BTreeSet;
use std::collections::HashMap;
use std::fmt::Debug;
use std::hash::Hash;

type WpoIdx = u32;

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

#[derive(Debug, PartialEq)]
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

    fn inc_num_outer_preds(&mut self, idx: WpoIdx) {
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
    nodes: Vec<WpoNode<NodeId>>,
    toplevel: Vec<WpoIdx>,
    post_dfn: HashMap<NodeId, u32>,
}

impl<NodeId> WeakPartialOrdering<NodeId>
where
    NodeId: Copy + Hash + Ord + Debug,
{
    pub fn new<F>(root: NodeId, successors_fn: F) -> Self
    where
        F: Fn(NodeId) -> Vec<NodeId>,
    {
        let mut wpo = Self {
            nodes: Vec::new(),
            toplevel: Vec::new(),
            post_dfn: HashMap::new(),
        };

        if successors_fn(root).is_empty() {
            wpo.nodes.push(WpoNode::plain(root, 1));
            wpo.toplevel.push(0);
            wpo.post_dfn.insert(root, 1);
            return wpo;
        }

        Self::build(
            successors_fn,
            &mut wpo.nodes,
            &mut wpo.toplevel,
            &mut wpo.post_dfn,
        );

        wpo
    }

    fn build<F>(
        successors: F,
        wpo_space: &mut Vec<WpoNode<NodeId>>,
        toplevel: &mut Vec<WpoIdx>,
        post_dfn: &mut HashMap<NodeId, u32>,
    ) where
        F: Fn(NodeId) -> Vec<NodeId>,
    {
        todo!("Construct WPO.");
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

    pub fn is_from_outside(&self, head: NodeId, pred: NodeId) -> bool {
        self.get_post_dfn(head) < self.get_post_dfn(pred)
    }

    fn get_post_dfn(&self, n: NodeId) -> u32 {
        *self.post_dfn.get(&n).unwrap_or(&0)
    }
}
