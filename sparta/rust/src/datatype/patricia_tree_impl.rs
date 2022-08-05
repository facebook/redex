/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::rc::Rc;
use std::string::ToString;

use crate::datatype::bitvec::BitVec;

// Implementation of the data structure

// TODO items:
// -- remove algorithm
// -- intersection/union algorithms

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

    fn key_or_prefix<'a>(&'a self) -> &'a BitVec {
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
        match self.root {
            None => true,
            _ => false,
        }
    }

    // Not a very fast operation.
    pub(crate) fn len(&self) -> usize {
        self.iter().count()
    }

    fn insert_leaf(maybe_node: Option<Rc<Node<V>>>, new_leaf: Rc<Node<V>>) -> Rc<Node<V>> {
        use Node::*;

        if let Some(node) = maybe_node {
            match *node {
                Leaf { ref key, value: _ } => {
                    if key == new_leaf.key_or_prefix() {
                        new_leaf
                    } else {
                        Rc::new(Node::make_branch(new_leaf, node.clone()))
                    }
                }
                Branch {
                    ref prefix,
                    ref left,
                    ref right,
                } => {
                    if new_leaf.key_or_prefix().begins_with(prefix) {
                        let branching_bit = new_leaf.key_or_prefix().get(prefix.len());
                        if !branching_bit {
                            let new_left = Self::insert_leaf(Some(left.clone()), new_leaf);
                            Rc::new(Node::make_branch(new_left, right.clone()))
                        } else {
                            let new_right = Self::insert_leaf(Some(right.clone()), new_leaf);
                            Rc::new(Node::make_branch(left.clone(), new_right))
                        }
                    } else {
                        // Branch differs, create new branch like how you'd do with another leaf.
                        Rc::new(Node::make_branch(new_leaf, node))
                    }
                }
            }
        } else {
            new_leaf
        }
    }

    pub(crate) fn insert(&mut self, key: BitVec, value: V) {
        let new_leaf = Rc::new(Node::Leaf { key, value });
        let mut temp_root = None;
        std::mem::swap(&mut self.root, &mut temp_root);
        self.root = Some(Self::insert_leaf(temp_root, new_leaf));
    }

    pub(crate) fn contains_key(&self, key: &BitVec) -> bool {
        match self.get(key) {
            None => false,
            _ => true,
        }
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

    pub(crate) fn iter(&self) -> PatriciaTreePostOrderIterator<V> {
        PatriciaTreePostOrderIterator::<V>::from_tree(self)
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

    fn into_tuple<'n>(node: Option<&'n Node<V>>) -> Option<(&'n BitVec, &'n V)> {
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
    }
}
