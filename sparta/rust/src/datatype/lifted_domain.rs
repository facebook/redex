/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use super::abstract_domain::AbstractDomain;

#[derive(PartialEq, Eq, Clone, Debug)]
pub enum LiftedDomain<D: AbstractDomain> {
    Lifted(D),
    Bottom,
}

macro_rules! get_lowered {
    ( $self: ident ) => {
        match $self {
            Self::Lifted(underlying) => underlying,
            Self::Bottom => panic!("It's bottom and cannot be lowered!"),
        }
    };
}

impl<D: AbstractDomain> LiftedDomain<D> {
    fn lifted(underlying: D) -> Self {
        Self::Lifted(underlying)
    }

    fn is_lifted(&self) -> bool {
        matches!(self, Self::Lifted(_))
    }

    fn lowered(&self) -> &D {
        get_lowered!(self)
    }

    fn lowered_mut(&mut self) -> &mut D {
        get_lowered!(self)
    }

    fn into_lowered(self) -> D {
        get_lowered!(self)
    }
}

impl<D: AbstractDomain> AbstractDomain for LiftedDomain<D> {
    fn bottom() -> Self {
        Self::Bottom
    }

    fn top() -> Self {
        Self::Lifted(D::top())
    }

    fn is_bottom(&self) -> bool {
        matches!(self, Self::Bottom)
    }

    fn is_top(&self) -> bool {
        matches!(self, Self::Lifted(s) if s.is_top())
    }

    fn leq(&self, rhs: &Self) -> bool {
        match (self, rhs) {
            (Self::Bottom, _) => true,
            (_, Self::Bottom) => false,
            (Self::Lifted(s), Self::Lifted(t)) => s.leq(t),
        }
    }

    fn join_with(&mut self, rhs: Self) {
        match (self, rhs) {
            (x @ Self::Bottom, y) => *x = y,
            (_, Self::Bottom) => {}
            (Self::Lifted(s), Self::Lifted(t)) => s.join_with(t),
        }
    }

    fn meet_with(&mut self, rhs: Self) {
        match (self, rhs) {
            (Self::Bottom, _) => {}
            (x, y @ Self::Bottom) => *x = y,
            (Self::Lifted(s), Self::Lifted(t)) => s.meet_with(t),
        }
    }

    fn widen_with(&mut self, rhs: Self) {
        match (self, rhs) {
            (x @ Self::Bottom, y) => *x = y,
            (_, Self::Bottom) => {}
            (Self::Lifted(s), Self::Lifted(t)) => s.widen_with(t),
        }
    }

    fn narrow_with(&mut self, rhs: Self) {
        match (self, rhs) {
            (Self::Bottom, _) => {}
            (x, y @ Self::Bottom) => *x = y,
            (Self::Lifted(s), Self::Lifted(t)) => s.narrow_with(t),
        }
    }
}

#[cfg(test)]
mod tests {
    use crate::datatype::lifted_domain::*;
    use crate::datatype::powerset::*;

    type UnderlyingDomain = HashSetAbstractDomain<i64>;
    type Domain = LiftedDomain<UnderlyingDomain>;

    #[test]
    fn test_basic_lift_ops() {
        let underlying_bottom = UnderlyingDomain::bottom();
        let underlying_top = UnderlyingDomain::top();
        let underlying_value1: UnderlyingDomain = vec![1, 2].into_iter().collect();
        let underlying_value2: UnderlyingDomain = vec![1, 2, 3].into_iter().collect();
        let underlying_value3: UnderlyingDomain = vec![1, 2, 4].into_iter().collect();
        let underlying_value4: UnderlyingDomain = vec![3, 4, 5].into_iter().collect();

        let bottom = Domain::bottom();
        let top = Domain::top();
        let mut lifted_bottom = Domain::lifted(underlying_bottom);
        let mut lifted_top = Domain::lifted(underlying_top);
        let mut lifted_value1 = Domain::lifted(underlying_value1);
        let mut lifted_value2 = Domain::lifted(underlying_value2);
        let lifted_value3 = Domain::lifted(underlying_value3);
        let lifted_value4 = Domain::lifted(underlying_value4);

        // Domain::Bottom is equal to Domain::bottom()
        assert!(bottom.leq(&Domain::Bottom));
        assert!(Domain::Bottom.leq(&bottom));
        assert_eq!(Domain::Bottom, bottom);

        // The lifted bottom is strictly greater than the bottom
        assert!(bottom.leq(&lifted_bottom));
        assert!(!lifted_bottom.leq(&bottom));
        assert_ne!(bottom, lifted_bottom);

        // The lifted top is equal to Domain::top()
        assert!(top.leq(&lifted_top));
        assert!(lifted_top.leq(&top));
        assert_eq!(top, lifted_top);

        // Check the ordering among the lifted values
        assert!(lifted_bottom.leq(&lifted_value1));
        assert!(lifted_value1.leq(&lifted_value2));
        assert!(lifted_value1.leq(&lifted_value3));

        assert!(!lifted_value2.leq(&lifted_value3));
        assert!(!lifted_value3.leq(&lifted_value2));

        assert!(lifted_value2.leq(&lifted_top));
        assert!(lifted_value3.leq(&lifted_top));

        // Test is_lifted
        assert!(lifted_bottom.is_lifted());
        assert!(lifted_top.is_lifted());
        assert!(lifted_value1.is_lifted());
        assert!(!bottom.is_lifted());

        // Test lowered
        assert_eq!(*lifted_bottom.lowered(), UnderlyingDomain::bottom());
        assert_eq!(*lifted_top.lowered(), UnderlyingDomain::top());
        assert_eq!(*lifted_value1.lowered(), vec![1, 2].into_iter().collect());

        // Test lowered_mut
        let underlying_val1_mut: &mut UnderlyingDomain = lifted_value1.lowered_mut();
        underlying_val1_mut.add_element(5);
        assert_eq!(*underlying_val1_mut, vec![1, 2, 5].into_iter().collect());

        // Test into_lowered
        let mut underlying_val1_into: UnderlyingDomain = lifted_value1.into_lowered();
        underlying_val1_into.add_element(6);
        assert_eq!(underlying_val1_into, vec![1, 2, 5, 6].into_iter().collect());
        // lifted_value1 cannot be used anymore after into_lowered

        // Test join_with
        lifted_bottom.join_with(Domain::top());
        assert_eq!(lifted_bottom, Domain::top());

        lifted_value2.join_with(lifted_value3);
        assert_eq!(
            *lifted_value2.lowered(),
            vec![1, 2, 3, 4].into_iter().collect()
        );

        // Test meet_with
        lifted_top.meet_with(Domain::bottom());
        assert_eq!(lifted_top, Domain::bottom());

        lifted_value2.meet_with(lifted_value4);
        assert_eq!(*lifted_value2.lowered(), vec![3, 4].into_iter().collect());
    }

    #[test]
    #[should_panic]
    fn test_bottom_lowered() {
        Domain::Bottom.lowered();
    }

    #[test]
    #[should_panic]
    fn test_bottom_lowered_mut() {
        Domain::Bottom.lowered_mut();
    }

    #[test]
    #[should_panic]
    fn test_bottom_into_lowered() {
        Domain::Bottom.into_lowered();
    }
}
