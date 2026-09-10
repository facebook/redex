/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//! Patricia trees are reference counted with `triomphe::Arc`, so they can be
//! shared across threads. These tests fail to compile if that regresses.

use sparta::datatype::PatriciaTreeMap;
use sparta::datatype::PatriciaTreeSet;

fn assert_send_sync<T: Send + Sync>() {}

#[test]
fn test_patricia_trees_are_send_and_sync() {
    assert_send_sync::<PatriciaTreeSet<u32>>();
    assert_send_sync::<PatriciaTreeMap<u32, u64>>();
}

#[test]
fn test_tree_can_be_shared_across_threads() {
    let n: u32 = 10_000;
    let set: PatriciaTreeSet<u32> = (0..n).collect();

    let found: usize = std::thread::scope(|scope| {
        let handles: Vec<_> = (0..4)
            .map(|t| {
                // Every clone here bumps the same atomic refcounts that used to
                // be non-atomic `Rc` counts.
                let set = set.clone();
                scope.spawn(move || (0..n).filter(|i| i % 4 == t && set.contains(*i)).count())
            })
            .collect();
        handles.into_iter().map(|h| h.join().unwrap()).sum()
    });

    assert_eq!(found, n as usize);
    assert_eq!(set.len(), n as usize);
}
