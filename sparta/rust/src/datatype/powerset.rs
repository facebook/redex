/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::iter::FromIterator;

use super::abstract_domain::AbstractDomain;

pub trait SetAbstractDomainOps: Clone {
    fn is_subset(&self, other: &Self) -> bool;
    fn intersection_with(&mut self, other: &Self);
    fn union_with(&mut self, other: Self);
}

pub trait SetElementOps {
    type Element;
    type ElementIter<'a>: Iterator<Item = &'a Self::Element>
    where
        Self: 'a;

    fn add_element(&mut self, e: Self::Element);
    fn remove_element(&mut self, e: &Self::Element);
    fn elements(&self) -> Self::ElementIter<'_>;
}

#[derive(PartialEq, Eq, Clone, Debug)]
pub enum PowersetLattice<S: SetAbstractDomainOps> {
    Top,
    Value(S),
    Bottom,
}

impl<S: SetAbstractDomainOps> PowersetLattice<S> {
    pub fn value_from_set(set: S) -> Self {
        PowersetLattice::<S>::Value(set)
    }
}

impl<S: SetAbstractDomainOps + SetElementOps> PowersetLattice<S> {
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

impl<S: SetAbstractDomainOps> AbstractDomain for PowersetLattice<S> {
    fn bottom() -> Self {
        PowersetLattice::Bottom
    }

    fn top() -> Self {
        PowersetLattice::Top
    }

    fn is_bottom(&self) -> bool {
        matches!(self, PowersetLattice::Bottom)
    }

    fn is_top(&self) -> bool {
        matches!(self, PowersetLattice::Top)
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

impl<S, A> FromIterator<A> for PowersetLattice<S>
where
    S: SetAbstractDomainOps + FromIterator<A>,
{
    fn from_iter<T: IntoIterator<Item = A>>(iter: T) -> Self {
        Self::value_from_set(S::from_iter(iter))
    }
}
