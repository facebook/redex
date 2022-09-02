/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::rc::Rc;
use std::string::ToString;

use crate::datatype::bitvec::BitVec;

/// This structure implements a map of integer/pointer keys to (possibly empty)
/// values. It's based on the following paper:
///
/// C. Okasaki, A. Gill. Fast Mergeable Integer Maps. In Workshop on ML (1998).

enum Node<V: Sized> {
    Leaf {
        key: BitVec,
        value: V,
    },
    // Rust doesn't have higher kinded types for Rc.
    // Ideally we should reuse this enum for an Arc variant of it.
    Branch {
        prefix: BitVec,
        left: Rc<Node<V>>,
        right: Rc<Node<V>>,
    },
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

impl<V: Sized> Node<V> {
    /// Core algorithm for node insert, update, and removal.
    /// Returns: updated tree.
    ///
    /// `op` will be called in two separate occasions.
    /// - When node with `key` is found. Then `op` will be called with a `Some` value containing
    ///   the matching node. The entire matching subtree will be replaced by return value of `op`.
    ///   If `op` returned `None`, the entire subtree will be removed.
    /// - When node with `key` is not found. Then `op` will be called with a `None` value. The
    ///   return value of `op` is emplaced to the tree. If the return value of `op` is `None`, a
    ///   value equivalent to the original tree `maybe_node` is returned.
    fn update_node_by_key<F>(
        maybe_node: Option<Rc<Node<V>>>,
        key: &BitVec,
        op: F,
    ) -> Option<Rc<Node<V>>>
    where
        F: FnOnce(Option<Rc<Node<V>>>) -> Option<Rc<Node<V>>>,
    {
        use Node::*;

        if let Some(ref node) = maybe_node {
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
                                Some(Rc::new(Node::make_branch(new_node, node.clone())))
                            }
                            None => maybe_node,
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
                            let maybe_new_left =
                                Self::update_node_by_key(Some(left.clone()), key, op);
                            match maybe_new_left {
                                Some(new_left) => {
                                    // Possible optimization: If `new_left` ptr_eq to `left`, do nothing.
                                    Some(Rc::new(Node::make_branch(new_left, right.clone())))
                                }
                                None => Some(right.clone()),
                            }
                        } else {
                            let maybe_new_right =
                                Self::update_node_by_key(Some(right.clone()), key, op);
                            match maybe_new_right {
                                Some(new_right) => {
                                    Some(Rc::new(Node::make_branch(left.clone(), new_right)))
                                }
                                None => Some(left.clone()),
                            }
                        }
                    } else {
                        // Branch differs, create new branch like how you'd do with another leaf.
                        match op(None) {
                            Some(new_node) => {
                                Some(Rc::new(Node::make_branch(new_node, node.clone())))
                            }
                            None => maybe_node,
                        }
                    }
                }
            }
        } else {
            op(None)
        }
    }

    fn find_node_by_key<'a>(
        maybe_node: Option<&'a Rc<Node<V>>>,
        lookup_key: &BitVec,
    ) -> Option<&'a Rc<Node<V>>> {
        use Node::*;
        if let Some(node) = maybe_node {
            match node.as_ref() {
                Leaf { ref key, value: _ } => {
                    if key == lookup_key {
                        Some(node)
                    } else {
                        None
                    }
                }
                Branch {
                    ref prefix,
                    ref left,
                    ref right,
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
        maybe_node: Option<&'a Rc<Node<V>>>,
        lookup_key: &BitVec,
    ) -> Option<&'a Rc<Node<V>>> {
        if let Some(found_node) = Self::find_node_by_key(maybe_node, lookup_key) {
            return match found_node.as_ref() {
                Node::Leaf { key: _, value: _ } => Some(found_node),
                _ => None,
            };
        }
        None
    }

    fn contains_leaf_with_key(maybe_node: Option<&Rc<Node<V>>>, lookup_key: &BitVec) -> bool {
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

    fn make_branch(one: Rc<Self>, other: Rc<Self>) -> Self {
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
        node: Rc<Node<V>>,
        key: &BitVec,
        other: Rc<Node<V>>,
        leaf_combine: &impl Fn(Rc<Node<V>>, Rc<Node<V>>) -> Option<Rc<Node<V>>>,
    ) -> Rc<Node<V>> {
        let updated = Self::update_node_by_key(Some(node), key, move |leaf| match leaf {
            Some(leaf) => leaf_combine(leaf, other),
            None => Some(other),
        });
        updated.unwrap() // Leaf combine should not make deletions.
    }

    /// Merge two trees. Combined tree should contain all keys from s and t.
    /// If duplicate keys are found, two nodes are passed to `leaf_combine` shall be called
    /// with values from s on the left hand side and values from t on the right hand side.
    fn merge_trees(
        s: &Rc<Node<V>>,
        t: &Rc<Node<V>>,
        leaf_combine: &impl Fn(Rc<Node<V>>, Rc<Node<V>>) -> Option<Rc<Node<V>>>,
    ) -> Rc<Node<V>> {
        use Node::*;

        if Rc::ptr_eq(s, t) {
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
                Self::combine_leaves_by_key(s.clone(), t_key, t.clone(), leaf_combine)
            }
            (
                Leaf {
                    key: s_key,
                    value: _,
                },
                _,
            ) => {
                // Insert s into t
                Self::combine_leaves_by_key(t.clone(), s_key, s.clone(), leaf_combine)
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

                    if Rc::ptr_eq(&new_left, s_left) && Rc::ptr_eq(&new_right, s_right) {
                        s.clone()
                    } else if Rc::ptr_eq(&new_left, t_left) && Rc::ptr_eq(&new_right, t_right) {
                        t.clone()
                    } else {
                        Rc::new(Node::make_branch(new_left, new_right))
                    }
                } else if t_prefix.begins_with(s_prefix) {
                    let branching_bit = t_prefix.get(s_prefix.len());
                    if !branching_bit {
                        let new_left = Self::merge_trees(s_left, t, leaf_combine);
                        if Rc::ptr_eq(s_left, &new_left) {
                            s.clone()
                        } else {
                            Rc::new(Node::make_branch(new_left, s_right.clone()))
                        }
                    } else {
                        let new_right = Self::merge_trees(s_right, t, leaf_combine);
                        if Rc::ptr_eq(s_right, &new_right) {
                            s.clone()
                        } else {
                            Rc::new(Node::make_branch(s_left.clone(), new_right))
                        }
                    }
                } else if s_prefix.begins_with(t_prefix) {
                    let branching_bit = s_prefix.get(t_prefix.len());
                    if !branching_bit {
                        let new_left = Self::merge_trees(s, t_left, leaf_combine);
                        if Rc::ptr_eq(t_left, &new_left) {
                            t.clone()
                        } else {
                            Rc::new(Node::make_branch(new_left, t_right.clone()))
                        }
                    } else {
                        let new_right = Self::merge_trees(s, t_right, leaf_combine);
                        if Rc::ptr_eq(t_right, &new_right) {
                            t.clone()
                        } else {
                            Rc::new(Node::make_branch(t_left.clone(), new_right))
                        }
                    }
                } else {
                    // The prefixes disagree.
                    Rc::new(Node::make_branch(s.clone(), t.clone()))
                }
            }
        }
    }

    /// Intersection of two trees. Combined tree should contain all keys that are found from both
    /// s and t. If duplicate keys are found, two nodes are passed to `leaf_combine` shall be
    /// called with values from s on the left hand side and values from t on the right hand side.
    fn intersect_trees(
        s: &Rc<Node<V>>,
        t: &Rc<Node<V>>,
        leaf_combine: &impl Fn(Rc<Node<V>>, Rc<Node<V>>) -> Option<Rc<Node<V>>>,
    ) -> Option<Rc<Node<V>>> {
        use Node::*;

        if Rc::ptr_eq(s, t) {
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
                        (Some(left), Some(right)) => Some(Rc::new(Self::make_branch(left, right))),
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
    fn is_tree_subset_of(s: &Rc<Node<V>>, t: &Rc<Node<V>>) -> bool {
        use Node::*;

        if Rc::ptr_eq(s, t) {
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

// Yes, the "deep" clone for a PatriciaTree is a shallow copy!
#[derive(Clone)]
pub(crate) struct PatriciaTree<V: Sized> {
    root: Option<Rc<Node<V>>>,
}

impl<V: Sized> PatriciaTree<V> {
    pub(crate) fn new() -> Self {
        Self { root: None }
    }

    pub(crate) fn clear(&mut self) {
        self.root = None;
    }

    pub(crate) fn is_empty(&self) -> bool {
        matches!(self.root, None)
    }

    // Not a very fast operation.
    pub(crate) fn len(&self) -> usize {
        self.iter().count()
    }

    fn apply_root_operation<F>(&mut self, op: F)
    where
        F: FnOnce(Option<Rc<Node<V>>>) -> Option<Rc<Node<V>>>,
    {
        let mut temp_root = None;
        std::mem::swap(&mut self.root, &mut temp_root);
        self.root = op(temp_root);
    }

    pub(crate) fn insert(&mut self, key: BitVec, value: V) {
        let new_leaf = Rc::new(Node::Leaf {
            key: key.clone(),
            value,
        });
        let node_op = move |_| Some(new_leaf);
        let root_op = move |root| Node::update_node_by_key(root, &key, node_op);
        self.apply_root_operation(root_op);
    }

    pub(crate) fn contains_key(&self, key: &BitVec) -> bool {
        !matches!(self.get(key), None)
    }

    pub(crate) fn get(&self, key: &BitVec) -> Option<&V> {
        use Node::*;
        let node = Node::<V>::find_leaf_by_key(self.root.as_ref(), key);
        match node {
            Some(leaf_node) => match leaf_node.as_ref() {
                Leaf { key: _, ref value } => Some(value),
                _ => panic!("Did not correctly get a leaf!"),
            },
            None => None,
        }
    }

    pub(crate) fn remove(&mut self, key: &BitVec) {
        let root_op = move |root| Node::update_node_by_key(root, key, |_| None);
        self.apply_root_operation(root_op);
    }

    pub(crate) fn iter(&self) -> PatriciaTreePostOrderIterator<V> {
        PatriciaTreePostOrderIterator::<V>::from_tree(self)
    }

    fn get_leaf_combine_with_value_op_semantics(
        value_op_on_duplicate_key: impl Fn(&V, &V) -> V,
    ) -> impl Fn(Rc<Node<V>>, Rc<Node<V>>) -> Option<Rc<Node<V>>> {
        use Node::*;

        move |one_leaf: Rc<Node<V>>, other_leaf: Rc<Node<V>>| match (
            one_leaf.as_ref(),
            other_leaf.as_ref(),
        ) {
            (
                Leaf {
                    key,
                    value: ref l_value,
                },
                Leaf {
                    key: _,
                    value: ref r_value,
                },
            ) => Some(Rc::new(Leaf {
                key: key.clone(),
                value: value_op_on_duplicate_key(l_value, r_value),
            })),
            _ => panic!("leaf_combine should only be called on leaves!"),
        }
    }

    pub(crate) fn union_with(
        &mut self,
        other: &Self,
        value_op_on_duplicate_key: impl Fn(&V, &V) -> V,
    ) {
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
    ) {
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

    fn next_leaf(&mut self, subtree: &'a Rc<Node<V>>) {
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
                    ref right,
                } => self.next_leaf(right),
                _ => panic!("Malformed Patricia Tree Iterator"),
            }
        }

        ret
    }

    fn into_tuple(node: Option<&Node<V>>) -> Option<(&BitVec, &V)> {
        match node {
            Some(leaf) => match leaf {
                Node::Leaf { ref key, ref value } => Some((key, value)),
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

        map2.insert(55.into(), 555);
        assert_eq!(map.len(), 4);
        assert_eq!(map2.len(), 5);

        map2.remove(&1.into());
        assert_eq!(map2.len(), 4);
        assert!(map.contains_key(&1.into()));
        assert!(!map2.contains_key(&1.into()));

        map2.remove(&1.into());
        assert_eq!(map2.len(), 4);
        assert!(!map2.contains_key(&1.into()));

        map2.remove(&22.into());
        assert_eq!(map2.len(), 3);
        assert!(!map2.contains_key(&22.into()));

        map2.remove(&13.into());
        assert_eq!(map2.len(), 2);
        assert!(!map2.contains_key(&13.into()));

        map2.remove(&42.into());
        assert_eq!(map2.len(), 1);

        map2.remove(&55.into());
        assert_eq!(map2.len(), 0);
    }
}
