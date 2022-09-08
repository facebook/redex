/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

pub trait AbstractDomain: Clone + Eq {
    fn bottom() -> Self;
    fn top() -> Self;
    fn is_bottom(&self) -> bool;
    fn is_top(&self) -> bool;
    fn leq(&self, rhs: &Self) -> bool;

    fn join(mut self, rhs: Self) -> Self {
        self.join_with(rhs);
        self
    }

    fn meet(mut self, rhs: Self) -> Self {
        self.meet_with(rhs);
        self
    }

    fn widen(mut self, rhs: Self) -> Self {
        self.widen_with(rhs);
        self
    }

    fn narrow(mut self, rhs: Self) -> Self {
        self.narrow_with(rhs);
        self
    }

    fn join_with(&mut self, rhs: Self);
    fn meet_with(&mut self, rhs: Self);
    fn widen_with(&mut self, rhs: Self);
    fn narrow_with(&mut self, rhs: Self);
}
