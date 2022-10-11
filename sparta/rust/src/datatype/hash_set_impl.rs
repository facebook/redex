/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::hash::Hash;

use im::HashSet;

use super::powerset::SetAbstractDomainOps;
use super::powerset::SetElementOps;

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

    fn add_element(&mut self, e: T) {
        self.insert(e);
    }

    fn remove_element(&mut self, e: &T) {
        self.remove(e);
    }
}
