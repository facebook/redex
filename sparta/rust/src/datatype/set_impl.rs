/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::collections::hash_set::Iter;
use std::collections::HashSet;
use std::hash::Hash;

use super::bitvec::BitVec;
use super::powerset::PowersetLattice;
use super::powerset::SetAbstractDomainOps;
use super::powerset::SetElementOps;
use crate::datatype::PatriciaTreeSet;

impl<T: Eq + Hash + Clone> SetAbstractDomainOps for HashSet<T> {
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

impl<T: Eq + Hash + Clone> SetElementOps for HashSet<T> {
    type Element = T;
    type ElementIter<'a> = Iter<'a, T> where Self: 'a;

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

pub type HashSetAbstractDomain<T> = PowersetLattice<HashSet<T>>;

impl<T> SetAbstractDomainOps for PatriciaTreeSet<T>
where
    T: Clone,
    BitVec: From<T>,
{
    fn is_subset(&self, _other: &Self) -> bool {
        todo!()
    }

    fn intersection_with(&mut self, _other: &Self) {
        todo!();
    }

    fn union_with(&mut self, _other: Self) {
        todo!();
    }
}

pub type PatriciaTreeSetAbstractDomain<T> = PowersetLattice<PatriciaTreeSet<T>>;

#[cfg(test)]
mod tests {
    use crate::datatype::AbstractDomain;
    use crate::datatype::HashSetAbstractDomain;

    #[test]
    fn test_powerset() {
        type IntPowerset = HashSetAbstractDomain<i64>;
        let top = IntPowerset::Top;
        let value1: IntPowerset = vec![1, 2, 3, 4, 5].into_iter().collect();
        let value2: IntPowerset = vec![3, 4, 5, 6, 7].into_iter().collect();

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
        let expected_joined: IntPowerset = vec![1, 2, 3, 4, 5, 6, 7].into_iter().collect();

        assert_eq!(joined, expected_joined);

        let met = value1.meet(value2);
        let expected_met: IntPowerset = vec![3, 4, 5].into_iter().collect();
        assert_eq!(met, expected_met);
    }
}
