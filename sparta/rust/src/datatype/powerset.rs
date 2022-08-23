/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::collections::hash_set::Iter;
use std::collections::HashSet;
use std::hash::Hash;

use super::abstract_domain::AbstractDomain;

pub trait SetOps: Clone {
    type Element;
    type ElementIter<'a>: Iterator<Item = &'a Self::Element>
    where
        Self: 'a;

    fn is_subset(&self, other: &Self) -> bool;

    fn intersection_with(&mut self, other: &Self);

    fn union_with(&mut self, other: Self);

    fn add_element(&mut self, e: Self::Element);

    fn remove_element(&mut self, e: &Self::Element);

    fn elements(&self) -> Self::ElementIter<'_>;
}

impl<T: Eq + Hash + Clone> SetOps for HashSet<T> {
    type Element = T;
    type ElementIter<'a> = Iter<'a, T> where Self: 'a;

    fn is_subset(&self, other: &Self) -> bool {
        self.is_subset(other)
    }

    fn intersection_with(&mut self, other: &Self) {
        self.retain(|elem| other.contains(elem));
    }

    fn union_with(&mut self, other: Self) {
        other.into_iter().for_each(|elem| {
            self.insert(elem);
        })
    }

    fn add_element(&mut self, e: T) {
        self.insert(e);
    }

    fn remove_element(&mut self, e: &T) {
        self.remove(e);
    }

    fn elements(&self) -> Self::ElementIter<'_> {
        self.iter()
    }
}

#[derive(PartialEq, Eq, Clone, Debug)]
pub enum PowersetLattice<S: SetOps> {
    Top,
    Value(S),
    Bottom,
}

impl<S: SetOps> PowersetLattice<S> {
    pub fn value_from_set(set: S) -> Self {
        PowersetLattice::<S>::Value(set)
    }

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

    pub fn remove_elements<'a, I: IntoIterator<Item = &'a S::Element>>(&mut self, elements: I)
    where
        S::Element: 'a,
    {
        if let Self::Value(powerset) = self {
            for e in elements {
                powerset.remove_element(e);
            }
        }
    }

    pub fn elements(&self) -> impl Iterator<Item = &'_ S::Element> {
        // NOTE: this is a workaround to make an empty iter, since we don't
        // know the actual type of S::ElementIter, we cannot create an empty
        // iter with the same type.
        let res = match self {
            Self::Value(powerset) => Some(powerset.elements()),
            _ => None,
        };
        res.into_iter().flatten()
    }
}

impl<S: SetOps> AbstractDomain for PowersetLattice<S> {
    fn bottom() -> PowersetLattice<S> {
        PowersetLattice::<S>::Bottom
    }

    fn top() -> PowersetLattice<S> {
        PowersetLattice::<S>::Top
    }

    fn is_bottom(&self) -> bool {
        match self {
            PowersetLattice::Bottom => true,
            _ => false,
        }
    }

    fn is_top(&self) -> bool {
        match self {
            PowersetLattice::Top => true,
            _ => false,
        }
    }

    fn leq(&self, rhs: &Self) -> bool {
        use PowersetLattice::*;
        match self {
            Top => rhs.is_top(),
            Value(ref s) => match rhs {
                Top => true,
                Value(ref t) => s.is_subset(t),
                Bottom => false,
            },
            Bottom => true,
        }
    }

    fn join_with(&mut self, rhs: Self) {
        use PowersetLattice::*;
        match self {
            Top => {}
            Value(ref mut s) => match rhs {
                Top => *self = rhs,
                // Is this ineffecient? Can we just ref mut s itself?
                Value(t) => {
                    s.union_with(t);
                }
                Bottom => {}
            },
            Bottom => *self = rhs,
        };
    }

    fn meet_with(&mut self, rhs: Self) {
        use PowersetLattice::*;
        match self {
            Top => *self = rhs,
            Value(ref mut s) => match rhs {
                Top => {}
                Value(ref t) => {
                    s.intersection_with(t);
                }
                Bottom => *self = rhs,
            },
            Bottom => {}
        };
    }

    fn widen_with(&mut self, _rhs: Self) {
        *self = Self::top();
    }

    fn narrow_with(&mut self, _rhs: Self) {
        *self = Self::bottom();
    }
}

pub type HashSetAbstractDomain<T> = PowersetLattice<HashSet<T>>;

#[cfg(test)]
mod tests {
    use crate::datatype::AbstractDomain;
    use crate::datatype::HashSetAbstractDomain;

    #[test]
    fn test_powerset() {
        type IntPowerset = HashSetAbstractDomain<i64>;
        let top = IntPowerset::Top;
        let value1 = IntPowerset::value_from_set(vec![1, 2, 3, 4, 5].into_iter().collect());
        let value2 = IntPowerset::value_from_set(vec![3, 4, 5, 6, 7].into_iter().collect());

        assert!(top.is_top());
        assert!(!value1.is_top());
        assert!(value1.leq(&IntPowerset::Top));
        assert!(IntPowerset::Bottom.leq(&IntPowerset::Top));

        assert!(!value1.leq(&value2));
        assert!(!value2.leq(&value1));

        let mut elements: Vec<i64> = value1.elements().copied().collect();
        elements.sort();
        assert_eq!(elements, vec![1, 2, 3, 4, 5]);
        let mut elements: Vec<i64> = top.elements().copied().collect();
        elements.sort();
        assert_eq!(elements, vec![]);

        let joined = value1.clone().join(value2.clone());
        let expected_joined =
            IntPowerset::value_from_set(vec![1, 2, 3, 4, 5, 6, 7].into_iter().collect());

        assert_eq!(joined, expected_joined);

        let met = value1.meet(value2);
        let expected_met = IntPowerset::value_from_set(vec![3, 4, 5].into_iter().collect());
        assert_eq!(met, expected_met);
    }
}
