/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

mod common;

use std::collections::HashMap;

use common::graph::NodeId;
use common::graph::SimpleGraph;
use sparta::graph::Graph;
use sparta::wpo::WeakPartialOrdering;
use sparta::wpo::WpoIdx;

// (NodeId in Graph, is plain node?, is head node?, is exit node?,
//  number of successors, number of predecessors, number of outer predecessors).
type NodeProperty = (NodeId, bool, bool, bool, usize, u32, usize);

fn check_wpo(wpo: WeakPartialOrdering<NodeId>, expected_results: &[NodeProperty], node_seq: &str) {
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
            if *c == wpo.get_num_preds(w) {
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
                output.push(' ');
            }
            output.push('(');
            output.push_str(&wpo.get_node(v).to_string());
        } else if wpo.is_exit(v) {
            assert_eq!(expected.6, wpo.get_num_outer_preds(v).len());
            assert_eq!(wpo.get_node(wpo.get_head_of_exit(v)), wpo.get_node(v));
            output.push(')');
        } else {
            if !first {
                output.push(' ');
            }
            output.push_str(&wpo.get_node(v).to_string());
        }

        first = false;
    }
    assert_eq!(output, node_seq);
}

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

    check_wpo(wpo, &expected_results, "1 2 (3 4 (5 6) 7) 8");
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

    check_wpo(wpo, &expected_results, "1 (2 (3 4) (6 7 9 8) 5) 10");
}

#[test]
fn test_wpo_single_node() {
    let g = SimpleGraph::new(1, 1);
    let wpo = WeakPartialOrdering::new(1, 1, &g);

    assert_eq!(1, wpo.size());

    let expected_results = [(1, true, false, false, 0, 0, 0)];

    check_wpo(wpo, &expected_results, "1");
}

//             +--+
//             v  |
// +---+     +------+
// | 1 | --> |  2   |
// +---+     +------+
#[test]
fn test_wpo_single_scc_at_end() {
    let mut g = SimpleGraph::new(1, 2);
    g.add_edge(1, 2);
    g.add_edge(2, 2);

    let wpo = WeakPartialOrdering::new(1, g.size(), &g);

    assert_eq!(3, wpo.size());

    let expected_results = [
        (1, true, false, false, 1, 0, 0),
        (2, false, true, false, 1, 1, 0),
        (2, false, false, true, 0, 1, 1),
    ];

    check_wpo(wpo, &expected_results, "1 (2)");
}

//             +--+
//             v  |
// +---+     +------+     +---+
// | 1 | <-- |  2   | --> | 3 |
// +---+     +------+     +---+
//   |         ^
//   +---------+
#[test]
fn test_wpo_single_scc_at_end_of_scc() {
    let mut g = SimpleGraph::new(1, 3);
    g.add_edge(1, 2);
    g.add_edge(2, 1);
    g.add_edge(2, 2);
    g.add_edge(2, 3);

    let wpo = WeakPartialOrdering::new(1, g.size(), &g);

    assert_eq!(5, wpo.size());
    assert!(!wpo.is_from_outside(2, 2));
    assert!(!wpo.is_from_outside(1, 2));
    assert!(wpo.is_from_outside(2, 1));

    let expected_results = [
        (1, false, true, false, 1, 0, 0),
        (2, false, true, false, 1, 1, 0),
        (2, false, false, true, 1, 1, 1),
        (1, false, false, true, 1, 1, 0),
        (3, true, false, false, 0, 1, 0),
    ];

    check_wpo(wpo, &expected_results, "(1 (2)) 3");
}

//             +---------+
//             v         |
// +---+     +---+     +---+
// | 1 | --> | 2 | --> | 3 |
// +---+     +---+     +---+
#[test]
fn test_wpo_scc_at_end() {
    let mut g = SimpleGraph::new(1, 3);
    g.add_edge(1, 2);
    g.add_edge(2, 3);
    g.add_edge(3, 2);

    let wpo = WeakPartialOrdering::new(1, g.size(), &g);

    assert!(!wpo.is_from_outside(2, 3));
    assert!(wpo.is_from_outside(2, 1));
    assert_eq!(4, wpo.size());

    let expected_results = [
        (1, true, false, false, 1, 0, 0),
        (2, false, true, false, 1, 1, 0),
        (3, true, false, false, 1, 1, 0),
        (2, false, false, true, 0, 1, 1),
    ];

    check_wpo(wpo, &expected_results, "1 (2 3)");
}

//   +-------------------+
//   |                   v
// +---+     +---+     +---+     +---+
// | 2 | <-- | 1 | <-- | 3 | --> | 4 |
// +---+     +---+     +---+     +---+
//   ^                   |
//   +-------------------+
#[test]
fn test_wpo_scc_at_end_of_scc() {
    let mut g = SimpleGraph::new(1, 4);
    g.add_edge(1, 2);
    g.add_edge(2, 3);
    g.add_edge(3, 2);
    g.add_edge(3, 1);
    g.add_edge(3, 4);

    let wpo = WeakPartialOrdering::new(1, g.size(), &g);

    assert!(!wpo.is_from_outside(1, 3));
    assert!(!wpo.is_from_outside(2, 3));
    assert!(wpo.is_from_outside(2, 1));
    assert_eq!(6, wpo.size());

    let expected_results = [
        (1, false, true, false, 1, 0, 0),
        (2, false, true, false, 1, 1, 0),
        (3, true, false, false, 1, 1, 0),
        (2, false, false, true, 1, 1, 1),
        (1, false, false, true, 1, 1, 0),
        (4, true, false, false, 0, 1, 0),
    ];

    check_wpo(wpo, &expected_results, "(1 (2 3)) 4");
}

#[test]
fn test_wpo_example_from_wpo_paper_irreducible() {
    let mut g = SimpleGraph::new(1, 6);
    g.add_edge(1, 2);
    g.add_edge(2, 3);
    g.add_edge(3, 2);
    g.add_edge(3, 4);
    g.add_edge(4, 3);
    g.add_edge(2, 5);
    g.add_edge(5, 4);
    g.add_edge(1, 6);
    g.add_edge(6, 4);

    let wpo = WeakPartialOrdering::new(1, g.size(), &g);

    assert!(!wpo.is_from_outside(3, 4));
    assert!(!wpo.is_from_outside(2, 3));
    assert!(wpo.is_from_outside(2, 6));
    assert!(wpo.is_from_outside(3, 6));
    assert!(wpo.is_from_outside(3, 5));
    assert!(!wpo.is_from_outside(2, 5));
    assert_eq!(8, wpo.size());

    let expected_results = [
        (1, true, false, false, 2, 0, 0),
        (2, false, true, false, 2, 1, 0),
        (3, false, true, false, 1, 1, 0),
        (5, true, false, false, 1, 1, 0),
        (6, true, false, false, 1, 1, 0),
        (4, true, false, false, 1, 3, 0),
        (3, false, false, true, 1, 1, 2),
        (2, false, false, true, 0, 1, 2),
    ];

    check_wpo(wpo, &expected_results, "1 (2 (3 5 6 4))");
}

#[test]
fn test_wpo_handle_outer_preds1() {
    let mut g = SimpleGraph::new(1, 93);
    g.add_edge(1, 12);
    g.add_edge(1, 16);
    g.add_edge(1, 18);
    g.add_edge(1, 26);
    g.add_edge(12, 45);
    g.add_edge(12, 75);
    g.add_edge(12, 46);
    g.add_edge(16, 74);
    g.add_edge(16, 75);
    g.add_edge(18, 92);
    g.add_edge(26, 93);
    g.add_edge(45, 46);
    g.add_edge(46, 47);
    g.add_edge(47, 73);
    g.add_edge(73, 74);
    g.add_edge(73, 75);
    g.add_edge(73, 73);
    g.add_edge(74, 46);
    g.add_edge(75, 45);
    g.add_edge(92, 93);
    g.add_edge(93, 45);
    g.add_edge(93, 46);

    let wpo = WeakPartialOrdering::new(1, g.size(), &g);

    assert_eq!(16, wpo.size());

    let expected_results = [
        (1, true, false, false, 4, 0, 0),
        (12, true, false, false, 1, 1, 0),
        (16, true, false, false, 2, 1, 0),
        (18, true, false, false, 1, 1, 0),
        (92, true, false, false, 1, 1, 0),
        (26, true, false, false, 1, 1, 0),
        (93, true, false, false, 2, 2, 0),
        (45, false, true, false, 1, 2, 0),
        (46, false, true, false, 1, 2, 0),
        (47, true, false, false, 1, 1, 0),
        (73, false, true, false, 1, 1, 0),
        (73, false, false, true, 1, 1, 1),
        (74, true, false, false, 1, 2, 0),
        (46, false, false, true, 1, 1, 2),
        (75, true, false, false, 1, 2, 0),
        (45, false, false, true, 0, 1, 4),
    ];

    check_wpo(
        wpo,
        &expected_results,
        "1 12 16 18 92 26 93 (45 (46 47 (73) 74) 75)",
    );
}

#[test]
fn test_wpo_handle_outer_preds2() {
    let mut g = SimpleGraph::new(1, 8);
    g.add_edge(1, 2);
    g.add_edge(2, 3);
    g.add_edge(3, 4);
    g.add_edge(4, 5);
    g.add_edge(5, 6);
    g.add_edge(6, 7);
    g.add_edge(6, 2);
    g.add_edge(5, 3);
    g.add_edge(1, 8);
    g.add_edge(8, 3);

    let wpo = WeakPartialOrdering::new(1, g.size(), &g);

    assert_eq!(10, wpo.size());

    let expected_results = [
        (1, true, false, false, 2, 0, 0),
        (2, false, true, false, 1, 1, 0),
        (8, true, false, false, 1, 1, 0),
        (3, false, true, false, 1, 2, 0),
        (4, true, false, false, 1, 1, 0),
        (5, true, false, false, 1, 1, 0),
        (3, false, false, true, 1, 1, 1),
        (6, true, false, false, 1, 1, 0),
        (2, false, false, true, 1, 1, 2),
        (7, true, false, false, 0, 1, 0),
    ];

    check_wpo(wpo, &expected_results, "1 (2 8 (3 4 5) 6) 7");
}

#[test]
fn test_wpo_handle_outer_preds3() {
    let mut g = SimpleGraph::new(1, 8);
    g.add_edge(1, 2);
    g.add_edge(2, 3);
    g.add_edge(3, 4);
    g.add_edge(4, 5);
    g.add_edge(5, 6);
    g.add_edge(6, 7);
    g.add_edge(6, 2);
    g.add_edge(5, 3);
    g.add_edge(1, 8);
    g.add_edge(8, 4);
    g.add_edge(7, 1);

    let wpo = WeakPartialOrdering::new(1, g.size(), &g);

    assert_eq!(11, wpo.size());

    let expected_results = [
        (1, false, true, false, 2, 0, 0),
        (2, false, true, false, 1, 1, 0),
        (3, false, true, false, 1, 1, 0),
        (8, true, false, false, 1, 1, 0),
        (4, true, false, false, 1, 2, 0),
        (5, true, false, false, 1, 1, 0),
        (3, false, false, true, 1, 1, 2),
        (6, true, false, false, 1, 1, 0),
        (2, false, false, true, 1, 1, 2),
        (7, true, false, false, 1, 1, 0),
        (1, false, false, true, 0, 1, 0),
    ];

    check_wpo(wpo, &expected_results, "(1 (2 (3 8 4 5) 6) 7)");
}

#[test]
fn test_wpo_handle_outer_preds4() {
    let mut g = SimpleGraph::new(0, 9);
    g.add_edge(0, 1);
    g.add_edge(1, 2);
    g.add_edge(2, 3);
    g.add_edge(3, 4);
    g.add_edge(4, 5);
    g.add_edge(5, 6);
    g.add_edge(6, 7);
    g.add_edge(6, 2);
    g.add_edge(5, 3);
    g.add_edge(1, 8);
    g.add_edge(8, 4);
    g.add_edge(7, 1);
    g.add_edge(7, 9);

    let wpo = WeakPartialOrdering::new(0, g.size(), &g);

    assert_eq!(13, wpo.size());

    let expected_results = [
        (0, true, false, false, 1, 0, 0),
        (1, false, true, false, 2, 1, 0),
        (2, false, true, false, 1, 1, 0),
        (3, false, true, false, 1, 1, 0),
        (8, true, false, false, 1, 1, 0),
        (4, true, false, false, 1, 2, 0),
        (5, true, false, false, 1, 1, 0),
        (3, false, false, true, 1, 1, 2),
        (6, true, false, false, 1, 1, 0),
        (2, false, false, true, 1, 1, 2),
        (7, true, false, false, 1, 1, 0),
        (1, false, false, true, 1, 1, 1),
        (9, true, false, false, 0, 1, 0),
    ];

    check_wpo(wpo, &expected_results, "0 (1 (2 (3 8 4 5) 6) 7) 9");
}

#[test]
fn test_wpo_handle_nested_loops_with_branch() {
    let mut g = SimpleGraph::new(1, 5);
    g.add_edge(1, 2);
    g.add_edge(1, 4);
    g.add_edge(2, 3);
    g.add_edge(3, 2);
    g.add_edge(3, 5);
    g.add_edge(4, 5);
    g.add_edge(5, 1);

    let wpo = WeakPartialOrdering::new(1, g.size(), &g);

    assert_eq!(7, wpo.size());

    let expected_results = [
        (1, false, true, false, 2, 0, 0),
        (2, false, true, false, 1, 1, 0),
        (3, true, false, false, 1, 1, 0),
        (2, false, false, true, 1, 1, 1),
        (4, true, false, false, 1, 1, 0),
        (5, true, false, false, 1, 2, 0),
        (1, false, false, true, 0, 1, 0),
    ];

    check_wpo(wpo, &expected_results, "(1 (2 3) 4 5)");
}
