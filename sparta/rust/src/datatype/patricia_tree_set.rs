/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::iter::FromIterator;
use std::iter::Iterator;
use std::marker::PhantomData;

use super::powerset::SetAbstractDomainOps;
use crate::datatype::bitvec::BitVec;
use crate::datatype::patricia_tree_impl::PatriciaTree;
use crate::datatype::patricia_tree_impl::PatriciaTreePostOrderIterator;

// Interface structs for PatriciaTree
#[derive(Clone)]
pub struct PatriciaTreeSet<K: Into<BitVec>> {
    storage: PatriciaTree<()>,
    _key_type_phantom: PhantomData<K>,
}

impl<K: Into<BitVec>> PatriciaTreeSet<K> {
    pub fn new() -> Self {
        Self {
            storage: PatriciaTree::<()>::new(),
            _key_type_phantom: Default::default(),
        }
    }

    pub fn clear(&mut self) {
        self.storage.clear();
    }

    pub fn is_empty(&self) -> bool {
        self.storage.is_empty()
    }

    // Not a very fast operation.
    pub fn len(&self) -> usize {
        self.storage.len()
    }

    pub fn insert(&mut self, key: K) {
        self.storage.insert(key.into(), ())
    }

    pub fn contains(&self, key: K) -> bool {
        self.storage.contains_key(&key.into())
    }

    pub fn remove(&mut self, key: K) {
        self.storage.remove(&key.into())
    }

    pub fn iter(&self) -> PatriciaTreeSetIterator<'_, K> {
        self.storage.iter().into()
    }
}

impl<K: Into<BitVec>> Default for PatriciaTreeSet<K> {
    fn default() -> Self {
        Self::new()
    }
}

impl<K: Into<BitVec>> PartialEq for PatriciaTreeSet<K> {
    fn eq(&self, other: &Self) -> bool {
        self.storage.eq(&other.storage)
    }
}

impl<K: Into<BitVec>> Eq for PatriciaTreeSet<K> {}

pub struct PatriciaTreeSetIterator<'a, K: Into<BitVec>> {
    iter_impl: PatriciaTreePostOrderIterator<'a, ()>,
    _key_type_phantom: PhantomData<K>,
}

impl<'a, K: Into<BitVec>> From<PatriciaTreePostOrderIterator<'a, ()>>
    for PatriciaTreeSetIterator<'a, K>
{
    fn from(iter_impl: PatriciaTreePostOrderIterator<'a, ()>) -> Self {
        Self {
            iter_impl,
            _key_type_phantom: Default::default(),
        }
    }
}

impl<'a, K: 'a + Into<BitVec> + From<&'a BitVec>> Iterator for PatriciaTreeSetIterator<'a, K> {
    type Item = K;

    fn next(&mut self) -> Option<Self::Item> {
        let wrapped = self.iter_impl.next();
        wrapped.map(|(key, _)| key.into())
    }
}

impl<K: Into<BitVec>> FromIterator<K> for PatriciaTreeSet<K> {
    fn from_iter<I: IntoIterator<Item = K>>(iter: I) -> Self {
        let mut ret: PatriciaTreeSet<K> = PatriciaTreeSet::new();
        for item in iter {
            ret.insert(item);
        }
        ret
    }
}

impl<K> SetAbstractDomainOps for PatriciaTreeSet<K>
where
    K: Into<BitVec> + Clone,
{
    fn is_subset(&self, other: &Self) -> bool {
        self.storage.subset_of(&other.storage)
    }

    fn intersection_with(&mut self, other: &Self) {
        self.storage.intersect_with(&other.storage, |_, _| ())
    }

    fn union_with(&mut self, other: Self) {
        self.storage.union_with(&other.storage, |_, _| ())
    }
}

impl<K, const N: usize> From<[K; N]> for PatriciaTreeSet<K>
where
    K: Into<BitVec> + Clone,
{
    fn from(arr: [K; N]) -> Self {
        arr.into_iter().collect()
    }
}

#[cfg(test)]
mod tests {
    use std::collections::HashSet;

    use rand::Rng;

    use crate::datatype::powerset::SetAbstractDomainOps;
    use crate::datatype::PatriciaTreeSet;

    #[test]
    fn test_set_operations() {
        let num_vals: u32 = 10000;

        let items: HashSet<u32> = (0..num_vals).collect();
        let mut set: PatriciaTreeSet<u32> = items.iter().cloned().collect();
        let out_items: HashSet<u32> = set.iter().collect();

        assert_eq!(items, out_items);
        assert_eq!(set.len(), 10000);
        assert!(!set.contains(10001));

        set.insert(200); // Nothing bad happens
        set.insert(10200); // Nothing bad happens

        assert_eq!(set.len(), 10001);
    }

    #[test]
    fn test_robustness_with_rng() {
        let mut rng = rand::thread_rng();

        let mut items: HashSet<u32> = HashSet::new();
        let mut set: PatriciaTreeSet<u32> = PatriciaTreeSet::new();

        let num_vals = 10000;
        for _ in 0..num_vals {
            let i: u32 = rng.gen();
            items.insert(i);
            set.insert(i);
        }

        let out_items: HashSet<u32> = set.iter().collect();

        for item in items.iter() {
            assert!(set.contains(*item));
        }

        assert_eq!(items, out_items);
    }

    #[test]
    fn test_union_operation_empty() {
        let mut set1 = PatriciaTreeSet::<u32>::from([1, 2, 3]);
        let set2 = PatriciaTreeSet::<u32>::from([]);
        set1.union_with(set2);

        let actual: HashSet<_> = set1.iter().collect();
        let expected = HashSet::<u32>::from([1, 2, 3]);

        assert_eq!(actual, expected);
    }

    #[test]
    fn test_simple_union_operation() {
        let mut set1 = PatriciaTreeSet::<u32>::from([1, 2, 3, 64, 99]);
        let set2 = PatriciaTreeSet::<u32>::from([4, 5, 6, 65, 102]);
        set1.union_with(set2);

        let actual: HashSet<_> = set1.iter().collect();
        let expected = HashSet::<u32>::from([1, 2, 3, 4, 5, 6, 64, 65, 99, 102]);

        assert_eq!(actual, expected);
    }

    #[test]
    fn test_union_operation_with_dup() {
        let mut set1 = PatriciaTreeSet::<u32>::from([1, 2, 3, 4, 5]);
        let set2 = PatriciaTreeSet::<u32>::from([4, 5, 6, 7, 8]);
        set1.union_with(set2);

        let actual: HashSet<_> = set1.iter().collect();
        let expected = HashSet::<u32>::from([1, 2, 3, 4, 5, 6, 7, 8]);

        assert_eq!(actual, expected);
    }

    #[test]
    fn test_is_subset() {
        let set1 = PatriciaTreeSet::<u32>::from([]);
        let set2 = PatriciaTreeSet::<u32>::from([]);
        assert!(set1.is_subset(&set2));
        assert!(set2.is_subset(&set1));

        let set1 = PatriciaTreeSet::<u32>::from([1, 2]);
        let set2 = PatriciaTreeSet::<u32>::from([1, 2, 3]);
        assert!(set1.is_subset(&set2));

        let set1 = PatriciaTreeSet::<u32>::from([203, 345, 324]);
        let mut set2 = PatriciaTreeSet::<u32>::from([203, 234]);
        assert!(!set1.is_subset(&set2));

        set2.union_with(set1.clone());
        assert!(set1.is_subset(&set2));

        let set1 = PatriciaTreeSet::<u32>::from([10, 20, 30]);
        let mut set2 = PatriciaTreeSet::<u32>::from([1, 2, 3, 4, 5]);
        assert!(!set1.is_subset(&set2));

        set2.union_with(set1.clone());
        assert!(set1.is_subset(&set2));
    }

    #[test]
    fn test_intersect_operation_empty() {
        let mut set1 = PatriciaTreeSet::<u32>::from([1, 2, 3]);
        let set2 = PatriciaTreeSet::<u32>::from([]);
        set1.intersection_with(&set2);

        let actual: HashSet<_> = set1.iter().collect();
        let expected = HashSet::<u32>::from([]);

        assert_eq!(actual, expected);

        let mut set1 = PatriciaTreeSet::<u32>::from([]);
        let set2 = PatriciaTreeSet::<u32>::from([]);
        set1.intersection_with(&set2);

        let actual: HashSet<_> = set1.iter().collect();
        let expected = HashSet::<u32>::from([]);

        assert_eq!(actual, expected);
    }

    #[test]
    fn test_simple_intersect_operation() {
        let mut set1 = PatriciaTreeSet::<u32>::from([0, 1, 2, 3, 4]);
        let set2 = PatriciaTreeSet::<u32>::from([2, 3, 4, 5, 6]);
        set1.intersection_with(&set2);

        let actual: HashSet<_> = set1.iter().collect();
        let expected = HashSet::<u32>::from([2, 3, 4]);

        assert_eq!(actual, expected);

        let mut set1 = PatriciaTreeSet::<u32>::from([0, 1, 2]);
        let set2 = PatriciaTreeSet::<u32>::from([3, 4, 5, 6]);
        set1.intersection_with(&set2);

        let actual: HashSet<_> = set1.iter().collect();
        let expected = HashSet::<u32>::from([]);

        assert_eq!(actual, expected);
    }
}
