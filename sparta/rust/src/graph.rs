/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::borrow::Borrow;
use std::fmt::Debug;
use std::hash::Hash;
use std::marker::PhantomData;

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
    type NodeId: Copy + Hash + Eq + Ord + Debug;
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

    /// The number of all nodes.
    fn size(&self) -> usize;
}

/// A limited view of the graph that allows retrieving successor nodes of a given node.
///
/// Clients can create a new type to implement it, or they can use the default
/// implementation provided on types that implemented `Graph`.
pub trait SuccessorNodes<const S: usize = DEFAULT_GRAPH_SUCCS_NUM> {
    type NodeId: Copy + Hash + Eq;

    fn get_succ_nodes(&self, n: Self::NodeId) -> SmallVec<[Self::NodeId; S]>;
}

/// Implement `SuccessorNodes` for any type implemented `Graph`.
///
/// This is a convenient facility for clients who don't want to
/// create a new type to implement `SuccessorNodes`. Note that
/// this may be less efficient than custom implementation of
/// SuccessorNodes since it can only use available trait methods
/// such as `successors`.
impl<const S: usize, T: Graph<S>> SuccessorNodes<S> for T {
    type NodeId = T::NodeId;

    fn get_succ_nodes(&self, n: Self::NodeId) -> SmallVec<[Self::NodeId; S]> {
        self.successors(n)
            .into_iter()
            .map(|edge_idx| self.target(edge_idx))
            .collect()
    }
}

/// A reversed graph used for backward analysis.
pub struct ReversedGraph<G: Graph<S>, B: Borrow<G>, const S: usize = DEFAULT_GRAPH_SUCCS_NUM> {
    graph: B,
    phantom: PhantomData<*const G>,
}

// Type alias to ease specifying generic parameters.
pub type ReversedRefGraph<'a, G, const S: usize = DEFAULT_GRAPH_SUCCS_NUM> =
    ReversedGraph<G, &'a G, S>;
pub type ReversedIntoGraph<G, const S: usize = DEFAULT_GRAPH_SUCCS_NUM> = ReversedGraph<G, G, S>;

/// Graph type that implements this trait can create reversed graph.
pub trait ReverseGraph<const S: usize = DEFAULT_GRAPH_SUCCS_NUM> {
    type G: Graph<S>;

    /// Reversed graph.
    fn rev(&self) -> ReversedRefGraph<Self::G, S>;

    /// Reversed graph that consumes the original graph.
    fn into_rev(self) -> ReversedIntoGraph<Self::G, S>;
}

/// Default `ReverseGraph` implementation for any type that implements `Graph`
///
/// This can be chained multiple times, e.g., `graph.rev().rev()` becomes
/// the same as `graph`. This behavior is the same as `std::iter::Iterator::rev()`.
impl<const S: usize, T: Graph<S>> ReverseGraph<S> for T {
    type G = T;

    fn rev(&self) -> ReversedRefGraph<T, S> {
        ReversedGraph {
            graph: self,
            phantom: PhantomData,
        }
    }

    fn into_rev(self) -> ReversedIntoGraph<T, S> {
        ReversedGraph {
            graph: self,
            phantom: PhantomData,
        }
    }
}

impl<G: Graph<S>, B: Borrow<G>, const S: usize> Graph<S> for ReversedGraph<G, B, S> {
    type NodeId = G::NodeId;
    type EdgeId = G::EdgeId;

    fn entry(&self) -> Self::NodeId {
        self.graph.borrow().exit()
    }

    fn exit(&self) -> Self::NodeId {
        self.graph.borrow().entry()
    }

    fn predecessors(&self, n: Self::NodeId) -> SmallVec<[Self::EdgeId; S]> {
        self.graph.borrow().successors(n)
    }

    fn successors(&self, n: Self::NodeId) -> SmallVec<[Self::EdgeId; S]> {
        self.graph.borrow().predecessors(n)
    }

    fn source(&self, e: Self::EdgeId) -> Self::NodeId {
        self.graph.borrow().target(e)
    }

    fn target(&self, e: Self::EdgeId) -> Self::NodeId {
        self.graph.borrow().source(e)
    }

    fn size(&self) -> usize {
        self.graph.borrow().size()
    }
}
