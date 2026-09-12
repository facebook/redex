/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//! Patricia trees update nodes in place when nothing else points at them. These
//! tests take snapshots part way through a long run of mutations and check that
//! the snapshots never observe a later edit, which is what an in-place write
//! below a shared node would look like.

use std::collections::HashMap;
use std::collections::HashSet;

use rand::RngExt as _;
use sparta::datatype::PatriciaTreeMap;
use sparta::datatype::PatriciaTreeSet;

#[test]
fn test_set_snapshots_are_unaffected_by_later_mutation() {
    let mut rng = rand::rng();

    let mut live: PatriciaTreeSet<u32> = PatriciaTreeSet::new();
    let mut expected: HashSet<u32> = HashSet::new();
    let mut snapshots: Vec<(PatriciaTreeSet<u32>, HashSet<u32>)> = Vec::new();

    for step in 0..20_000 {
        let key: u32 = rng.random_range(0..2_000);
        if rng.random_bool(0.65) {
            live.insert(key);
            expected.insert(key);
        } else {
            live.remove(key);
            expected.remove(&key);
        }

        // From here on `live` shares nodes with the snapshot.
        if step % 500 == 0 {
            snapshots.push((live.clone(), expected.clone()));
        }
    }

    for (index, (snapshot, expected)) in snapshots.iter().enumerate() {
        let actual: HashSet<u32> = snapshot.iter().collect();
        assert_eq!(&actual, expected, "snapshot {} was modified", index);
    }

    let actual: HashSet<u32> = live.iter().collect();
    assert_eq!(&actual, &expected);
}

#[test]
fn test_map_snapshots_keep_their_values() {
    let mut rng = rand::rng();

    let mut live: PatriciaTreeMap<u32, u32> = PatriciaTreeMap::new();
    let mut expected: HashMap<u32, u32> = HashMap::new();
    let mut snapshots: Vec<(PatriciaTreeMap<u32, u32>, HashMap<u32, u32>)> = Vec::new();

    for step in 0..20_000 {
        let key: u32 = rng.random_range(0..2_000);
        if rng.random_bool(0.65) {
            // Rebinding an existing key to a new value is the case that has to
            // copy rather than overwrite when the leaf is shared.
            let value: u32 = rng.random_range(0..10);
            live.upsert(key, value);
            expected.insert(key, value);
        } else {
            live.remove(key);
            expected.remove(&key);
        }

        if step % 500 == 0 {
            snapshots.push((live.clone(), expected.clone()));
        }
    }

    for (index, (snapshot, expected)) in snapshots.iter().enumerate() {
        let actual: HashMap<u32, u32> = snapshot.iter().map(|(k, v)| (k, *v)).collect();
        assert_eq!(&actual, expected, "snapshot {} was modified", index);
    }

    let actual: HashMap<u32, u32> = live.iter().map(|(k, v)| (k, *v)).collect();
    assert_eq!(&actual, &expected);
}
