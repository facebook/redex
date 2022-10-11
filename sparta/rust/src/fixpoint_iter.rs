/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::borrow::Cow;
use std::collections::HashMap;
use std::collections::HashSet;
use std::collections::VecDeque;
use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering;

use crate::datatype::AbstractDomain;
use crate::graph::Graph;
use crate::graph::SuccessorNodes;
use crate::wpo::WeakPartialOrdering;
use crate::wpo::WpoIdx;

// Unlike the C++ version, we don't treat this as a base interface
// for all iterators, instead, it's only a trait for concrete type
// to analyze nodes and edges. And this can be composited in the
// fixpoint iterator type. We will revisit this part in the following
// diff for analyzer interface.
pub trait FixpointIteratorTransformer<G: Graph, D: AbstractDomain> {
    /// The *current_state* could be updated in place.
    fn analyze_node(&mut self, n: G::NodeId, current_state: &mut D);

    fn analyze_edge(&mut self, e: G::EdgeId, exit_state_at_src: &D) -> D;
}

pub struct MonotonicFixpointIteratorContext<G: Graph, D: AbstractDomain> {
    init_value: D,
    local_iterations: HashMap<G::NodeId, u32>,
    global_iterations: HashMap<G::NodeId, u32>,
}

impl<G, D> MonotonicFixpointIteratorContext<G, D>
where
    G: Graph,
    D: AbstractDomain,
{
    pub fn get_local_iterations_for(&self, n: G::NodeId) -> u32 {
        *self.local_iterations.get(&n).unwrap_or(&0)
    }

    pub fn get_global_iterations_for(&self, n: G::NodeId) -> u32 {
        *self.global_iterations.get(&n).unwrap_or(&0)
    }

    pub fn get_init_value(&self) -> &D {
        &self.init_value
    }

    fn increase_iteration_count(n: G::NodeId, table: &mut HashMap<G::NodeId, u32>) {
        *table.entry(n).or_default() += 1;
    }

    pub fn increase_iteration_count_for(&mut self, n: G::NodeId) {
        Self::increase_iteration_count(n, &mut self.local_iterations);
        Self::increase_iteration_count(n, &mut self.global_iterations);
    }

    pub fn reset_local_iteration_count_for(&mut self, n: G::NodeId) {
        *self.local_iterations.entry(n).or_default() = 0;
    }

    pub fn new(init_value: D) -> Self {
        Self {
            init_value,
            local_iterations: Default::default(),
            global_iterations: Default::default(),
        }
    }

    pub fn with_nodes(mut self, nodes: &HashSet<G::NodeId>) -> Self {
        for &node in nodes {
            *self.global_iterations.entry(node).or_default() = 0;
            *self.local_iterations.entry(node).or_default() = 0;
        }
        self
    }
}

pub struct MonotonicFixpointIterator<
    'g,
    G: Graph,
    D: AbstractDomain,
    T: FixpointIteratorTransformer<G, D>,
> {
    graph: &'g G,
    entry_states: HashMap<G::NodeId, D>,
    exit_states: HashMap<G::NodeId, D>,
    transformer: T,
    wpo: WeakPartialOrdering<G::NodeId>,
}

impl<'g, G, D, T> MonotonicFixpointIterator<'g, G, D, T>
where
    G: Graph,
    D: AbstractDomain,
    T: FixpointIteratorTransformer<G, D>,
{
    pub fn new<SN>(g: &'g G, cfg_size_hint: usize, transformer: T, successors_nodes: &SN) -> Self
    where
        SN: SuccessorNodes<NodeId = G::NodeId>,
    {
        let wpo = WeakPartialOrdering::new(g.entry(), g.size(), successors_nodes);
        Self {
            graph: g,
            entry_states: HashMap::with_capacity(cfg_size_hint),
            exit_states: HashMap::with_capacity(cfg_size_hint),
            transformer,
            wpo,
        }
    }

    pub fn run(&mut self, init_value: D) {
        self.clear();

        let mut context = MonotonicFixpointIteratorContext::new(init_value);
        let wpo_counter: Vec<AtomicU32> =
            (0..self.wpo.size()).map(|_| Default::default()).collect();

        let mut worklist = VecDeque::new();
        let entry_idx = self.wpo.get_entry();
        worklist.push_front(entry_idx);
        assert_eq!(self.wpo.get_num_preds(entry_idx), 0);

        let mut process_node = |wpo_idx: WpoIdx, worklist: &mut VecDeque<WpoIdx>| {
            assert_eq!(
                wpo_counter[wpo_idx as usize].load(Ordering::Relaxed),
                self.wpo.get_num_preds(wpo_idx)
            );

            wpo_counter[wpo_idx as usize].store(0, Ordering::Relaxed);

            if !self.wpo.is_exit(wpo_idx) {
                self.analyze_vertex(&context, self.wpo.get_node(wpo_idx));

                for &succ_idx in self.wpo.get_successors(wpo_idx) {
                    let old_counter =
                        wpo_counter[succ_idx as usize].fetch_add(1, Ordering::Relaxed);
                    if old_counter + 1 == self.wpo.get_num_preds(succ_idx) {
                        worklist.push_back(succ_idx);
                    }
                }

                return;
            }

            let head_idx = self.wpo.get_head_of_exit(wpo_idx);
            let head = self.wpo.get_node(head_idx);
            let current_state = self.entry_states.entry(head).or_insert_with(D::bottom);
            let mut new_state = D::bottom();
            Self::compute_entry_state(
                self.graph,
                &self.exit_states,
                &mut self.transformer,
                &context,
                head,
                &mut new_state,
            );

            if new_state.leq(current_state) {
                context.reset_local_iteration_count_for(head);
                *current_state = new_state;

                for &succ_idx in self.wpo.get_successors(wpo_idx) {
                    let old_counter =
                        wpo_counter[succ_idx as usize].fetch_add(1, Ordering::Relaxed);
                    if old_counter + 1 == self.wpo.get_num_preds(succ_idx) {
                        worklist.push_back(succ_idx);
                    }
                }
            } else {
                Self::extrapolate(&context, head, current_state, new_state);
                context.increase_iteration_count_for(head);
                for (&component_idx, &num) in self.wpo.get_num_outer_preds(wpo_idx) {
                    assert!(component_idx != entry_idx);
                    let old_counter =
                        wpo_counter[component_idx as usize].fetch_add(num, Ordering::Relaxed);
                    if old_counter + num == self.wpo.get_num_preds(component_idx) {
                        worklist.push_back(component_idx);
                    }
                }

                if head_idx == entry_idx {
                    worklist.push_back(head_idx);
                }
            }
        };

        while let Some(idx) = worklist.pop_front() {
            process_node(idx, &mut worklist);
        }

        for counter in wpo_counter {
            assert_eq!(counter.load(Ordering::Relaxed), 0);
        }
    }

    /// Default strategy for applying widening operator (apply
    /// join at the first iteration and then widening in all
    /// rest iterations).
    pub fn extrapolate(
        context: &MonotonicFixpointIteratorContext<G, D>,
        n: G::NodeId,
        current_state: &mut D,
        new_state: D,
    ) {
        if 0 == context.get_global_iterations_for(n) {
            // TODO: we need to revisit this design, should we use
            // move or clone of domain?
            current_state.join_with(new_state);
        } else {
            current_state.widen_with(new_state);
        }
    }

    fn get_state_at_or_bottom(states: &HashMap<G::NodeId, D>, n: G::NodeId) -> Cow<'_, D> {
        if let Some(state) = states.get(&n) {
            Cow::Borrowed(state)
        } else {
            Cow::Owned(D::bottom())
        }
    }

    pub fn get_entry_state_at(&self, n: G::NodeId) -> Cow<'_, D> {
        Self::get_state_at_or_bottom(&self.entry_states, n)
    }

    pub fn get_exit_state_at(&self, n: G::NodeId) -> Cow<'_, D> {
        Self::get_state_at_or_bottom(&self.exit_states, n)
    }

    pub fn clear(&mut self) {
        self.entry_states.clear();
        self.entry_states.shrink_to_fit();
        self.exit_states.clear();
        self.exit_states.shrink_to_fit();
    }

    pub fn set_all_to_bottom(&mut self, all_nodes: &HashSet<G::NodeId>) {
        for &node in all_nodes {
            self.entry_states
                .entry(node)
                .and_modify(|s| *s = D::bottom())
                .or_insert_with(D::bottom);
            self.exit_states
                .entry(node)
                .and_modify(|s| *s = D::bottom())
                .or_insert_with(D::bottom);
        }
    }

    pub fn compute_entry_state(
        graph: &'g G,
        exit_states: &HashMap<G::NodeId, D>,
        transformer: &mut T,
        context: &MonotonicFixpointIteratorContext<G, D>,
        n: G::NodeId,
        entry_state: &mut D,
    ) {
        if n == graph.entry() {
            entry_state.join_with(context.get_init_value().clone());
        }

        for e in graph.predecessors(n) {
            let d = Self::get_state_at_or_bottom(exit_states, graph.source(e));
            entry_state.join_with(transformer.analyze_edge(e, &d));
        }
    }

    pub fn analyze_vertex(
        &mut self,
        context: &MonotonicFixpointIteratorContext<G, D>,
        n: G::NodeId,
    ) {
        let entry_state = self.entry_states.entry(n).or_insert_with(D::bottom);
        Self::compute_entry_state(
            self.graph,
            &self.exit_states,
            &mut self.transformer,
            context,
            n,
            entry_state,
        );
        let exit_state = self
            .exit_states
            .entry(n)
            .and_modify(|s| *s = entry_state.clone())
            .or_insert_with(|| entry_state.clone());
        self.transformer.analyze_node(n, exit_state);
    }
}
