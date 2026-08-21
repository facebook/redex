/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::hash::Hash;

use sparta::datatype::AbstractDomain;
use sparta::datatype::DisjointUnion;
use sparta::datatype::HashSetAbstractDomain;

#[allow(dead_code)]
#[derive(Clone, DisjointUnion, PartialEq, Eq)]
enum MyUnionedDomain {
    FirstCase(HashSetAbstractDomain<i32>),
    SecondCase(HashSetAbstractDomain<i64>),
}

#[test]
fn test_basic_union_top_bottom_leq() {
    assert!(MyUnionedDomain::top().is_top());
    assert!(MyUnionedDomain::SecondCase(AbstractDomain::top()).is_top());
    assert!(MyUnionedDomain::SecondCase(AbstractDomain::bottom()).is_bottom());

    let top: MyUnionedDomain = AbstractDomain::top();
    let bot: MyUnionedDomain = AbstractDomain::bottom();

    assert!(bot.leq(&top));

    let hsdom1: HashSetAbstractDomain<_> = [1, 2].into_iter().collect();
    let hsdom2: HashSetAbstractDomain<_> = [1, 2, 3].into_iter().collect();

    assert!(hsdom1.leq(&hsdom2));

    assert!(MyUnionedDomain::FirstCase(hsdom1).leq(&MyUnionedDomain::FirstCase(hsdom2)));
}

#[test]
fn test_diff_arms_no_leq() {
    let hsdom1: HashSetAbstractDomain<i32> = [1, 2].into_iter().collect();
    let hsdom2: HashSetAbstractDomain<i64> = [1, 2, 3].into_iter().collect();

    // watch out! Two different arms should not leq one another.
    let mudom1 = MyUnionedDomain::FirstCase(hsdom1);
    let mudom2 = MyUnionedDomain::SecondCase(hsdom2);

    assert!(!mudom1.leq(&mudom2));
    assert!(!mudom2.leq(&mudom1));
}

#[test]
fn test_join_same_arm() {
    let hsdom1: HashSetAbstractDomain<_> = [1, 2].into_iter().collect();
    let hsdom2: HashSetAbstractDomain<_> = [2, 3].into_iter().collect();

    let joined = hsdom1.clone().join(hsdom2.clone());
    let expected_joined: HashSetAbstractDomain<_> = [1, 2, 3].into_iter().collect();

    assert_eq!(joined, expected_joined);

    let mudom1 = MyUnionedDomain::FirstCase(hsdom1);
    let mudom2 = MyUnionedDomain::FirstCase(hsdom2);

    let joined_mudom = mudom1.join(mudom2);
    let inner_dom = match joined_mudom {
        MyUnionedDomain::FirstCase(inner) => inner,
        _ => panic!("Unexpected case"),
    };

    assert_eq!(inner_dom, expected_joined);
}

#[test]
fn test_meet_same_arm() {
    let hsdom1: HashSetAbstractDomain<_> = [1, 2].into_iter().collect();
    let hsdom2: HashSetAbstractDomain<_> = [2, 3].into_iter().collect();

    let met = hsdom1.clone().meet(hsdom2.clone());
    let expected_met: HashSetAbstractDomain<_> = [2].into_iter().collect();

    assert_eq!(met, expected_met);

    let mudom1 = MyUnionedDomain::FirstCase(hsdom1);
    let mudom2 = MyUnionedDomain::FirstCase(hsdom2);

    let met_mudom = mudom1.meet(mudom2);
    let inner_dom = match met_mudom {
        MyUnionedDomain::FirstCase(inner) => inner,
        _ => panic!("Unexpected case"),
    };

    assert_eq!(inner_dom, expected_met);
}

#[test]
fn test_join_diff_arm() {
    let hsdom1: HashSetAbstractDomain<_> = [1, 2].into_iter().collect();
    let hsdom2: HashSetAbstractDomain<_> = [2, 3].into_iter().collect();

    let mudom1 = MyUnionedDomain::FirstCase(hsdom1);
    let mudom2 = MyUnionedDomain::SecondCase(hsdom2);

    let joined_mudom = mudom1.join(mudom2);

    assert!(joined_mudom.is_top());
}

#[test]
fn test_meet_diff_arm() {
    let hsdom1: HashSetAbstractDomain<_> = [1, 2].into_iter().collect();
    let hsdom2: HashSetAbstractDomain<_> = [2, 3].into_iter().collect();

    let mudom1 = MyUnionedDomain::FirstCase(hsdom1);
    let mudom2 = MyUnionedDomain::SecondCase(hsdom2);

    let met_mudom = mudom1.meet(mudom2);

    assert!(met_mudom.is_bottom());
}

fn first_case_inner(dom: MyUnionedDomain) -> HashSetAbstractDomain<i32> {
    match dom {
        MyUnionedDomain::FirstCase(inner) => inner,
        _ => panic!("Expected FirstCase"),
    }
}

#[test]
fn test_join_with_bottom_is_identity() {
    let value_inner: HashSetAbstractDomain<_> = [1, 2].into_iter().collect();
    let value = MyUnionedDomain::FirstCase(value_inner.clone());

    // Bottom is the identity element for join, so joining with it in either
    // order yields the original value rather than top.
    let bot: MyUnionedDomain = AbstractDomain::bottom();
    assert_eq!(
        first_case_inner(value.clone().join(bot.clone())),
        value_inner
    );
    assert_eq!(first_case_inner(bot.join(value.clone())), value_inner);

    // The bottom of a different arm is still bottom and must behave the same.
    let other_arm_bottom = MyUnionedDomain::SecondCase(AbstractDomain::bottom());
    assert!(other_arm_bottom.is_bottom());
    assert_eq!(
        first_case_inner(other_arm_bottom.join(value.clone())),
        value_inner
    );
}

#[test]
fn test_meet_with_top_is_identity() {
    let value_inner: HashSetAbstractDomain<_> = [1, 2].into_iter().collect();
    let value = MyUnionedDomain::FirstCase(value_inner.clone());

    // Top is the identity element for meet, so meeting with it in either order
    // yields the original value rather than bottom.
    let top: MyUnionedDomain = AbstractDomain::top();
    assert_eq!(
        first_case_inner(value.clone().meet(top.clone())),
        value_inner
    );
    assert_eq!(first_case_inner(top.meet(value.clone())), value_inner);

    // The top of a different arm is still top and must behave the same.
    let other_arm_top = MyUnionedDomain::SecondCase(AbstractDomain::top());
    assert!(other_arm_top.is_top());
    assert_eq!(
        first_case_inner(other_arm_top.meet(value.clone())),
        value_inner
    );
}

#[allow(dead_code)]
#[derive(Clone, DisjointUnion, PartialEq, Eq)]
enum TestGenericsDeriveTypechecks<S, T>
where
    S: Eq + Hash + Clone,
    T: Eq + Hash + Clone,
{
    FirstCase(HashSetAbstractDomain<S>),
    SecondCase(HashSetAbstractDomain<T>),
}

#[allow(dead_code)]
#[derive(Clone, DisjointUnion, PartialEq, Eq)]
enum TestGenericsDeriveForWholeDomainTypechecks<S: AbstractDomain, T>
where
    T: AbstractDomain,
{
    FirstCase(S),
    SecondCase(T),
}
