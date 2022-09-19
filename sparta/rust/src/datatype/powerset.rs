/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::collections::HashSet;
use std::iter::FromIterator;

use super::abstract_domain::AbstractDomain;
use crate::datatype::PatriciaTreeSet;

pub trait SetAbstractDomainOps: Clone + Eq {
    fn is_subset(&self, other: &Self) -> bool;
    fn intersection_with(&mut self, other: &Self);
    fn union_with(&mut self, other: Self);
}

pub trait SetElementOps {
    type Element;

    fn add_element(&mut self, e: Self::Element);
    fn remove_element(&mut self, e: &Self::Element);
}

#[derive(PartialEq, Eq, Clone, Debug)]
pub enum PowersetLattice<S: SetAbstractDomainOps> {
    Top,
    Value(S),
    Bottom,
}

impl<S: SetAbstractDomainOps> PowersetLattice<S> {
    pub fn value_from_set(set: S) -> Self {
        Self::Value(set)
    }
}

impl<'a, S: SetAbstractDomainOps + SetElementOps + 'a> PowersetLattice<S>
where
    &'a S: IntoIterator,
{
    pub fn add_element(&mut self, e: S::Element) {
        if let Self::Value(powerset) = self {
            powerset.add_element(e);
        }
    }

    pub fn add_elements<I: IntoIterator<Item = S::Element>>(&mut self, elements: I) {
        if let Self::Value(powerset) = self {
            for e in elements {
                powerset.add_element(e);
            }
        }
    }

    pub fn remove_element(&mut self, e: &S::Element) {
        if let Self::Value(powerset) = self {
            powerset.remove_element(e);
        }
    }

    pub fn remove_elements<'b, I: IntoIterator<Item = &'b S::Element>>(&mut self, elements: I)
    where
        S::Element: 'b,
    {
        if let Self::Value(powerset) = self {
            for e in elements {
                powerset.remove_element(e);
            }
        }
    }

    pub fn elements(&'a self) -> impl Iterator<Item = <&'a S as IntoIterator>::Item> {
        // NOTE: this is a workaround to make an empty iter, since we don't
        // know the actual type of S::ElementIter, we cannot create an empty
        // iter with the same type.
        let res = match self {
            Self::Value(powerset) => Some(powerset.into_iter()),
            _ => None,
        };
        res.into_iter().flatten()
    }

    pub fn set(&self) -> &S {
        match self {
            Self::Value(powerset) => powerset,
            _ => panic!("set called on Top or Bottom value!"),
        }
    }

    pub fn into_set(self) -> S {
        match self {
            Self::Value(powerset) => powerset,
            _ => panic!("into_set called on Top or Bottom value!"),
        }
    }
}

impl<S: SetAbstractDomainOps> AbstractDomain for PowersetLattice<S> {
    fn bottom() -> Self {
        Self::Bottom
    }

    fn top() -> Self {
        Self::Top
    }

    fn is_bottom(&self) -> bool {
        matches!(self, Self::Bottom)
    }

    fn is_top(&self) -> bool {
        matches!(self, Self::Top)
    }

    fn leq(&self, rhs: &Self) -> bool {
        match self {
            Self::Top => rhs.is_top(),
            Self::Value(ref s) => match rhs {
                Self::Top => true,
                Self::Value(ref t) => s.is_subset(t),
                Self::Bottom => false,
            },
            Self::Bottom => true,
        }
    }

    fn join_with(&mut self, rhs: Self) {
        match self {
            Self::Top => {}
            Self::Value(ref mut s) => match rhs {
                Self::Top => *self = rhs,
                Self::Value(t) => {
                    s.union_with(t);
                }
                Self::Bottom => {}
            },
            Self::Bottom => *self = rhs,
        };
    }

    fn meet_with(&mut self, rhs: Self) {
        match self {
            Self::Top => *self = rhs,
            Self::Value(ref mut s) => match rhs {
                Self::Top => {}
                Self::Value(ref t) => {
                    s.intersection_with(t);
                }
                Self::Bottom => *self = rhs,
            },
            Self::Bottom => {}
        };
    }

    fn widen_with(&mut self, _rhs: Self) {
        *self = Self::top();
    }

    fn narrow_with(&mut self, _rhs: Self) {
        *self = Self::bottom();
    }
}

impl<S, A> FromIterator<A> for PowersetLattice<S>
where
    S: SetAbstractDomainOps + FromIterator<A>,
{
    fn from_iter<T: IntoIterator<Item = A>>(iter: T) -> Self {
        Self::value_from_set(S::from_iter(iter))
    }
}

pub type PatriciaTreeSetAbstractDomain<T> = PowersetLattice<PatriciaTreeSet<T>>;
pub type HashSetAbstractDomain<T> = PowersetLattice<HashSet<T>>;

#[cfg(test)]
mod tests {
    use crate::datatype::abstract_domain::AbstractDomain;
    use crate::datatype::HashSetAbstractDomain;
    use crate::datatype::PatriciaTreeSetAbstractDomain;

    macro_rules! test_powerset_impl {
        ($powerset_type:ty) => {
            type IntPowerset = $powerset_type;

            let top = IntPowerset::Top;
            let value1: IntPowerset = vec![1, 2, 3, 4, 5].into_iter().collect();
            let value2: IntPowerset = vec![3, 4, 5, 6, 7].into_iter().collect();

            assert!(top.is_top());
            assert!(!value1.is_top());
            assert!(value1.leq(&IntPowerset::Top));
            assert!(IntPowerset::Bottom.leq(&IntPowerset::Top));

            assert!(!value1.leq(&value2));
            assert!(!value2.leq(&value1));

            let mut elements: Vec<_> = value1.elements().map(|e| e.to_owned()).collect();
            elements.sort();
            assert_eq!(elements, vec![1, 2, 3, 4, 5]);
            let mut elements: Vec<_> = top.elements().map(|e| e.to_owned()).collect();
            elements.sort();
            assert_eq!(elements, vec![]);

            let joined = value1.clone().join(value2.clone());
            let expected_joined: IntPowerset = vec![1, 2, 3, 4, 5, 6, 7].into_iter().collect();

            assert_eq!(joined, expected_joined);

            let met = value1.meet(value2);
            let expected_met: IntPowerset = vec![3, 4, 5].into_iter().collect();
            assert_eq!(met, expected_met);
        };
    }

    #[test]
    fn test_hash_powerset() {
        test_powerset_impl!(HashSetAbstractDomain<i64>);
    }

    #[test]
    fn test_patriciatree_powerset() {
        test_powerset_impl!(PatriciaTreeSetAbstractDomain<i64>);
    }
}
