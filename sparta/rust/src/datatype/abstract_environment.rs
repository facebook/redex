/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::borrow::Cow;
use std::collections::HashMap;
use std::hash::Hash;

use crate::datatype::AbstractDomain;

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
    fn update(&mut self, variable: &V, op: impl Fn(&mut D));
}

#[derive(Clone, PartialEq, Eq)]
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
            HashMapAbstractEnvironment::Value(ref map) => Some(map),
            _ => None,
        }
    }

    fn into_bindings(self) -> Option<HashMap<V, D>> {
        match self {
            HashMapAbstractEnvironment::Value(map) => Some(map),
            _ => None,
        }
    }

    fn len(&self) -> usize {
        self.bindings()
            .expect("Bottom doesn't have a length!")
            .len()
    }

    fn is_empty(&self) -> bool {
        use HashMapAbstractEnvironment::*;
        match self {
            Value(map) => map.is_empty(),
            Bottom => true,
        }
    }

    fn get(&self, variable: &V) -> Cow<'_, D> {
        use HashMapAbstractEnvironment::*;
        let map = match self {
            Value(map) => map,
            Bottom => return Cow::Owned(D::bottom()),
        };

        match map.get(variable) {
            Some(domain) => Cow::Borrowed(domain),
            None => Cow::Owned(D::top()),
        }
    }

    fn set(&mut self, variable: V, domain: D) {
        use HashMapAbstractEnvironment::*;
        if let Value(map) = self {
            if domain.is_top() {
                map.remove(&variable);
            } else if domain.is_bottom() {
                *self = Bottom;
            } else {
                map.insert(variable, domain);
            }
        }
    }

    fn update(&mut self, variable: &V, op: impl FnOnce(&mut D)) {
        use HashMapAbstractEnvironment::*;

        if let Value(map) = self {
            match map.get_mut(variable) {
                Some(explicit_value) => {
                    op(explicit_value);
                    if explicit_value.is_top() {
                        // Use implicit binding
                        map.remove(variable);
                    } else if explicit_value.is_bottom() {
                        *self = Bottom;
                    }
                }
                None => {
                    let mut domain = D::top();
                    op(&mut domain);
                    if domain.is_top() {
                        // Do nothing. Continue to use implicit binding.
                    } else if domain.is_bottom() {
                        *self = Bottom;
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
        HashMapAbstractEnvironment::Bottom
    }

    fn top() -> Self {
        HashMapAbstractEnvironment::Value(HashMap::new())
    }

    fn is_bottom(&self) -> bool {
        matches!(self, HashMapAbstractEnvironment::Bottom)
    }

    fn is_top(&self) -> bool {
        match self {
            HashMapAbstractEnvironment::Value(map) => map.is_empty(),
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

        match (&mut (*lhs), rhs) {
            (Value(l_map), Value(ref mut r_map)) => {
                l_map.retain(|l_k, _| r_map.contains_key(l_k));

                for (l_k, l_v) in l_map.iter_mut() {
                    let r_v = r_map.remove(l_k).unwrap();
                    operation(l_v, r_v);
                }
                l_map.retain(|_, l_v| !l_v.is_top());
            }
            (Bottom, rhs) => *lhs = rhs,
            (_, Bottom) => {}
        }
    }

    fn meet_like_operation(lhs: &mut Self, rhs: Self, operation: impl Fn(&mut D, D)) {
        use HashMapAbstractEnvironment::*;

        if lhs.is_bottom() {
            return;
        }

        if rhs.is_bottom() {
            *lhs = rhs;
            return;
        }

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
            (_, Bottom) => {
                *lhs = Bottom;
            }
        }
    }
}
