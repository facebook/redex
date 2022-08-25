/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//! Tests borrowed from the MonotonicFixpointIteratorTest.cpp in the C++ version.

mod liveness {
    use std::collections::HashMap;
    use std::collections::HashSet;

    use smallvec::SmallVec;
    use sparta::datatype::HashSetAbstractDomain;
    use sparta::fixpoint_iter::FixpointIteratorTransformer;
    use sparta::fixpoint_iter::MonotonicFixpointIterator;
    use sparta::graph::Graph;
    use sparta::graph::ReverseGraph;
    use sparta::graph::ReversedRefGraph;
    use sparta::graph::DEFAULT_GRAPH_SUCCS_NUM;

    pub type NodeId = u32;
    pub type EdgeId = u32;

    pub struct Edge(NodeId, NodeId);

    pub type Symbol = &'static str;

    pub struct Statement {
        uses: Vec<Symbol>,
        defs: Vec<Symbol>,
    }

    impl Statement {
        pub fn new(uses: Vec<Symbol>, defs: Vec<Symbol>) -> Self {
            Self { uses, defs }
        }
    }

    #[derive(Default)]
    pub struct Program {
        statements: HashMap<NodeId, Statement>,
        edge_interner: Vec<Edge>,
        edges: HashMap<NodeId, HashSet<EdgeId>>,
        pred_edges: HashMap<NodeId, HashSet<EdgeId>>,
        stmt_index: NodeId,
        exit: NodeId,
    }

    impl Program {
        pub fn add_stmt(&mut self, stmt: Statement) -> NodeId {
            self.statements.insert(self.stmt_index, stmt);
            let cur_index = self.stmt_index;
            self.stmt_index += 1;
            cur_index
        }

        pub fn stmt_at(&self, n: NodeId) -> &Statement {
            self.statements
                .get(&n)
                .unwrap_or_else(|| panic!("Statement {} does not exist. ", n))
        }

        pub fn add_edge(&mut self, src: NodeId, dst: NodeId) {
            self.edge_interner.push(Edge(src, dst));
            let edge_id = self.edge_interner.len() as u32 - 1;
            self.edges.entry(src).or_default().insert(edge_id);
            self.pred_edges.entry(dst).or_default().insert(edge_id);
        }

        pub fn set_exit(&mut self, exit: NodeId) {
            self.exit = exit;
        }
    }

    impl Graph for Program {
        type NodeId = NodeId;
        type EdgeId = EdgeId;

        fn entry(&self) -> Self::NodeId {
            0
        }

        fn exit(&self) -> Self::NodeId {
            self.exit
        }

        fn predecessors(
            &self,
            n: Self::NodeId,
        ) -> SmallVec<[Self::EdgeId; DEFAULT_GRAPH_SUCCS_NUM]> {
            self.pred_edges
                .get(&n)
                .map(|v| v.iter().copied().collect())
                .unwrap_or_else(|| SmallVec::new())
        }

        fn successors(&self, n: Self::NodeId) -> SmallVec<[Self::EdgeId; DEFAULT_GRAPH_SUCCS_NUM]> {
            self.edges
                .get(&n)
                .map(|v| v.iter().copied().collect())
                .unwrap_or_else(|| SmallVec::new())
        }

        fn source(&self, e: Self::EdgeId) -> Self::NodeId {
            self.edge_interner[e as usize].0
        }

        fn target(&self, e: Self::EdgeId) -> Self::NodeId {
            self.edge_interner[e as usize].1
        }

        fn size(&self) -> usize {
            self.statements.len()
        }
    }

    type LivenessDomain = HashSetAbstractDomain<Symbol>;

    pub struct LivenessTransformer<'a> {
        graph: &'a Program,
    }

    impl<'a> LivenessTransformer<'a> {
        pub fn new(graph: &'a Program) -> Self {
            Self { graph }
        }
    }

    impl<'a> FixpointIteratorTransformer<ReversedRefGraph<'a, Program>, LivenessDomain>
        for LivenessTransformer<'a>
    {
        fn analyze_node(&self, n: NodeId, current_state: &mut LivenessDomain) {
            let stmt = self.graph.stmt_at(n);
            current_state.remove_elements(&stmt.defs);
            current_state.add_elements(stmt.uses.clone());
        }

        fn analyze_edge(&self, _: EdgeId, exit_state_at_src: &LivenessDomain) -> LivenessDomain {
            exit_state_at_src.clone()
        }
    }

    trait LivenessAnalysis {
        fn get_live_in_vars_at(&self, n: NodeId) -> LivenessDomain;
        fn get_live_out_vars_at(&self, n: NodeId) -> LivenessDomain;
    }

    type LivenessFixpointIterator<'g> = MonotonicFixpointIterator<
        'g,
        ReversedRefGraph<'g, Program>,
        LivenessDomain,
        LivenessTransformer<'g>,
    >;

    impl<'g> LivenessAnalysis for LivenessFixpointIterator<'g> {
        fn get_live_in_vars_at(&self, n: NodeId) -> LivenessDomain {
            self.get_exit_state_at(n)
        }

        fn get_live_out_vars_at(&self, n: NodeId) -> LivenessDomain {
            self.get_entry_state_at(n)
        }
    }

    /**
     *                       live in          live out
     *  1: a = 0;             {c}              {a, c}
     *  2: b = a + 1;         {a, c}           {b, c}
     *  3: c = c + b;         {b, c}           {b, c}
     *  4: a = b * 2;         {b, c}           {a, c}
     *  5: if (a < 9) {       {a, c}           {a, c}
     *       goto 2;
     *     } else {
     *  6:   return c;        {c}              {}
     *     }
     */
    fn build_program1() -> Program {
        let mut program = Program::default();
        let l1 = program.add_stmt(Statement::new(vec![], vec!["a"]));
        let l2 = program.add_stmt(Statement::new(vec!["a"], vec!["b"]));
        let l3 = program.add_stmt(Statement::new(vec!["c", "b"], vec!["c"]));
        let l4 = program.add_stmt(Statement::new(vec!["b"], vec!["a"]));
        let l5 = program.add_stmt(Statement::new(vec!["a"], vec![]));
        let l6 = program.add_stmt(Statement::new(vec!["c"], vec![]));
        program.add_edge(l1, l2);
        program.add_edge(l2, l3);
        program.add_edge(l3, l4);
        program.add_edge(l4, l5);
        program.add_edge(l5, l6);
        program.add_edge(l5, l2);
        program.set_exit(l6);
        program
    }

    /**
     *                       live in          live out
     *  1: x = a + b;        {a, b}           {x, a, b}
     *  2: y = a * b;        {x, a, b}        {x, y, a, b}
     *  3: if (y > a) {      {x, y, a, b}     {x, y, a, b}
     *  4:   return x;       {x}              {}
     *     }
     *  5: a = a + 1;        {y, a, b}        {y, a, b}
     *  6: x = a + b;        {y, a, b}        {x, y, a, b}
     *     if (...) {
     *       goto 7;
     *     }
     *     goto 3;
     *  7: x = y + a;
     *
     */
    fn build_program2() -> Program {
        let mut program = Program::default();
        let l1 = program.add_stmt(Statement::new(vec!["a", "b"], vec!["x"]));
        let l2 = program.add_stmt(Statement::new(vec!["a", "b"], vec!["y"]));
        let l3 = program.add_stmt(Statement::new(vec!["y", "a"], vec![]));
        let l4 = program.add_stmt(Statement::new(vec!["x"], vec![]));
        let l5 = program.add_stmt(Statement::new(vec!["a"], vec!["a"]));
        let l6 = program.add_stmt(Statement::new(vec!["a", "b"], vec!["x"]));
        let l7 = program.add_stmt(Statement::new(vec!["y", "a"], vec!["x"]));
        program.add_edge(l1, l2);
        program.add_edge(l2, l3);
        program.add_edge(l3, l4);
        program.add_edge(l3, l5);
        program.add_edge(l5, l6);
        program.add_edge(l6, l3);
        program.add_edge(l6, l7);
        program.set_exit(l4);
        program
    }

    /*
     *                          live in           live out
     *  1: a, b -> x, y         {a, b, z}         {a, b, x, y, z}
     *  2: x, y -> z            {x, y, a, b}      {a, b, y, z}
     *  3: a -> c               {a, b, y, z}      {c, b, y, z}
     *  4: b -> d               {c, b, y, z}      {c, d, y, z}
     *  5: c, d -> a, b         {c, d, y, z}      {a, b, y, z}
     *  6: a, b -> x            {a, b, y, z}      {a, b, x, y, z}
     *  7: return z             {z}               {}
     *  8: a, b -> c, d         {a, b, y, z}      {c, b, y, z}
     *
     *  1->2, 2->3, 3->4, 4->5, 5->6, 6->7, 6->2, 5->3, 1->8, 8->4
     */
    fn build_program3() -> Program {
        let mut program = Program::default();
        let l1 = program.add_stmt(Statement::new(vec!["a", "b"], vec!["x", "y"]));
        let l2 = program.add_stmt(Statement::new(vec!["x", "y"], vec!["z"]));
        let l3 = program.add_stmt(Statement::new(vec!["a"], vec!["c"]));
        let l4 = program.add_stmt(Statement::new(vec!["b"], vec!["d"]));
        let l5 = program.add_stmt(Statement::new(vec!["c", "d"], vec!["a", "b"]));
        let l6 = program.add_stmt(Statement::new(vec!["a", "b"], vec!["x"]));
        let l7 = program.add_stmt(Statement::new(vec!["z"], vec![]));
        let l8 = program.add_stmt(Statement::new(vec!["a", "b"], vec!["c", "d"]));
        program.add_edge(l1, l2);
        program.add_edge(l2, l3);
        program.add_edge(l3, l4);
        program.add_edge(l4, l5);
        program.add_edge(l5, l6);
        program.add_edge(l6, l7);
        program.add_edge(l6, l2);
        program.add_edge(l5, l3);
        program.add_edge(l1, l8);
        program.add_edge(l8, l4);
        program.set_exit(l7);
        program
    }

    macro_rules! assert_analysis_values {
        ( $fp:ident, $index:literal, $method:ident, [$($vars:literal),*] ) => {
            let mut elements: Vec<Symbol> = $fp.$method($index).elements().copied().collect();
            elements.sort();
            let expected: Vec<Symbol> = vec![$($vars),*];
            assert_eq!(elements, expected);
        };
    }

    macro_rules! assert_analysis {
        ( $fp:ident, $index:literal, [$($live_in_vars:literal),*], [$($live_out_vars:literal),*] ) => {
            assert!(matches!(
                $fp.get_exit_state_at($index),
                LivenessDomain::Value(_)
            ));
            assert!(matches!(
                $fp.get_entry_state_at($index),
                LivenessDomain::Value(_)
            ));
            assert_analysis_values!($fp, $index, get_live_in_vars_at, [$($live_in_vars),*]);
            assert_analysis_values!($fp, $index, get_live_out_vars_at, [$($live_out_vars),*]);
        };
    }

    #[test]
    fn test_fixpoint_iter_liveness_program1() {
        let program = build_program1();
        let reversed_program = program.rev();
        let transformer = LivenessTransformer::new(&program);
        let mut fp =
            MonotonicFixpointIterator::new(&reversed_program, 4, transformer, &reversed_program);
        fp.run(LivenessDomain::value_from_set(HashSet::new()));

        // 0: a = 0;
        assert_analysis!(fp, 0, ["c"], ["a", "c"]);

        // 1: b = a + 1;
        assert_analysis!(fp, 1, ["a", "c"], ["b", "c"]);

        // 2: c = c + b;
        assert_analysis!(fp, 2, ["b", "c"], ["b", "c"]);

        // 3: a = b * 2;
        assert_analysis!(fp, 3, ["b", "c"], ["a", "c"]);

        // 4: if (a < 9)
        assert_analysis!(fp, 4, ["a", "c"], ["a", "c"]);

        // 5: return c;
        assert_analysis!(fp, 5, ["c"], []);
    }

    #[test]
    fn test_fixpoint_iter_liveness_program2() {
        let program = build_program2();
        let reversed_program = program.rev();
        let transformer = LivenessTransformer::new(&program);
        let mut fp =
            MonotonicFixpointIterator::new(&reversed_program, 4, transformer, &reversed_program);
        fp.run(LivenessDomain::value_from_set(HashSet::new()));

        // 0: x = a + b;
        assert_analysis!(fp, 0, ["a", "b"], ["a", "b", "x"]);

        // 1: y = a * b;
        assert_analysis!(fp, 1, ["a", "b", "x"], ["a", "b", "x", "y"]);

        // 2: if (y > a);
        assert_analysis!(fp, 2, ["a", "b", "x", "y"], ["a", "b", "x", "y"]);

        // 3: return x;
        assert_analysis!(fp, 3, ["x"], []);

        // 4: a = a + 1;
        assert_analysis!(fp, 4, ["a", "b", "y"], ["a", "b", "y"]);

        // 5: x = a + b;
        assert_analysis!(fp, 5, ["a", "b", "y"], ["a", "b", "x", "y"]);

        // 7: x = y + a;
        assert!(matches!(fp.get_exit_state_at(6), LivenessDomain::Bottom));
        assert!(matches!(fp.get_entry_state_at(6), LivenessDomain::Bottom));
    }

    #[test]
    fn test_fixpoint_iter_liveness_program3() {
        let program = build_program3();
        let reversed_program = program.rev();
        let transformer = LivenessTransformer::new(&program);
        let mut fp =
            MonotonicFixpointIterator::new(&reversed_program, 4, transformer, &reversed_program);
        fp.run(LivenessDomain::value_from_set(HashSet::new()));

        // 0: a, b -> x, y
        assert_analysis!(fp, 0, ["a", "b", "z"], ["a", "b", "x", "y", "z"]);

        // 1: x, y -> z
        assert_analysis!(fp, 1, ["a", "b", "x", "y"], ["a", "b", "y", "z"]);

        // 2: a -> c
        assert_analysis!(fp, 2, ["a", "b", "y", "z"], ["b", "c", "y", "z"]);

        // 3: b -> d
        assert_analysis!(fp, 3, ["b", "c", "y", "z"], ["c", "d", "y", "z"]);

        // 4: c, d -> a, b
        assert_analysis!(fp, 4, ["c", "d", "y", "z"], ["a", "b", "y", "z"]);

        // 5: a, b -> x
        assert_analysis!(fp, 5, ["a", "b", "y", "z"], ["a", "b", "x", "y", "z"]);

        // 7: return z
        assert_analysis!(fp, 6, ["z"], []);

        // 8: a, b -> c, d
        assert_analysis!(fp, 7, ["a", "b", "y", "z"], ["b", "c", "y", "z"]);
    }
}
