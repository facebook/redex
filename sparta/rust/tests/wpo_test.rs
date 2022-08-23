/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

mod common;

use std::collections::HashMap;

use common::graph::SimpleGraph;
use sparta::graph::Graph;
use sparta::wpo::WeakPartialOrdering;
use sparta::wpo::WpoIdx;

/*
 * This graph and the corresponding weak partial ordering are described
 * on page 4 of Bourdoncle's paper:
 *   F. Bourdoncle. Efficient chaotic iteration strategies with widenings.
 *   In Formal Methods in Programming and Their Applications, pp 128-141.
 * The graph is given as follows:
 *
 *                 +-----------------------+
 *                 |           +-----+     |
 *                 |           |     |     |
 *                 V           V     |     |
 *     1 --> 2 --> 3 --> 4 --> 5 --> 6 --> 7 --> 8
 *           |           |                 ^     ^
 *           |           |                 |     |
 *           |           +-----------------+     |
 *           +-----------------------------------+
 *
 * Bourdoncle's algorithm computes the following weak partial ordering:
 *
 *     1 2 (3 4 (5 6) 7) 8
 */
#[test]
fn test_wpo_example_from_wto_paper() {
    let mut g = SimpleGraph::new(1, 8);
    g.add_edge(1, 2);
    g.add_edge(2, 3);
    g.add_edge(3, 4);
    g.add_edge(4, 5);
    g.add_edge(5, 6);
    g.add_edge(6, 7);
    g.add_edge(7, 8);
    g.add_edge(2, 8);
    g.add_edge(4, 7);
    g.add_edge(6, 5);
    g.add_edge(7, 3);
    // "1 2 (3 4 (5 6) 7) 8"

    println!("Size: {}", g.size());
    let wpo = WeakPartialOrdering::new(1, g.size(), &g);

    assert!(!wpo.is_from_outside(5, 6));
    assert!(!wpo.is_from_outside(3, 7));
    assert!(wpo.is_from_outside(3, 2));
    assert!(!wpo.is_from_outside(3, 4));

    assert_eq!(wpo.size(), 10);

    let expected_results = [
        (1, true, false, false, 1, 0, 0),
        (2, true, false, false, 1, 1, 0),
        (3, false, true, false, 1, 1, 0),
        (4, true, false, false, 1, 1, 0),
        (5, false, true, false, 1, 1, 0),
        (6, true, false, false, 1, 1, 0),
        (5, false, false, true, 1, 1, 1),
        (7, true, false, false, 1, 1, 0),
        (3, false, false, true, 1, 1, 1),
        (8, true, false, false, 0, 1, 0),
    ];
    let mut count = HashMap::<WpoIdx, u32>::new();
    let mut worklist = Vec::new();
    worklist.push(wpo.get_entry());
    let mut output = String::new();
    let mut first = true;
    let mut i = 0;
    while let Some(v) = worklist.pop() {
        for &w in wpo.get_successors(v).iter() {
            let c = count.entry(w).or_default();
            *c += 1;
            if *c as usize == wpo.get_num_preds(w) {
                worklist.push(w);
            }
        }

        let expected = expected_results[i];
        i += 1;
        assert_eq!(expected.0, wpo.get_node(v));
        assert_eq!(expected.1, wpo.is_plain(v));
        assert_eq!(expected.2, wpo.is_head(v));
        assert_eq!(expected.3, wpo.is_exit(v));
        assert_eq!(expected.4, wpo.get_successors(v).len());
        assert_eq!(expected.5, wpo.get_num_preds(v));
        if wpo.is_head(v) {
            assert_eq!(wpo.get_node(wpo.get_exit_of_head(v)), wpo.get_node(v));
            if !first {
                output.push_str(" ");
            }
            output.push_str("(");
            output.push_str(&wpo.get_node(v).to_string());
        } else if wpo.is_exit(v) {
            assert_eq!(expected.6, wpo.get_num_outer_preds(v).len());
            assert_eq!(wpo.get_node(wpo.get_head_of_exit(v)), wpo.get_node(v));
            output.push_str(")");
        } else {
            if !first {
                output.push_str(" ");
            }
            output.push_str(&wpo.get_node(v).to_string());
        }

        first = false;
    }
    assert_eq!(output, "1 2 (3 4 (5 6) 7) 8");
}

#[test]
fn test_wpo_example_from_wpo_paper() {
    let mut g = SimpleGraph::new(1, 10);
    g.add_edge(1, 2);
    g.add_edge(2, 3);
    g.add_edge(3, 4);
    g.add_edge(4, 3);
    g.add_edge(3, 5);
    g.add_edge(5, 2);
    g.add_edge(2, 6);
    g.add_edge(6, 5);
    g.add_edge(6, 7);
    g.add_edge(7, 8);
    g.add_edge(8, 6);
    g.add_edge(6, 9);
    g.add_edge(9, 8);
    g.add_edge(2, 10);

    let wpo = WeakPartialOrdering::new(1, g.size(), &g);

    assert_eq!(13, wpo.size());

    let expected_results = [
        (1, true, false, false, 1, 0, 0),
        (2, false, true, false, 2, 1, 0),
        (3, false, true, false, 1, 1, 0),
        (4, true, false, false, 1, 1, 0),
        (3, false, false, true, 1, 1, 1),
        (6, false, true, false, 2, 1, 0),
        (7, true, false, false, 1, 1, 0),
        (9, true, false, false, 1, 1, 0),
        (8, true, false, false, 1, 2, 0),
        (6, false, false, true, 1, 1, 1),
        (5, true, false, false, 1, 2, 0),
        (2, false, false, true, 1, 1, 1),
        (10, true, false, false, 0, 1, 0),
    ];

    let mut count = HashMap::<WpoIdx, u32>::new();
    let mut worklist = Vec::new();
    worklist.push(wpo.get_entry());
    let mut i = 0;
    let mut output = String::new();
    let mut first = true;
    while let Some(v) = worklist.pop() {
        for &w in wpo.get_successors(v) {
            let c = count.entry(w).or_default();
            *c += 1;
            if *c as usize == wpo.get_num_preds(w) {
                worklist.push(w);
            }
        }

        let expected = expected_results[i];
        i += 1;
        assert_eq!(expected.0, wpo.get_node(v));
        assert_eq!(expected.1, wpo.is_plain(v));
        assert_eq!(expected.2, wpo.is_head(v));
        assert_eq!(expected.3, wpo.is_exit(v));
        assert_eq!(expected.4, wpo.get_successors(v).len());
        assert_eq!(expected.5, wpo.get_num_preds(v));
        if wpo.is_head(v) {
            assert_eq!(wpo.get_node(wpo.get_exit_of_head(v)), wpo.get_node(v));
            if !first {
                output.push_str(" ");
            }
            output.push_str("(");
            output.push_str(&wpo.get_node(v).to_string());
        } else if wpo.is_exit(v) {
            assert_eq!(expected.6, wpo.get_num_outer_preds(v).len());
            assert_eq!(wpo.get_node(wpo.get_head_of_exit(v)), wpo.get_node(v));
            output.push_str(")");
        } else {
            if !first {
                output.push_str(" ");
            }
            output.push_str(&wpo.get_node(v).to_string());
        }

        first = false;
    }
    assert_eq!(output, "1 (2 (3 4) (6 7 9 8) 5) 10");
}

#[test]
fn test_wpo_single_node() {
    let g = SimpleGraph::new(1, 1);
    let wpo = WeakPartialOrdering::new(1, 1, &g);

    assert_eq!(1, wpo.size());

    let expected_results = [(1, true, false, false, 0, 0, 0)];

    let mut count = HashMap::<WpoIdx, u32>::new();
    let mut worklist = Vec::new();
    worklist.push(wpo.get_entry());
    let mut i = 0;
    let mut output = String::new();
    let mut first = true;
    while let Some(v) = worklist.pop() {
        for &w in wpo.get_successors(v) {
            let c = count.entry(w).or_default();
            *c += 1;
            if *c as usize == wpo.get_num_preds(w) {
                worklist.push(w);
            }
        }

        let expected = expected_results[i];
        i += 1;
        assert_eq!(expected.0, wpo.get_node(v));
        assert_eq!(expected.1, wpo.is_plain(v));
        assert_eq!(expected.2, wpo.is_head(v));
        assert_eq!(expected.3, wpo.is_exit(v));
        assert_eq!(expected.4, wpo.get_successors(v).len());
        assert_eq!(expected.5, wpo.get_num_preds(v));
        if wpo.is_head(v) {
            assert_eq!(wpo.get_node(wpo.get_exit_of_head(v)), wpo.get_node(v));
            if !first {
                output.push_str(" ");
            }
            output.push_str("(");
            output.push_str(&wpo.get_node(v).to_string());
        } else if wpo.is_exit(v) {
            assert_eq!(expected.6, wpo.get_num_outer_preds(v).len());
            assert_eq!(wpo.get_node(wpo.get_head_of_exit(v)), wpo.get_node(v));
            output.push_str(")");
        } else {
            if !first {
                output.push_str(" ");
            }
            output.push_str(&wpo.get_node(v).to_string());
        }

        first = false;
    }
    assert_eq!(output, "1");
}
