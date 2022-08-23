/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::collections::HashMap;
use std::collections::HashSet;

use crate::datatype::AbstractDomain;
use crate::graph::Graph;

// Unlike the C++ version, we don't treat this as a base interface
// for all iterators, instead, it's only a trait for concrete type
// to analyze nodes and edges. And this can be composited in the
// fixpoint iterator type. We will revisit this part in the following
// diff for analyzer interface.
pub trait FixpointIteratorTransformer<G: Graph, D: AbstractDomain> {
    /// The *current_state* could be updated in place.
    fn analyze_node(&self, n: G::NodeId, current_state: &mut D);

    fn analyze_edge(&self, e: G::EdgeId, exit_state_at_src: &D) -> D;
}

pub struct MonotonicFixpointIteratorContext<'d, G: Graph, D: AbstractDomain> {
    // TODO: consider using value instead of reference
    init_value: &'d D,
    local_iterations: HashMap<G::NodeId, u32>,
    global_iterations: HashMap<G::NodeId, u32>,
}

impl<'d, G, D> MonotonicFixpointIteratorContext<'d, G, D>
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
        self.init_value
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

    pub fn new(init_value: &'d D) -> Self {
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
}

impl<'g, G, D, T> MonotonicFixpointIterator<'g, G, D, T>
where
    G: Graph,
    D: AbstractDomain,
    T: FixpointIteratorTransformer<G, D>,
{
    pub fn new(g: &'g G, cfg_size_hint: usize, transformer: T) -> Self {
        Self {
            graph: g,
            entry_states: HashMap::with_capacity(cfg_size_hint),
            exit_states: HashMap::with_capacity(cfg_size_hint),
            transformer,
        }
    }

    /// Default strategy for applying widening operator (apply
    /// join at the first iteration and then widening in all
    /// rest iterations).
    pub fn extrapolate<'d>(
        &self,
        context: &MonotonicFixpointIteratorContext<'d, G, D>,
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

    pub fn get_state_at_or_bottom(states: &HashMap<G::NodeId, D>, n: G::NodeId) -> D {
        states.get(&n).cloned().unwrap_or_else(D::bottom)
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

    pub fn compute_entry_state<'d>(
        graph: &'g G,
        exit_states: &HashMap<G::NodeId, D>,
        transformer: &T,
        context: &MonotonicFixpointIteratorContext<'d, G, D>,
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

    pub fn analyze_vertex<'d>(
        &mut self,
        context: &MonotonicFixpointIteratorContext<'d, G, D>,
        n: G::NodeId,
    ) {
        let entry_state = self.entry_states.entry(n).or_insert_with(D::bottom);
        Self::compute_entry_state(
            self.graph,
            &self.exit_states,
            &self.transformer,
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
