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

// Interface structs for PatriciaTreeMap. Does not require V to impl Clone.
pub struct PatriciaTreeMap<K: Into<BitVec>, V> {
    storage: PatriciaTree<V>,
    _key_type_phantom: PhantomData<K>,
}

impl<K: Into<BitVec>, V> PatriciaTreeMap<K, V> {
    pub fn new() -> Self {
        Self {
            storage: PatriciaTree::<V>::new(),
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

    pub fn upsert(&mut self, key: K, value: V) {
        self.storage.insert(key.into(), value)
    }

    pub fn contains_key(&self, key: K) -> bool {
        self.storage.contains_key(&key.into())
    }

    pub fn get(&self, key: K) -> Option<&V> {
        self.storage.get(&key.into())
    }

    pub fn remove(&mut self, key: K) {
        self.storage.remove(&key.into())
    }

    pub fn iter(&self) -> PatriciaTreeMapIterator<'_, K, V> {
        self.storage.iter().into()
    }
}

impl<K: Into<BitVec>, V> Default for PatriciaTreeMap<K, V> {
    fn default() -> Self {
        Self::new()
    }
}

impl<K: Into<BitVec>, V: Eq> PartialEq for PatriciaTreeMap<K, V> {
    fn eq(&self, other: &Self) -> bool {
        self.storage.eq(&other.storage)
    }
}

impl<K: Into<BitVec>, V: Eq> Eq for PatriciaTreeMap<K, V> {}

impl<K: Into<BitVec>, V> Clone for PatriciaTreeMap<K, V> {
    fn clone(&self) -> Self {
        Self {
            storage: self.storage.clone(),
            ..Default::default()
        }
    }
}

pub struct PatriciaTreeMapIterator<'a, K: Into<BitVec>, V> {
    iter_impl: PatriciaTreePostOrderIterator<'a, V>,
    _key_type_phantom: PhantomData<K>,
}

impl<'a, K: Into<BitVec>, V> From<PatriciaTreePostOrderIterator<'a, V>>
    for PatriciaTreeMapIterator<'a, K, V>
{
    fn from(iter_impl: PatriciaTreePostOrderIterator<'a, V>) -> Self {
        Self {
            iter_impl,
            _key_type_phantom: Default::default(),
        }
    }
}

impl<'a, K: 'a + Into<BitVec> + From<&'a BitVec>, V> Iterator
    for PatriciaTreeMapIterator<'a, K, V>
{
    type Item = (K, &'a V);

    fn next(&mut self) -> Option<Self::Item> {
        let wrapped = self.iter_impl.next();
        wrapped.map(|(key, value)| (key.into(), value))
    }
}

impl<K: Into<BitVec>, V> FromIterator<(K, V)> for PatriciaTreeMap<K, V> {
    fn from_iter<I: IntoIterator<Item = (K, V)>>(iter: I) -> Self {
        let mut ret: PatriciaTreeMap<K, V> = PatriciaTreeMap::new();
        for (k, v) in iter {
            ret.upsert(k, v);
        }
        ret
    }
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;

    use crate::datatype::PatriciaTreeMap;

    #[test]
    fn test_map_operations() {
        let num_vals: usize = 10000;

        let mut items: HashMap<usize, String> = HashMap::new();

        for i in 0..num_vals {
            items.insert(i, format!("{}", i));
        }

        let map: PatriciaTreeMap<_, _> = items.clone().into_iter().collect();

        assert_eq!(map.len(), 10000);
        assert_eq!(*map.get(15).unwrap(), "15".to_string());
        assert_eq!(*map.get(9999).unwrap(), "9999".to_string());
        assert_eq!(map.get(10000), None);

        assert!(map == map);
        let mut map2 = map.clone();
        assert!(map == map2);
        map2.upsert(0xabcdef12, "0xabcdef12".to_owned());
        assert!(map != map2);
        map2.remove(0xabcdef12);
        assert!(map == map2)
    }
}
