/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//! A patricia tree is keyed by whatever converts into a `BitVec`, so a key
//! type declared outside this crate needs to name that type to convert into
//! it. These tests are written the way a user of the library has to write one,
//! and fail to compile if `BitVec` stops being reachable from outside.

use sparta::datatype::AbstractDomain;
use sparta::datatype::AbstractEnvironment;
use sparta::datatype::BitVec;
use sparta::datatype::HashSetAbstractDomain;
use sparta::datatype::PatriciaTreeMap;
use sparta::datatype::PatriciaTreeMapAbstractEnvironment;
use sparta::datatype::PatriciaTreeSet;

/// The shape a key usually has outside this crate: a newtype over a number,
/// so that one arena's identifiers cannot be mistaken for another's.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
struct Register(u32);

const REGISTER_BITS: usize = u32::BITS as usize;

impl From<Register> for BitVec {
    fn from(register: Register) -> BitVec {
        register.0.into()
    }
}

impl From<&BitVec> for Register {
    fn from(bits: &BitVec) -> Register {
        Register(bits.into())
    }
}

type Domain = HashSetAbstractDomain<String>;

fn build_domain<const N: usize>(arr: [&str; N]) -> Domain {
    arr.iter().map(|str| str.to_string()).collect()
}

#[test]
fn test_foreign_key_round_trips_through_a_bitvec() {
    for value in [0, 1, 2, 7, 4096, u32::MAX] {
        let register = Register(value);
        let bits: BitVec = register.into();
        assert_eq!(bits.len(), REGISTER_BITS);
        assert_eq!(Register::from(&bits), register);
    }
}

#[test]
fn test_map_keyed_on_a_foreign_type() {
    let mut map: PatriciaTreeMap<Register, u64> = PatriciaTreeMap::new();
    map.upsert(Register(3), 30);
    map.upsert(Register(1), 10);
    map.upsert(Register(u32::MAX), 40);

    assert_eq!(map.get(Register(1)), Some(&10));
    assert_eq!(map.get(Register(2)), None);
    assert!(map.contains_key(Register(u32::MAX)));

    // Reading the keys back is the direction that needs `From<&BitVec>`.
    let mut read_back: Vec<(Register, u64)> = map.iter().map(|(k, v)| (k, *v)).collect();
    read_back.sort_by_key(|(register, _)| register.0);
    assert_eq!(
        read_back,
        vec![
            (Register(1), 10),
            (Register(3), 30),
            (Register(u32::MAX), 40)
        ],
    );
}

#[test]
fn test_set_keyed_on_a_foreign_type() {
    let set: PatriciaTreeSet<Register> = [Register(1), Register(9), Register(1)]
        .into_iter()
        .collect();

    assert_eq!(set.len(), 2);
    assert!(set.contains(Register(9)));
    assert!(!set.contains(Register(2)));

    let mut read_back: Vec<u32> = set.iter().map(|register: Register| register.0).collect();
    read_back.sort();
    assert_eq!(read_back, vec![1, 9]);
}

#[test]
fn test_environment_keyed_on_a_foreign_type() {
    type Environment = PatriciaTreeMapAbstractEnvironment<Register, Domain>;

    let mut left = Environment::top();
    left.set(Register(1), build_domain(["a", "b"]));
    left.set(Register(2), build_domain(["c"]));

    let mut right = Environment::top();
    right.set(Register(1), build_domain(["b", "d"]));
    right.set(Register(3), build_domain(["e"]));

    let joined = left.clone().join(right);
    // A variable only one side binds is unconstrained on the other.
    assert_eq!(
        joined.get(&Register(1)).into_owned(),
        build_domain(["a", "b", "d"])
    );
    assert!(joined.get(&Register(2)).is_top());
    assert_eq!(joined.len(), 1);

    assert!(left.leq(&Environment::top()));
    assert!(Environment::bottom().leq(&left));
}
