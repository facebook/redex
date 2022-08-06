/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::iter::FromIterator;
use std::iter::Iterator;
use std::marker::PhantomData;

use crate::datatype::bitvec::BitVec;
use crate::datatype::patricia_tree_impl::PatriciaTree;
use crate::datatype::patricia_tree_impl::PatriciaTreePostOrderIterator;

// Interface structs for PatriciaTree

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

    pub fn iter(&self) -> PatriciaTreeSetIterator<'_, K> {
        self.storage.iter().into()
    }
}

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

#[cfg(test)]
mod tests {
    use std::collections::HashSet;

    use rand::Rng;

    use crate::datatype::PatriciaTreeSet;

    #[test]
    fn test_set_operations() {
        let num_vals: u32 = 10000;

        let items: HashSet<u32> = (0..num_vals).collect();
        let mut set: PatriciaTreeSet<u32> = items.clone().into_iter().collect();
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
}
