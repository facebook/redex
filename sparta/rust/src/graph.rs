/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::hash::Hash;

use smallvec::SmallVec;

pub const DEFAULT_GRAPH_SUCCS_NUM: usize = 4;

/// Graph trait used in Sparta fixpoint iterator.
///
/// Clients can either maintain their own structure and implement
/// this trait, or use third-party graph crates and implement
/// this trait as a wrapper.
/// The constant generic parameter S is an estimated number of successor
/// nodes that is just enough for most nodes. A larger/smaller
/// value will not cause incorrect result, it only affects
/// performance.
// NOTE: due to the status of Rust const generics, we can not define
// S as an associated constant since it is part of the return type for
// method predecessors and successors.
pub trait Graph<const S: usize = DEFAULT_GRAPH_SUCCS_NUM> {
    type NodeId: Copy + Hash + Eq;
    type EdgeId: Copy;

    /// Entry node.
    fn entry(&self) -> Self::NodeId;

    /// Exit node.
    fn exit(&self) -> Self::NodeId;

    /// Predecessors of n.
    fn predecessors(&self, n: Self::NodeId) -> SmallVec<[Self::EdgeId; S]>;

    /// Sucessors of n.
    fn successors(&self, n: Self::NodeId) -> SmallVec<[Self::EdgeId; S]>;

    /// The source node of e.
    fn source(&self, e: Self::EdgeId) -> Self::NodeId;

    /// The target node of e.
    fn target(&self, e: Self::EdgeId) -> Self::NodeId;
}

/// A limited view of the graph that allows retrieving successor nodes of a given node.
///
/// The same type that implements Graph trait can implement this trait,
/// or clients can create a new type to implement it.
pub trait SuccessorNodes<const S: usize = DEFAULT_GRAPH_SUCCS_NUM> {
    type NodeId: Copy + Hash + Eq;

    fn get_succ_nodes(&self, n: Self::NodeId) -> SmallVec<[Self::NodeId; S]>;
}
