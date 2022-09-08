/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::borrow::Cow;
use std::collections::HashMap;
use std::hash::Hash;

use crate::datatype::bitvec::BitVec;
use crate::datatype::AbstractDomain;
use crate::datatype::PatriciaTreeMap;

/*
 * A partition is a mapping from a set of labels to elements in an abstract
 * domain. It denotes a union of properties. A partition is Bottom iff all its
 * bindings are set to Bottom, and it is Top iff all its bindings are set to
 * Top.
 *
 * All lattice operations are applied componentwise.
 */
pub trait AbstractPartition<L, D: AbstractDomain>: AbstractDomain {
    type ContainerType;
    fn bindings(&self) -> Option<&Self::ContainerType>;
    fn into_bindings(self) -> Option<Self::ContainerType>;

    fn len(&self) -> usize;
    fn is_empty(&self) -> bool;
    fn get(&self, label: &L) -> Cow<'_, D>;
    fn set(&mut self, label: L, domain: D);
    fn update(&mut self, label: &L, op: impl Fn(&mut D));
}

/*
 * In order to minimize the size of the hashtable, we do not explicitly
 * represent bindings to Bottom.
 *
 * This implementation differs slightly from the textbook definition of a
 * partition: our Top partition cannot have its labels re-bound to anything
 * other than Top. I.e. for all labels L and domains D,
 *
 *   HashMapAbstractPartition::top().set(L, D) == HashMapAbstractPartition::top()
 *
 * This makes for a much simpler implementation.
 */
#[derive(Clone, PartialEq, Eq)]
pub enum HashMapAbstractPartition<L: Clone + Eq + Hash, D: AbstractDomain> {
    Top,
    Value(HashMap<L, D>), // Use empty map value as bottom
}

impl<L, D> AbstractPartition<L, D> for HashMapAbstractPartition<L, D>
where
    L: Clone + Eq + Hash,
    D: AbstractDomain,
{
    type ContainerType = HashMap<L, D>;

    fn bindings(&self) -> Option<&Self::ContainerType> {
        match self {
            Self::Value(ref map) => Some(map),
            _ => None,
        }
    }

    fn into_bindings(self) -> Option<Self::ContainerType> {
        match self {
            Self::Value(map) => Some(map),
            _ => None,
        }
    }

    fn len(&self) -> usize {
        self.bindings()
            .expect("Top abstract domain doesn't have a length!")
            .len()
    }

    fn is_empty(&self) -> bool {
        match self {
            Self::Top => false,
            Self::Value(map) => map.is_empty(),
        }
    }

    fn get(&self, label: &L) -> Cow<'_, D> {
        let map = match self {
            Self::Top => return Cow::Owned(D::top()),
            Self::Value(map) => map,
        };

        match map.get(label) {
            Some(domain) => Cow::Borrowed(domain),
            None => Cow::Owned(D::bottom()),
        }
    }

    fn set(&mut self, label: L, domain: D) {
        let map = match self {
            Self::Top => return,
            Self::Value(ref mut map) => map,
        };

        // Save some memory by implicitly storing bottom.
        if domain.is_bottom() {
            map.remove(&label);
        } else {
            map.insert(label, domain);
        }
    }

    fn update(&mut self, label: &L, op: impl FnOnce(&mut D)) {
        let map = match self {
            Self::Top => return,
            Self::Value(ref mut map) => map,
        };

        match map.get_mut(label) {
            Some(domain) => op(domain),
            None => {
                let mut temp = D::bottom();
                op(&mut temp);
                if !temp.is_bottom() {
                    map.insert(label.clone(), temp);
                }
            }
        }
    }
}

impl<L, D> AbstractDomain for HashMapAbstractPartition<L, D>
where
    L: Clone + Eq + Hash,
    D: AbstractDomain,
{
    fn bottom() -> Self {
        Self::Value(HashMap::new())
    }

    fn top() -> Self {
        Self::Top
    }

    fn is_bottom(&self) -> bool {
        match self {
            Self::Value(map) => map.is_empty(),
            _ => false,
        }
    }

    fn is_top(&self) -> bool {
        matches!(self, Self::Top)
    }

    fn leq(&self, rhs: &Self) -> bool {
        use HashMapAbstractPartition::*;

        match (self, rhs) {
            (Top, _) => rhs.is_top(),
            (_, Top) => true,
            (Value(self_map), Value(other_map)) => {
                if self_map.len() > other_map.len() {
                    // Perf optimization
                    false
                } else {
                    for (k, v) in self_map.iter() {
                        match other_map.get(k) {
                            Some(rd) => {
                                if !v.leq(rd) {
                                    return false;
                                }
                            }
                            None => return false,
                        }
                    }
                    true
                }
            }
        }
    }

    fn join_with(&mut self, rhs: Self) {
        Self::join_like_operation(self, rhs, |d1, d2| d1.join_with(d2));
    }

    fn meet_with(&mut self, rhs: Self) {
        Self::meet_like_operation(self, rhs, |d1, d2| d1.meet_with(d2));
    }

    fn widen_with(&mut self, rhs: Self) {
        Self::join_like_operation(self, rhs, |d1, d2| d1.widen_with(d2));
    }

    fn narrow_with(&mut self, rhs: Self) {
        Self::meet_like_operation(self, rhs, |d1, d2| d1.narrow_with(d2));
    }
}

impl<L, D> HashMapAbstractPartition<L, D>
where
    L: Clone + Eq + Hash,
    D: AbstractDomain,
{
    fn join_like_operation(lhs: &mut Self, rhs: Self, operation: impl Fn(&mut D, D)) {
        use HashMapAbstractPartition::*;

        match (lhs, rhs) {
            (Top, _) => {}
            (lhs, Top) => {
                *lhs = Top;
            }
            (Value(l_map), Value(r_map)) => {
                for (r_k, r_v) in r_map.into_iter() {
                    if let Some(l_v) = l_map.get_mut(&r_k) {
                        operation(l_v, r_v);
                        // l_v wasn't bottom. A join-like operation should not make it bottom.
                        assert!(!l_v.is_bottom());
                    } else {
                        // The value is Bottom, we just insert the other value (Bottom is the
                        // identity for join-like operations).
                        l_map.insert(r_k, r_v);
                    }
                }
            }
        }
    }

    fn meet_like_operation(lhs: &mut Self, rhs: Self, operation: impl Fn(&mut D, D)) {
        use HashMapAbstractPartition::*;

        match (lhs, rhs) {
            (lhs @ Top, rhs) => {
                *lhs = rhs;
            }
            (_, Top) => {}
            (Value(l_map), Value(mut r_map)) => {
                l_map.retain(|l_k, _| r_map.contains_key(l_k));

                for (l_k, l_v) in l_map.iter_mut() {
                    let r_v = r_map.remove(l_k).unwrap();
                    operation(l_v, r_v);
                }

                l_map.retain(|_, l_v| !l_v.is_bottom());
            }
        }
    }
}

pub enum PatriciaTreeMapAbstractPartition<L: Into<BitVec> + Clone, D: Sized + Eq + AbstractDomain> {
    Top,
    Value(PatriciaTreeMap<L, D>), // Use empty map value as bottom
}

impl<L, D> Clone for PatriciaTreeMapAbstractPartition<L, D>
where
    L: Into<BitVec> + Clone,
    D: Sized + AbstractDomain,
{
    fn clone(&self) -> Self {
        use PatriciaTreeMapAbstractPartition::*;

        match self {
            Top => Top,
            Value(map) => Value(map.clone()),
        }
    }
}

impl<L, D> PartialEq for PatriciaTreeMapAbstractPartition<L, D>
where
    L: Into<BitVec> + Clone,
    D: Sized + AbstractDomain,
{
    fn eq(&self, rhs: &Self) -> bool {
        use PatriciaTreeMapAbstractPartition::*;

        match (self, rhs) {
            (Top, Top) => true,
            (Value(l_map), Value(r_map)) => l_map == r_map,
            (_, _) => false,
        }
    }
}

impl<L, D> Eq for PatriciaTreeMapAbstractPartition<L, D>
where
    L: Into<BitVec> + Clone,
    D: Sized + AbstractDomain,
{
}

impl<L, D> AbstractPartition<L, D> for PatriciaTreeMapAbstractPartition<L, D>
where
    L: Into<BitVec> + Clone,
    D: Sized + AbstractDomain,
{
    type ContainerType = PatriciaTreeMap<L, D>;

    fn bindings(&self) -> Option<&Self::ContainerType> {
        match self {
            Self::Value(ref map) => Some(map),
            _ => None,
        }
    }

    fn into_bindings(self) -> Option<Self::ContainerType> {
        match self {
            Self::Value(map) => Some(map),
            _ => None,
        }
    }

    fn len(&self) -> usize {
        self.bindings()
            .expect("Top abstract domain doesn't have a length!")
            .len()
    }

    fn is_empty(&self) -> bool {
        match self {
            Self::Top => false,
            Self::Value(map) => map.is_empty(),
        }
    }

    fn get(&self, label: &L) -> Cow<'_, D> {
        let map = match self {
            Self::Top => return Cow::Owned(D::top()),
            Self::Value(map) => map,
        };

        match map.get(label.clone()) {
            Some(domain) => Cow::Borrowed(domain),
            None => Cow::Owned(D::bottom()),
        }
    }

    fn set(&mut self, label: L, domain: D) {
        let map = match self {
            Self::Top => return,
            Self::Value(ref mut map) => map,
        };

        // Save some memory by implicitly storing bottom.
        if domain.is_bottom() {
            map.remove(label);
        } else {
            map.upsert(label, domain);
        }
    }

    fn update(&mut self, label: &L, op: impl FnOnce(&mut D)) {
        let map = match self {
            Self::Top => return,
            Self::Value(ref mut map) => map,
        };

        let mut update_domain = match map.get(label.clone()) {
            Some(domain) => domain.clone(),
            None => D::bottom(),
        };

        op(&mut update_domain);

        self.set(label.clone(), update_domain);
    }
}

impl<L, D> AbstractDomain for PatriciaTreeMapAbstractPartition<L, D>
where
    L: Into<BitVec> + Clone,
    D: Sized + AbstractDomain,
{
    fn bottom() -> Self {
        Self::Value(PatriciaTreeMap::new())
    }

    fn top() -> Self {
        Self::Top
    }

    fn is_bottom(&self) -> bool {
        match self {
            Self::Value(map) => map.is_empty(),
            _ => false,
        }
    }

    fn is_top(&self) -> bool {
        matches!(self, Self::Top)
    }

    fn leq(&self, rhs: &Self) -> bool {
        use PatriciaTreeMapAbstractPartition::*;

        match (self, rhs) {
            (Top, _) => rhs.is_top(),
            (_, Top) => true,
            (Value(self_map), Value(other_map)) => self_map.leq(other_map, &D::bottom()),
        }
    }

    fn join_with(&mut self, rhs: Self) {
        Self::join_like_operation(self, rhs, |d1, d2| d1.join_with(d2));
    }

    fn meet_with(&mut self, rhs: Self) {
        Self::meet_like_operation(self, rhs, |d1, d2| d1.meet_with(d2));
    }

    fn widen_with(&mut self, rhs: Self) {
        Self::join_like_operation(self, rhs, |d1, d2| d1.widen_with(d2));
    }

    fn narrow_with(&mut self, rhs: Self) {
        Self::meet_like_operation(self, rhs, |d1, d2| d1.narrow_with(d2));
    }
}

impl<L, D> PatriciaTreeMapAbstractPartition<L, D>
where
    L: Into<BitVec> + Clone,
    D: Sized + AbstractDomain,
{
    fn join_like_operation(lhs: &mut Self, rhs: Self, operation: impl Fn(&mut D, D)) {
        use PatriciaTreeMapAbstractPartition::*;

        match (lhs, rhs) {
            (Top, _) => {}
            (lhs, rhs @ Top) => {
                *lhs = rhs;
            }
            (Value(lmap), Value(rmap)) => {
                lmap.union_with(&rmap, |s, t| {
                    let mut s = s.clone();
                    operation(&mut s, t.clone());
                    s
                });
            }
        }
    }

    fn meet_like_operation(lhs: &mut Self, rhs: Self, operation: impl Fn(&mut D, D)) {
        use PatriciaTreeMapAbstractPartition::*;

        match (lhs, rhs) {
            (lhs @ Top, rhs) => {
                *lhs = rhs;
            }
            (_, Top) => {}
            (Value(lmap), Value(rmap)) => {
                lmap.intersect_with(&rmap, |s, t| {
                    let mut s = s.clone();
                    operation(&mut s, t.clone());
                    s
                });
            }
        }
    }
}
