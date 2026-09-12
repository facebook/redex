/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::string::ToString;

use triomphe::Arc;

use crate::datatype::AbstractDomain;
use crate::datatype::bitvec::BitVec;

/// This structure implements a map of integer/pointer keys to (possibly empty)
/// values. It's based on the following paper:
///
/// C. Okasaki, A. Gill. Fast Mergeable Integer Maps. In Workshop on ML (1998).

#[derive(Debug)]
enum Node<V> {
    Leaf {
        key: BitVec,
        value: V,
    },
    Branch {
        prefix: BitVec,
        left: Arc<Node<V>>,
        right: Arc<Node<V>>,
    },
}

/// Result of updating a subtree through a mutable reference.
enum Updated<V> {
    /// The subtree is still rooted at the same node, which may have been
    /// mutated in place. The caller's `Arc` is still correct.
    Kept,
    /// The subtree is now rooted at a different node.
    Replaced(Arc<Node<V>>),
    /// The subtree is now empty.
    Emptied,
}

impl<V: Eq> PartialEq for Node<V> {
    fn eq(&self, other: &Self) -> bool {
        use Node::*;

        match (self, other) {
            (
                Leaf {
                    key: l_key,
                    value: l_value,
                },
                Leaf {
                    key: r_key,
                    value: r_value,
                },
            ) => l_key == r_key && l_value == r_value,
            (
                Branch {
                    prefix: l_prefix,
                    left: l_left,
                    right: l_right,
                },
                Branch {
                    prefix: r_prefix,
                    left: r_left,
                    right: r_right,
                },
            ) => {
                (l_prefix == r_prefix)
                    && ((Arc::ptr_eq(l_left, r_left) || l_left == r_left)
                        && (Arc::ptr_eq(l_right, r_right) || l_right == r_right))
            }
            (_, _) => false,
        }
    }
}

impl<V> ToString for Node<V> {
    fn to_string(&self) -> String {
        use Node::*;
        match self {
            Leaf { key, value: _ } => format!("(Leaf {})", key.to_string()),
            Branch {
                prefix,
                left,
                right,
            } => format!(
                "(Branch prefix: {} Left: {} Right: {})",
                prefix.to_string(),
                left.to_string(),
                right.to_string()
            ),
        }
    }
}

impl<V> Node<V> {
    /// Core algorithm for node insert, update, and removal.
    /// Returns: updated tree.
    ///
    /// `op` will be called in two separate occasions.
    /// - When node with `key` is found. Then `op` will be called with a reference to the matching
    ///   node. The entire matching subtree will be replaced by return value of `op`. If `op`
    ///   returned `None`, the entire subtree will be removed.
    /// - When node with `key` is not found. Then `op` will be called with a `None` value. The
    ///   return value of `op` is emplaced to the tree. If the return value of `op` is `None`, a
    ///   value equivalent to the original tree `maybe_node` is returned.
    ///
    /// The node is borrowed rather than owned: taking it by value would clone an
    /// `Arc` at every level of the descent, and each of those is an atomic
    /// increment on a node the recursion usually hands straight back.
    fn update_node_by_key<F>(
        maybe_node: Option<&Arc<Node<V>>>,
        key: &BitVec,
        op: F,
    ) -> Option<Arc<Node<V>>>
    where
        F: FnOnce(Option<&Arc<Node<V>>>) -> Option<Arc<Node<V>>>,
    {
        use Node::*;

        if let Some(node) = maybe_node {
            match node.as_ref() {
                Leaf {
                    key: node_key,
                    value: _,
                } => {
                    if node_key == key {
                        op(maybe_node)
                    } else {
                        let maybe_new_node = op(None);
                        match maybe_new_node {
                            Some(new_node) => {
                                Some(Arc::new(Node::make_branch(new_node, node.clone())))
                            }
                            None => maybe_node.cloned(),
                        }
                    }
                }
                Branch {
                    prefix,
                    left,
                    right,
                } => {
                    if key.begins_with(prefix) {
                        let branching_bit = key.get(prefix.len());
                        if !branching_bit {
                            let maybe_new_left = Self::update_node_by_key(Some(left), key, op);
                            match maybe_new_left {
                                Some(new_left) => {
                                    if Arc::ptr_eq(&new_left, left) {
                                        // The subtree is unchanged, so is this branch.
                                        // Returning it as-is keeps the tree shared with
                                        // whatever else points at it.
                                        Some(node.clone())
                                    } else {
                                        Some(Arc::new(Node::make_branch(new_left, right.clone())))
                                    }
                                }
                                None => Some(right.clone()),
                            }
                        } else {
                            let maybe_new_right = Self::update_node_by_key(Some(right), key, op);
                            match maybe_new_right {
                                Some(new_right) => {
                                    if Arc::ptr_eq(&new_right, right) {
                                        Some(node.clone())
                                    } else {
                                        Some(Arc::new(Node::make_branch(left.clone(), new_right)))
                                    }
                                }
                                None => Some(left.clone()),
                            }
                        }
                    } else {
                        // Branch differs, create new branch like how you'd do with another leaf.
                        match op(None) {
                            Some(new_node) => {
                                Some(Arc::new(Node::make_branch(new_node, node.clone())))
                            }
                            None => maybe_node.cloned(),
                        }
                    }
                }
            }
        } else {
            op(None)
        }
    }

    fn updated_from(node: &Arc<Node<V>>, rebuilt: Option<Arc<Node<V>>>) -> Updated<V> {
        match rebuilt {
            Some(new_node) if Arc::ptr_eq(&new_node, node) => Updated::Kept,
            Some(new_node) => Updated::Replaced(new_node),
            None => Updated::Emptied,
        }
    }

    /// Like `update_node_by_key`, but takes the subtree by mutable reference so
    /// that a branch nothing else points at can have its child slot written
    /// directly, rather than the whole root-to-leaf path being rebuilt.
    ///
    /// A node may only be written to once every node between it and the root
    /// has been found to be uniquely owned. As soon as a shared node is
    /// reached, the rest of the descent is delegated to `update_node_by_key`,
    /// which copies: writing below a shared node would be visible to every
    /// other tree holding it.
    fn update_node_by_key_mut<F>(node: &mut Arc<Node<V>>, key: &BitVec, op: F) -> Updated<V>
    where
        F: FnOnce(Option<Arc<Node<V>>>) -> Option<Arc<Node<V>>>,
    {
        use Node::*;

        // Only a branch that the key descends into has a child slot worth
        // writing. Leaves, and branches whose prefix the key diverges from,
        // produce a new node either way.
        let descend_left = match node.as_ref() {
            Branch { prefix, .. } if key.begins_with(prefix) => !key.get(prefix.len()),
            _ => {
                let rebuilt = Self::update_node_by_key(Some(&*node), key, op);
                return Self::updated_from(node, rebuilt);
            }
        };

        if !Arc::is_unique(node) {
            let rebuilt = Self::update_node_by_key(Some(&*node), key, op);
            return Self::updated_from(node, rebuilt);
        }

        let Some(Branch { left, right, .. }) = Arc::get_mut(node) else {
            unreachable!("matched as a branch above")
        };

        // The branch keeps its prefix. Every key underneath it still begins
        // with that prefix, and the two sides still differ at the branching
        // bit, so the value `make_branch` would recompute is the one already
        // stored here.
        if descend_left {
            match Self::update_node_by_key_mut(left, key, op) {
                Updated::Kept => Updated::Kept,
                Updated::Replaced(new_left) => {
                    *left = new_left;
                    Updated::Kept
                }
                // A branch never keeps a single child.
                Updated::Emptied => Updated::Replaced(right.clone()),
            }
        } else {
            match Self::update_node_by_key_mut(right, key, op) {
                Updated::Kept => Updated::Kept,
                Updated::Replaced(new_right) => {
                    *right = new_right;
                    Updated::Kept
                }
                Updated::Emptied => Updated::Replaced(left.clone()),
            }
        }
    }

    fn find_node_by_key<'a>(
        maybe_node: Option<&'a Arc<Node<V>>>,
        lookup_key: &BitVec,
    ) -> Option<&'a Arc<Node<V>>> {
        use Node::*;
        if let Some(node) = maybe_node {
            match node.as_ref() {
                Leaf { key, value: _ } => {
                    if key == lookup_key {
                        Some(node)
                    } else {
                        None
                    }
                }
                Branch {
                    prefix,
                    left,
                    right,
                } => {
                    if prefix.len() < lookup_key.len() {
                        if !lookup_key.get(prefix.len()) {
                            Self::find_node_by_key(Some(left), lookup_key)
                        } else {
                            Self::find_node_by_key(Some(right), lookup_key)
                        }
                    } else if prefix == lookup_key {
                        Some(node)
                    } else {
                        None
                    }
                }
            }
        } else {
            None
        }
    }

    fn find_leaf_by_key<'a>(
        maybe_node: Option<&'a Arc<Node<V>>>,
        lookup_key: &BitVec,
    ) -> Option<&'a Arc<Node<V>>> {
        if let Some(found_node) = Self::find_node_by_key(maybe_node, lookup_key) {
            return match found_node.as_ref() {
                Node::Leaf { key: _, value: _ } => Some(found_node),
                _ => None,
            };
        }
        None
    }

    fn contains_leaf_with_key(maybe_node: Option<&Arc<Node<V>>>, lookup_key: &BitVec) -> bool {
        Self::find_leaf_by_key(maybe_node, lookup_key).is_some()
    }

    fn key_or_prefix(&self) -> &BitVec {
        use Node::*;
        match self {
            Leaf { key, value: _ } => key,
            Branch {
                prefix,
                left: _,
                right: _,
            } => prefix,
        }
    }

    fn make_branch(one: Arc<Self>, other: Arc<Self>) -> Self {
        let v1 = one.key_or_prefix();
        let v2 = other.key_or_prefix();
        assert!(v1 != v2);
        let common = BitVec::common_prefix(v1, v2);
        let branching_bit = common.len();

        let b1 = v1.get(branching_bit);
        let b2 = v2.get(branching_bit);
        assert!(b1 != b2);

        let left;
        let right;

        if !b1 {
            left = one;
            right = other;
        } else {
            left = other;
            right = one;
        }

        Node::Branch {
            prefix: common,
            left,
            right,
        }
    }

    fn combine_leaves_by_key(
        node: &Arc<Node<V>>,
        key: &BitVec,
        other: Arc<Node<V>>,
        leaf_combine: &impl Fn(Arc<Node<V>>, Arc<Node<V>>) -> Option<Arc<Node<V>>>,
    ) -> Arc<Node<V>> {
        let updated = Self::update_node_by_key(Some(node), key, move |leaf| match leaf {
            Some(leaf) => leaf_combine(leaf.clone(), other),
            None => Some(other),
        });
        updated.unwrap() // Leaf combine should not make deletions.
    }

    /// Merge two trees. Combined tree should contain all keys from s and t.
    /// If duplicate keys are found, two nodes are passed to `leaf_combine` shall be called
    /// with values from s on the left hand side and values from t on the right hand side.
    fn merge_trees(
        s: &Arc<Node<V>>,
        t: &Arc<Node<V>>,
        leaf_combine: &impl Fn(Arc<Node<V>>, Arc<Node<V>>) -> Option<Arc<Node<V>>>,
    ) -> Arc<Node<V>> {
        use Node::*;

        if Arc::ptr_eq(s, t) {
            // Quickly checking if the two trees are identical to allow union
            // operation to complete in sublinear time when operands share some structures.
            return s.clone();
        }

        match (s.as_ref(), t.as_ref()) {
            // We check if t is a leaf first, before s. Because the leaf combine
            // operator may not be commutative when s and t are both leaves.
            (
                _,
                Leaf {
                    key: t_key,
                    value: _,
                },
            ) => {
                // Insert t into s, where s may or may not be a leaf.
                // If t has the same key as one of the element in s, t is rhs of the combine operator.
                Self::combine_leaves_by_key(s, t_key, t.clone(), leaf_combine)
            }
            (
                Leaf {
                    key: s_key,
                    value: _,
                },
                _,
            ) => {
                // Insert s into t
                Self::combine_leaves_by_key(t, s_key, s.clone(), leaf_combine)
            }
            (
                Branch {
                    prefix: s_prefix,
                    left: s_left,
                    right: s_right,
                },
                Branch {
                    prefix: t_prefix,
                    left: t_left,
                    right: t_right,
                },
            ) => {
                if s_prefix == t_prefix {
                    // The two trees have the same prefix. We just merge the subtrees.
                    let new_left = Self::merge_trees(s_left, t_left, leaf_combine);
                    let new_right = Self::merge_trees(s_right, t_right, leaf_combine);

                    if Arc::ptr_eq(&new_left, s_left) && Arc::ptr_eq(&new_right, s_right) {
                        s.clone()
                    } else if Arc::ptr_eq(&new_left, t_left) && Arc::ptr_eq(&new_right, t_right) {
                        t.clone()
                    } else {
                        Arc::new(Node::make_branch(new_left, new_right))
                    }
                } else if t_prefix.begins_with(s_prefix) {
                    let branching_bit = t_prefix.get(s_prefix.len());
                    if !branching_bit {
                        let new_left = Self::merge_trees(s_left, t, leaf_combine);
                        if Arc::ptr_eq(s_left, &new_left) {
                            s.clone()
                        } else {
                            Arc::new(Node::make_branch(new_left, s_right.clone()))
                        }
                    } else {
                        let new_right = Self::merge_trees(s_right, t, leaf_combine);
                        if Arc::ptr_eq(s_right, &new_right) {
                            s.clone()
                        } else {
                            Arc::new(Node::make_branch(s_left.clone(), new_right))
                        }
                    }
                } else if s_prefix.begins_with(t_prefix) {
                    let branching_bit = s_prefix.get(t_prefix.len());
                    if !branching_bit {
                        let new_left = Self::merge_trees(s, t_left, leaf_combine);
                        if Arc::ptr_eq(t_left, &new_left) {
                            t.clone()
                        } else {
                            Arc::new(Node::make_branch(new_left, t_right.clone()))
                        }
                    } else {
                        let new_right = Self::merge_trees(s, t_right, leaf_combine);
                        if Arc::ptr_eq(t_right, &new_right) {
                            t.clone()
                        } else {
                            Arc::new(Node::make_branch(t_left.clone(), new_right))
                        }
                    }
                } else {
                    // The prefixes disagree.
                    Arc::new(Node::make_branch(s.clone(), t.clone()))
                }
            }
        }
    }

    /// Intersection of two trees. Combined tree should contain all keys that are found from both
    /// s and t. If duplicate keys are found, two nodes are passed to `leaf_combine` shall be
    /// called with values from s on the left hand side and values from t on the right hand side.
    fn intersect_trees(
        s: &Arc<Node<V>>,
        t: &Arc<Node<V>>,
        leaf_combine: &impl Fn(Arc<Node<V>>, Arc<Node<V>>) -> Option<Arc<Node<V>>>,
    ) -> Option<Arc<Node<V>>> {
        use Node::*;

        if Arc::ptr_eq(s, t) {
            // This conditions allows the inclusion test to run in sublinear time
            // when comparing Patricia trees that share some structure.
            return Some(s.clone());
        }

        match (s.as_ref(), t.as_ref()) {
            (Leaf { key, value: _ }, _) => match Self::find_leaf_by_key(Some(t), key) {
                // Keep leaves from s on the left.
                Some(t_leaf) => leaf_combine(s.clone(), t_leaf.clone()),
                None => None,
            },
            (_, Leaf { key, value: _ }) => match Self::find_leaf_by_key(Some(s), key) {
                // Keep leaves from s on the left.
                Some(s_leaf) => leaf_combine(s_leaf.clone(), t.clone()),
                None => None,
            },
            (
                Branch {
                    prefix: s_prefix,
                    left: s_left,
                    right: s_right,
                },
                Branch {
                    prefix: t_prefix,
                    left: t_left,
                    right: t_right,
                },
            ) => {
                if s_prefix == t_prefix {
                    let new_left = Self::intersect_trees(s_left, t_left, leaf_combine);
                    let new_right = Self::intersect_trees(s_right, t_right, leaf_combine);
                    match (new_left, new_right) {
                        (left, None) => left,
                        (None, right) => right,
                        (Some(left), Some(right)) => {
                            if Arc::ptr_eq(&left, s_left) && Arc::ptr_eq(&right, s_right) {
                                Some(s.clone())
                            } else {
                                Some(Arc::new(Self::make_branch(left, right)))
                            }
                        }
                    }
                } else if t_prefix.begins_with(s_prefix) {
                    let branching_bit = t_prefix.get(s_prefix.len());
                    Self::intersect_trees(
                        if !branching_bit { s_left } else { s_right },
                        t,
                        leaf_combine,
                    )
                } else if s_prefix.begins_with(t_prefix) {
                    let branching_bit = s_prefix.get(t_prefix.len());
                    Self::intersect_trees(
                        s,
                        if !branching_bit { t_left } else { t_right },
                        leaf_combine,
                    )
                } else {
                    None
                }
            }
        }
    }

    /// Returns true if s is a subset of t.
    fn is_tree_subset_of(s: &Arc<Node<V>>, t: &Arc<Node<V>>) -> bool {
        use Node::*;

        if Arc::ptr_eq(s, t) {
            // This conditions allows the inclusion test to run in sublinear time
            // when comparing Patricia trees that share some structure.
            return true;
        }

        match (s.as_ref(), t.as_ref()) {
            (Leaf { key, value: _ }, _) => Self::contains_leaf_with_key(Some(t), key),
            (_, Leaf { key: _, value: _ }) => false,
            (
                Branch {
                    prefix: s_prefix,
                    left: s_left,
                    right: s_right,
                },
                Branch {
                    prefix: t_prefix,
                    left: t_left,
                    right: t_right,
                },
            ) => {
                if s_prefix == t_prefix {
                    Self::is_tree_subset_of(s_left, t_left)
                        && Self::is_tree_subset_of(s_right, t_right)
                } else if s_prefix.begins_with(t_prefix) {
                    assert!(s_prefix.len() > t_prefix.len());
                    let branching_bit = s_prefix.get(t_prefix.len());
                    if !branching_bit {
                        Self::is_tree_subset_of(s_left, t_left)
                            && Self::is_tree_subset_of(s_right, t_left)
                    } else {
                        Self::is_tree_subset_of(s_left, t_right)
                            && Self::is_tree_subset_of(s_right, t_right)
                    }
                } else {
                    false
                }
            }
        }
    }
}

impl<D: AbstractDomain> Node<D> {
    fn is_tree_leq(s: Option<&Arc<Node<D>>>, t: Option<&Arc<Node<D>>>, implicit_value: &D) -> bool {
        match (s, t) {
            (None, None) => true,
            (None, _) => implicit_value.is_bottom(),
            (_, None) => implicit_value.is_top(),
            (Some(s), Some(t)) => Self::is_tree_leq_impl(s, t, implicit_value),
        }
    }

    fn is_tree_leq_impl(s: &Arc<Node<D>>, t: &Arc<Node<D>>, implicit_value: &D) -> bool {
        use Node::*;

        if Arc::ptr_eq(s, t) {
            return true;
        }

        match (s.as_ref(), t.as_ref()) {
            (
                Leaf {
                    key: s_key,
                    value: s_value,
                },
                Leaf {
                    key: t_key,
                    value: t_value,
                },
            ) => s_key == t_key && s_value.leq(t_value),
            (
                Leaf {
                    key: s_key,
                    value: s_value,
                },
                _,
            ) => {
                if implicit_value.is_top() {
                    false
                } else {
                    match Self::find_leaf_by_key(Some(t), s_key) {
                        Some(rc) => match rc.as_ref() {
                            Leaf {
                                key: _,
                                value: t_value,
                            } => s_value.leq(t_value),
                            _ => unreachable!(),
                        },
                        None => false,
                    }
                }
            }
            (
                _,
                Leaf {
                    key: t_key,
                    value: t_value,
                },
            ) => {
                if implicit_value.is_bottom() {
                    false
                } else {
                    match Self::find_leaf_by_key(Some(s), t_key) {
                        Some(rc) => match rc.as_ref() {
                            Leaf {
                                key: _,
                                value: s_value,
                            } => s_value.leq(t_value),
                            _ => unreachable!(),
                        },
                        None => false,
                    }
                }
            }
            (
                Branch {
                    prefix: s_prefix,
                    left: s_left,
                    right: s_right,
                },
                Branch {
                    prefix: t_prefix,
                    left: t_left,
                    right: t_right,
                },
            ) => {
                if s_prefix == t_prefix {
                    // The two trees have the same prefix, compare each subtrees.
                    Self::is_tree_leq_impl(s_left, t_left, implicit_value)
                        && Self::is_tree_leq_impl(s_right, t_right, implicit_value)
                } else if s_prefix.begins_with(t_prefix) {
                    // The tree s only contains bindings present in a subtree of t, and t has
                    // bindings not present in s.
                    let branching_bit = s_prefix.get(t_prefix.len());
                    implicit_value.is_bottom()
                        && Self::is_tree_leq_impl(
                            s,
                            if !branching_bit { t_left } else { t_right },
                            implicit_value,
                        )
                } else if t_prefix.begins_with(s_prefix) {
                    // The tree t only contains bindings present in a subtree of s, and s has
                    // bindings not present in t.
                    let branching_bit = t_prefix.get(s_prefix.len());
                    implicit_value.is_top()
                        && Self::is_tree_leq_impl(
                            if !branching_bit { s_left } else { s_right },
                            t,
                            implicit_value,
                        )
                } else {
                    false
                }
            }
        }
    }
}

// Create an interface that gives the user a "mutable" illusion of an immutable data structure.
#[derive(Debug)]
pub(crate) struct PatriciaTree<V> {
    root: Option<Arc<Node<V>>>,
}

impl<V> PatriciaTree<V> {
    pub(crate) fn new() -> Self {
        Self { root: None }
    }

    pub(crate) fn clear(&mut self) {
        self.root = None;
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.root.is_none()
    }

    // Not a very fast operation.
    pub(crate) fn len(&self) -> usize {
        self.iter().count()
    }

    /// Apply `op` to the leaf bound to `key`, updating nodes in place where
    /// nothing else points at them.
    fn update_by_key<F>(&mut self, key: &BitVec, op: F)
    where
        F: FnOnce(Option<Arc<Node<V>>>) -> Option<Arc<Node<V>>>,
    {
        let outcome = match self.root.as_mut() {
            None => {
                self.root = op(None);
                return;
            }
            Some(root) => Node::update_node_by_key_mut(root, key, op),
        };

        match outcome {
            Updated::Kept => {}
            Updated::Replaced(new_root) => self.root = Some(new_root),
            Updated::Emptied => self.root = None,
        }
    }

    pub(crate) fn insert(&mut self, key: BitVec, value: V)
    where
        V: Eq,
    {
        let leaf_key = key.clone();
        let node_op = move |existing: Option<&Arc<Node<V>>>| {
            let value_is_unchanged = matches!(
                existing.map(Arc::as_ref),
                Some(Node::Leaf { value: old_value, .. }) if *old_value == value
            );

            if value_is_unchanged {
                // Keep the existing leaf, so that the branches above it are
                // shared rather than rebuilt.
                existing.cloned()
            } else {
                Some(Arc::new(Node::Leaf {
                    key: leaf_key,
                    value,
                }))
            }
        };
        self.update_by_key(&key, node_op);
    }

    pub(crate) fn contains_key(&self, key: &BitVec) -> bool {
        self.get(key).is_some()
    }

    pub(crate) fn get(&self, key: &BitVec) -> Option<&V> {
        use Node::*;
        let node = Node::find_leaf_by_key(self.root.as_ref(), key);
        match node {
            Some(leaf_node) => match leaf_node.as_ref() {
                Leaf { key: _, value } => Some(value),
                _ => panic!("Did not correctly get a leaf!"),
            },
            None => None,
        }
    }

    pub(crate) fn remove(&mut self, key: &BitVec) {
        self.update_by_key(key, |_| None);
    }

    pub(crate) fn iter(&self) -> PatriciaTreePostOrderIterator<V> {
        PatriciaTreePostOrderIterator::<V>::from_tree(self)
    }

    fn get_leaf_combine_with_value_op_semantics(
        value_op_on_duplicate_key: impl Fn(&V, &V) -> V,
    ) -> impl Fn(Arc<Node<V>>, Arc<Node<V>>) -> Option<Arc<Node<V>>>
    where
        V: Eq,
    {
        use Node::*;

        move |one_leaf: Arc<Node<V>>, other_leaf: Arc<Node<V>>| match (
            one_leaf.as_ref(),
            other_leaf.as_ref(),
        ) {
            (
                Leaf {
                    key,
                    value: l_value,
                },
                Leaf {
                    key: _,
                    value: r_value,
                },
            ) => {
                let new_value = value_op_on_duplicate_key(l_value, r_value);
                if new_value == *l_value {
                    // Keeping the existing leaf is what lets the callers above
                    // recognize an unchanged subtree and share it.
                    Some(one_leaf.clone())
                } else {
                    Some(Arc::new(Leaf {
                        key: key.clone(),
                        value: new_value,
                    }))
                }
            }
            _ => panic!("leaf_combine should only be called on leaves!"),
        }
    }

    pub(crate) fn union_with(
        &mut self,
        other: &Self,
        value_op_on_duplicate_key: impl Fn(&V, &V) -> V,
    ) where
        V: Eq,
    {
        match (self.root.as_ref(), other.root.as_ref()) {
            (None, _) => self.root = other.root.clone(),
            (Some(_), None) => {}
            (Some(self_node), Some(other_node)) => {
                self.root = Some(Node::merge_trees(
                    self_node,
                    other_node,
                    &Self::get_leaf_combine_with_value_op_semantics(value_op_on_duplicate_key),
                ));
            }
        }
    }

    pub(crate) fn intersect_with(
        &mut self,
        other: &Self,
        value_op_on_duplicate_key: impl Fn(&V, &V) -> V,
    ) where
        V: Eq,
    {
        match (self.root.as_ref(), other.root.as_ref()) {
            (None, _) => {}
            (Some(_), None) => {
                self.root = None;
            }
            (Some(self_node), Some(other_node)) => {
                self.root = Node::intersect_trees(
                    self_node,
                    other_node,
                    &Self::get_leaf_combine_with_value_op_semantics(value_op_on_duplicate_key),
                );
            }
        }
    }

    pub(crate) fn subset_of(&self, other: &Self) -> bool {
        match (self.root.as_ref(), other.root.as_ref()) {
            (None, _) => true,
            (_, None) => false,
            (Some(s), Some(t)) => Node::is_tree_subset_of(s, t),
        }
    }
}

impl<D: AbstractDomain> PatriciaTree<D> {
    // Leq operation here facilitates the abstract partition and abstract environment leqs.
    // Both structures have implicit values that they use to encode top and bottom values
    // efficiently, which changes the semantics of the leq operation.
    pub(crate) fn leq(&self, other: &Self, implicit_value: &D) -> bool {
        Node::is_tree_leq(self.root.as_ref(), other.root.as_ref(), implicit_value)
    }
}

impl<V> Clone for PatriciaTree<V> {
    fn clone(&self) -> Self {
        // Cloning a patricia tree is just cloning the shared reference to the root node.
        Self {
            root: self.root.clone(),
        }
    }
}

impl<V: Eq> PartialEq for PatriciaTree<V> {
    fn eq(&self, other: &Self) -> bool {
        match (self.root.as_ref(), other.root.as_ref()) {
            (None, None) => true,
            (Some(ref self_node), Some(ref other_node)) => {
                Arc::ptr_eq(self_node, other_node) || (self_node == other_node)
            }
            (_, _) => false,
        }
    }
}

impl<V: Eq> Eq for PatriciaTree<V> {}

#[derive(Debug)]
pub(crate) struct PatriciaTreePostOrderIterator<'a, V> {
    branch_stack: Vec<&'a Node<V>>,
    current: Option<&'a Node<V>>,
}

impl<'a, V> PatriciaTreePostOrderIterator<'a, V> {
    pub(crate) fn from_tree(tree: &'a PatriciaTree<V>) -> Self {
        let mut ret = Self {
            branch_stack: vec![],
            current: None,
        };

        match tree.root {
            Some(ref node) => ret.next_leaf(node),
            None => (),
        };

        ret
    }

    fn next_leaf(&mut self, subtree: &'a Arc<Node<V>>) {
        let mut node = subtree.as_ref();

        while let Node::Branch {
            prefix: _,
            left,
            right: _,
        } = node
        {
            self.branch_stack.push(node);
            node = left.as_ref();
        }

        // node is a leaf now.
        self.current = Some(node);
    }

    fn next_node(&mut self) -> Option<&'a Node<V>> {
        let ret = self.current;
        self.current = None;

        if let Some(br) = self.branch_stack.pop() {
            match br {
                Node::Branch {
                    prefix: _,
                    left: _,
                    right,
                } => self.next_leaf(right),
                _ => panic!("Malformed Patricia Tree Iterator"),
            }
        }

        ret
    }

    fn into_tuple(node: Option<&Node<V>>) -> Option<(&BitVec, &V)> {
        match node {
            Some(leaf) => match leaf {
                Node::Leaf { key, value } => Some((key, value)),
                _ => panic!("Malformed Patricia Tree Iterator"),
            },
            None => None,
        }
    }
}

impl<'a, V> Iterator for PatriciaTreePostOrderIterator<'a, V> {
    type Item = (&'a BitVec, &'a V);

    fn next(&mut self) -> Option<Self::Item> {
        Self::into_tuple(self.next_node())
    }
}

#[cfg(test)]
mod tests {
    use crate::datatype::patricia_tree_impl::*;

    fn root_ptr<V>(tree: &PatriciaTree<V>) -> *const Node<V> {
        tree.root.as_ref().map_or(std::ptr::null(), Arc::as_ptr)
    }

    fn set_of(range: std::ops::Range<u32>) -> PatriciaTree<()> {
        let mut tree = PatriciaTree::new();
        for i in range {
            tree.insert(i.into(), ());
        }
        tree
    }

    #[test]
    fn test_update_node_by_key_borrows_matching_node() {
        let key: BitVec = 42u32.into();
        let root = Arc::new(Node::Leaf {
            key: key.clone(),
            value: (),
        });

        let updated = Node::update_node_by_key(Some(&root), &key, |existing| {
            let existing = existing.expect("the root leaf should match the key");
            assert!(
                Arc::is_unique(existing),
                "the operation should not receive a cloned `Arc`"
            );
            Some(existing.clone())
        })
        .expect("the operation should retain the matching root");

        assert!(Arc::ptr_eq(&updated, &root));
    }

    /// Operations whose result is equal to their input must return the input
    /// tree itself, so that it stays shared with anything else pointing at it.
    #[test]
    fn test_no_op_operations_preserve_sharing() {
        let tree = set_of(0..1000);
        let root = root_ptr(&tree);

        let mut removed = tree.clone();
        removed.remove(&999_999u32.into());
        assert_eq!(root_ptr(&removed), root);

        let mut intersected = tree.clone();
        intersected.intersect_with(&set_of(0..1500), |_, _| ());
        assert_eq!(root_ptr(&intersected), root);

        let mut united = tree.clone();
        united.union_with(&set_of(0..100), |_, _| ());
        assert_eq!(root_ptr(&united), root);

        let mut inserted = tree.clone();
        inserted.insert(500u32.into(), ());
        assert_eq!(root_ptr(&inserted), root);
    }

    /// Re-inserting an equal binding must be free, but a different value must
    /// still take effect and must not disturb trees sharing the old one.
    #[test]
    fn test_insert_of_existing_binding_preserves_sharing() {
        let mut tree: PatriciaTree<u32> = PatriciaTree::new();
        for i in 0u32..1000 {
            tree.insert(i.into(), i * 2);
        }
        let root = root_ptr(&tree);

        let mut same = tree.clone();
        same.insert(500u32.into(), 1000);
        assert_eq!(root_ptr(&same), root);
        assert_eq!(same.get(&500u32.into()), Some(&1000));

        let mut changed = tree.clone();
        changed.insert(500u32.into(), 7);
        assert_ne!(root_ptr(&changed), root);
        assert_eq!(changed.len(), 1000);
        assert_eq!(changed.get(&500u32.into()), Some(&7));
        assert_eq!(tree.get(&500u32.into()), Some(&1000));
    }

    /// The leaf reuse above must not swallow an actual change of value.
    #[test]
    fn test_combine_still_updates_changed_values() {
        let mut s: PatriciaTree<u32> = PatriciaTree::new();
        s.insert(1u32.into(), 10);
        s.insert(2u32.into(), 20);

        let mut t: PatriciaTree<u32> = PatriciaTree::new();
        t.insert(2u32.into(), 5);
        t.insert(3u32.into(), 30);

        let mut union = s.clone();
        union.union_with(&t, |l, r| l + r);
        assert_eq!(union.get(&1u32.into()), Some(&10));
        assert_eq!(union.get(&2u32.into()), Some(&25));
        assert_eq!(union.get(&3u32.into()), Some(&30));

        let mut intersection = s.clone();
        intersection.intersect_with(&t, |l, r| l + r);
        assert_eq!(intersection.len(), 1);
        assert_eq!(intersection.get(&2u32.into()), Some(&25));
    }

    #[test]
    fn test_basic_insertion() {
        let mut map: PatriciaTree<usize> = PatriciaTree::new();
        map.insert(1.into(), 111);
        map.insert(22.into(), 222);
        map.insert(42.into(), 444);
        map.insert(42.into(), 444);
        map.insert(42.into(), 444);
        map.insert(13.into(), 1313);

        assert!(map.contains_key(&1.into()));
        assert!(map.contains_key(&22.into()));
        assert!(map.contains_key(&42.into()));
        assert!(!map.contains_key(&2.into()));
        assert!(!map.contains_key(&3.into()));

        assert_eq!(map.len(), 4);

        let mut map2 = map.clone();

        assert!(map2 == map);

        map2.insert(55.into(), 555);
        assert_eq!(map.len(), 4);
        assert_eq!(map2.len(), 5);

        assert!(map2 != map);

        map2.remove(&55.into());
        assert_eq!(map2.len(), 4);
        assert!(map2 == map);

        map2.remove(&1.into());
        assert!(map.contains_key(&1.into()));
        assert!(!map2.contains_key(&1.into()));

        map2.remove(&1.into());
        assert_eq!(map2.len(), 3);
        assert!(!map2.contains_key(&1.into()));

        map2.remove(&22.into());
        assert_eq!(map2.len(), 2);
        assert!(!map2.contains_key(&22.into()));

        map2.remove(&13.into());
        assert_eq!(map2.len(), 1);
        assert!(!map2.contains_key(&13.into()));

        map2.remove(&42.into());
        assert_eq!(map2.len(), 0);
    }
}
