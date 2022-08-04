/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use super::abstract_domain::AbstractDomain;
use std::collections::HashSet;
use std::hash::Hash;

pub trait SetOps {
    fn is_subset(&self, other: &Self) -> bool;
    fn intersection_with(&mut self, other: &Self);
    fn union_with(&mut self, other: Self);
}

impl<T: Eq + Hash + Clone> SetOps for HashSet<T> {
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
}

#[derive(PartialEq, Eq, Clone, Debug)]
pub enum PowersetLattice<S: SetOps> {
    Top,
    Value(S),
    Bottom,
}

impl<S: SetOps> PowersetLattice<S> {
    fn value_from_set(set: S) -> Self {
        PowersetLattice::<S>::Value(set)
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

#[cfg(test)]
mod tests {
    use crate::datatype::AbstractDomain;
    use crate::datatype::PowersetLattice;
    use std::collections::HashSet;
    #[test]
    fn test_powerset() {
        type IntPowerset = PowersetLattice<HashSet<i64>>;
        let top = IntPowerset::Top;
        let value1 = IntPowerset::value_from_set(HashSet::from([1, 2, 3, 4, 5]));
        let value2 = IntPowerset::value_from_set(HashSet::from([3, 4, 5, 6, 7]));

        assert!(top.is_top());
        assert!(!value1.is_top());
        assert!(value1.leq(&IntPowerset::Top));
        assert!(IntPowerset::Bottom.leq(&IntPowerset::Top));

        assert!(!value1.leq(&value2));
        assert!(!value2.leq(&value1));

        let joined = value1.clone().join(value2.clone());
        let expected_joined = IntPowerset::value_from_set(HashSet::from([1, 2, 3, 4, 5, 6, 7]));

        assert_eq!(joined, expected_joined);

        let met = value1.meet(value2);
        let expected_met = IntPowerset::value_from_set(HashSet::from([3, 4, 5]));
        assert_eq!(met, expected_met);
    }
}
