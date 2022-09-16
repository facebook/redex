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
 * An abstract environment is a type of abstract domain that maps the variables
 * of a program to elements of a common abstract domain. For example, to perform
 * range analysis one can use an abstract environment that maps variable names
 * to intervals:
 *
 *   {"x" -> [-1, 1], "i" -> [0, 10], ...}
 *
 * Another example is descriptive type analysis for Dex code, where one computes
 * the set of all possible Java classes a register can hold a reference to at
 * any point in the code:
 *
 *  {"v0" -> {android.app.Fragment, java.lang.Object}, "v1" -> {...}, ...}
 *
 * This type of domain is commonly used for nonrelational (also called
 * attribute-independent) analyses that do not track relationships among
 * program variables. Please note that by definition of an abstract
 * environment, if the value _|_ appears in a variable binding, then no valid
 * execution state can ever be represented by this abstract environment. Hence,
 * assigning _|_ to a variable is equivalent to setting the entire environment
 * to _|_.
 */

pub trait AbstractEnvironment<V, D: AbstractDomain>: AbstractDomain {
    type ContainerType;
    fn bindings(&self) -> Option<&Self::ContainerType>;
    fn into_bindings(self) -> Option<Self::ContainerType>;

    fn len(&self) -> usize;
    fn is_empty(&self) -> bool;
    fn get(&self, variable: &V) -> Cow<'_, D>;
    fn set(&mut self, variable: V, domain: D);
    fn update(&mut self, variable: &V, op: impl FnOnce(&mut D));
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub enum HashMapAbstractEnvironment<V: Clone + Eq + Hash, D: AbstractDomain> {
    Value(HashMap<V, D>),
    Bottom,
}

impl<V, D> AbstractEnvironment<V, D> for HashMapAbstractEnvironment<V, D>
where
    V: Clone + Eq + Hash,
    D: AbstractDomain,
{
    type ContainerType = HashMap<V, D>;

    fn bindings(&self) -> Option<&HashMap<V, D>> {
        match self {
            Self::Value(ref map) => Some(map),
            _ => None,
        }
    }

    fn into_bindings(self) -> Option<HashMap<V, D>> {
        match self {
            Self::Value(map) => Some(map),
            _ => None,
        }
    }

    fn len(&self) -> usize {
        self.bindings()
            .expect("Bottom doesn't have a length!")
            .len()
    }

    fn is_empty(&self) -> bool {
        match self {
            Self::Value(map) => map.is_empty(),
            Self::Bottom => true,
        }
    }

    fn get(&self, variable: &V) -> Cow<'_, D> {
        let map = match self {
            Self::Value(map) => map,
            Self::Bottom => return Cow::Owned(D::bottom()),
        };

        match map.get(variable) {
            Some(domain) => Cow::Borrowed(domain),
            None => Cow::Owned(D::top()),
        }
    }

    fn set(&mut self, variable: V, domain: D) {
        if let Self::Value(map) = self {
            if domain.is_top() {
                map.remove(&variable);
            } else if domain.is_bottom() {
                *self = Self::Bottom;
            } else {
                map.insert(variable, domain);
            }
        }
    }

    fn update(&mut self, variable: &V, op: impl FnOnce(&mut D)) {
        if let Self::Value(map) = self {
            match map.get_mut(variable) {
                Some(explicit_value) => {
                    op(explicit_value);
                    if explicit_value.is_top() {
                        // Use implicit binding
                        map.remove(variable);
                    } else if explicit_value.is_bottom() {
                        *self = Self::Bottom;
                    }
                }
                None => {
                    let mut domain = D::top();
                    op(&mut domain);
                    if domain.is_top() {
                        // Do nothing. Continue to use implicit binding.
                    } else if domain.is_bottom() {
                        *self = Self::Bottom;
                    } else {
                        map.insert(variable.clone(), domain);
                    }
                }
            }
        }
    }
}

impl<V, D> AbstractDomain for HashMapAbstractEnvironment<V, D>
where
    V: Clone + Eq + Hash,
    D: AbstractDomain,
{
    fn bottom() -> Self {
        Self::Bottom
    }

    fn top() -> Self {
        Self::Value(HashMap::new())
    }

    fn is_bottom(&self) -> bool {
        matches!(self, Self::Bottom)
    }

    fn is_top(&self) -> bool {
        match self {
            Self::Value(map) => map.is_empty(),
            _ => false,
        }
    }

    fn leq(&self, rhs: &Self) -> bool {
        use HashMapAbstractEnvironment::*;
        match (self, rhs) {
            (Value(l_map), Value(r_map)) => {
                if l_map.len() < r_map.len() {
                    // Perf optimization
                    false
                } else {
                    for (l_k, l_v) in l_map.iter() {
                        if let Some(r_v) = r_map.get(l_k) {
                            if !l_v.leq(r_v) {
                                return false;
                            }
                        }
                    }

                    for (r_k, _) in r_map.iter() {
                        if l_map.get(r_k).is_none() {
                            return false;
                        }
                    }
                    true
                }
            }
            (Bottom, _) => true,
            (_, Bottom) => self.is_bottom(),
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

impl<V, D> HashMapAbstractEnvironment<V, D>
where
    V: Clone + Eq + Hash,
    D: AbstractDomain,
{
    fn join_like_operation(lhs: &mut Self, rhs: Self, operation: impl Fn(&mut D, D)) {
        use HashMapAbstractEnvironment::*;

        match (lhs, rhs) {
            (Value(l_map), Value(ref mut r_map)) => {
                l_map.retain(|l_k, _| r_map.contains_key(l_k));

                for (l_k, l_v) in l_map.iter_mut() {
                    let r_v = r_map.remove(l_k).unwrap();
                    operation(l_v, r_v);
                }
                l_map.retain(|_, l_v| !l_v.is_top());
            }
            (lhs @ Bottom, rhs) => *lhs = rhs,
            (_, Bottom) => {}
        }
    }

    fn meet_like_operation(lhs: &mut Self, rhs: Self, operation: impl Fn(&mut D, D)) {
        use HashMapAbstractEnvironment::*;

        match (&mut *lhs, rhs) {
            (Value(l_map), Value(r_map)) => {
                for (r_k, r_v) in r_map.into_iter() {
                    if let Some(l_v) = l_map.get_mut(&r_k) {
                        operation(l_v, r_v);
                        // l_v wasn't top. A meet-like operation should not make it top.
                        assert!(!l_v.is_top());
                        if l_v.is_bottom() {
                            *lhs = Bottom;
                            return;
                        }
                    } else {
                        // The value is Top, we just insert the other value (Top is the
                        // identity for meet-like operations).
                        l_map.insert(r_k, r_v);
                    }
                }
            }
            (Bottom, _) => {}
            (_, rhs @ Bottom) => {
                *lhs = rhs;
            }
        }
    }
}

#[derive(Debug)]
pub enum PatriciaTreeMapAbstractEnvironment<V: Into<BitVec> + Clone, D: Sized + Eq + AbstractDomain>
{
    Value(PatriciaTreeMap<V, D>), // Use empty map value as top
    Bottom,
}

impl<V, D> Clone for PatriciaTreeMapAbstractEnvironment<V, D>
where
    V: Into<BitVec> + Clone,
    D: Sized + AbstractDomain,
{
    fn clone(&self) -> Self {
        match self {
            Self::Value(map) => Self::Value(map.clone()),
            Self::Bottom => Self::Bottom,
        }
    }
}

impl<V, D> PartialEq for PatriciaTreeMapAbstractEnvironment<V, D>
where
    V: Into<BitVec> + Clone,
    D: Sized + AbstractDomain,
{
    fn eq(&self, rhs: &Self) -> bool {
        match (self, rhs) {
            (Self::Value(l_map), Self::Value(r_map)) => l_map == r_map,
            (Self::Bottom, Self::Bottom) => true,
            (_, _) => false,
        }
    }
}

impl<V, D> Eq for PatriciaTreeMapAbstractEnvironment<V, D>
where
    V: Into<BitVec> + Clone,
    D: Sized + AbstractDomain,
{
}

impl<V, D> AbstractEnvironment<V, D> for PatriciaTreeMapAbstractEnvironment<V, D>
where
    V: Into<BitVec> + Clone,
    D: Sized + AbstractDomain,
{
    type ContainerType = PatriciaTreeMap<V, D>;

    fn bindings(&self) -> Option<&PatriciaTreeMap<V, D>> {
        match self {
            Self::Value(ref map) => Some(map),
            _ => None,
        }
    }

    fn into_bindings(self) -> Option<PatriciaTreeMap<V, D>> {
        match self {
            Self::Value(map) => Some(map),
            _ => None,
        }
    }

    fn len(&self) -> usize {
        self.bindings()
            .expect("Bottom doesn't have a length!")
            .len()
    }

    fn is_empty(&self) -> bool {
        match self {
            Self::Value(map) => map.is_empty(),
            Self::Bottom => true,
        }
    }

    fn get(&self, variable: &V) -> Cow<'_, D> {
        let map = match self {
            Self::Value(map) => map,
            Self::Bottom => return Cow::Owned(D::bottom()),
        };

        match map.get(variable.clone()) {
            Some(domain) => Cow::Borrowed(domain),
            None => Cow::Owned(D::top()),
        }
    }

    fn set(&mut self, variable: V, domain: D) {
        if let Self::Value(map) = self {
            if domain.is_top() {
                map.remove(variable);
            } else if domain.is_bottom() {
                *self = Self::Bottom;
            } else {
                map.upsert(variable, domain);
            }
        }
    }

    fn update(&mut self, variable: &V, op: impl FnOnce(&mut D)) {
        let map = match self {
            Self::Bottom => return,
            Self::Value(ref mut map) => map,
        };

        let mut update_domain = match map.get(variable.clone()) {
            Some(domain) => domain.clone(),
            None => D::top(),
        };

        op(&mut update_domain);

        self.set(variable.clone(), update_domain);
    }
}

impl<V, D> AbstractDomain for PatriciaTreeMapAbstractEnvironment<V, D>
where
    V: Into<BitVec> + Clone,
    D: Sized + AbstractDomain,
{
    fn bottom() -> Self {
        Self::Bottom
    }

    fn top() -> Self {
        Self::Value(PatriciaTreeMap::new())
    }

    fn is_bottom(&self) -> bool {
        matches!(self, Self::Bottom)
    }

    fn is_top(&self) -> bool {
        match self {
            Self::Value(map) => map.is_empty(),
            _ => false,
        }
    }

    fn leq(&self, rhs: &Self) -> bool {
        use PatriciaTreeMapAbstractEnvironment::*;

        match (self, rhs) {
            (Value(self_map), Value(other_map)) => self_map.leq(other_map, &D::top()),
            (Bottom, _) => true,
            (_, Bottom) => self.is_bottom(),
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

impl<V, D> PatriciaTreeMapAbstractEnvironment<V, D>
where
    V: Into<BitVec> + Clone,
    D: Sized + AbstractDomain,
{
    fn join_like_operation(lhs: &mut Self, rhs: Self, operation: impl Fn(&mut D, D)) {
        use PatriciaTreeMapAbstractEnvironment::*;

        match (lhs, rhs) {
            (Value(lmap), Value(rmap)) => {
                lmap.intersect_with(&rmap, |s, t| {
                    let mut s = s.clone();
                    operation(&mut s, t.clone());
                    s
                });
            }
            (lhs @ Bottom, rhs) => *lhs = rhs,
            (_, Bottom) => {}
        }
    }

    fn meet_like_operation(lhs: &mut Self, rhs: Self, operation: impl Fn(&mut D, D)) {
        use PatriciaTreeMapAbstractEnvironment::*;

        match (lhs, rhs) {
            (Value(lmap), Value(rmap)) => {
                lmap.union_with(&rmap, |s, t| {
                    let mut s = s.clone();
                    operation(&mut s, t.clone());
                    s
                });
            }
            (Bottom, _) => {}
            (lhs, rhs @ Bottom) => {
                *lhs = rhs;
            }
        }
    }
}
