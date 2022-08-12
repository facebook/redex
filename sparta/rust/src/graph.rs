/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::hash::Hash;

/// Graph trait used in Sparta fixpoint iterator.
///
/// Clients can either maintain their own structure and implement
/// this trait, or use third-party graph crates and implement
/// this trait as a wrapper.
pub trait Graph {
    type NodeId: Copy + Hash + Eq;
    type EdgeId: Copy;

    /// Entry node.
    fn entry(&self) -> Self::NodeId;

    /// Exit node.
    fn exit(&self) -> Self::NodeId;

    /// Predecessors of n.
    fn predecessors(&self, n: Self::NodeId) -> &[Self::EdgeId];

    /// Sucessors of n.
    fn successors(&self, n: Self::NodeId) -> &[Self::EdgeId];

    /// The source node of e.
    fn source(&self, e: Self::EdgeId) -> Self::NodeId;

    /// The target node of e.
    fn target(&self, e: Self::EdgeId) -> Self::NodeId;
}
