# SPARTA

<img src="https://github.com/facebook/SPARTA/raw/main/SPARTA.png" width="300" height="300"/>

[![Support Ukraine](https://img.shields.io/badge/Support-Ukraine-FFD500?style=flat&labelColor=005BBB)](https://opensource.fb.com/support-ukraine)
![Rust Build](https://github.com/facebook/SPARTA/actions/workflows/rust.yml/badge.svg)
[![crates.io](https://img.shields.io/crates/v/sparta.svg)](https://crates.io/crates/sparta)

SPARTA is a library of software components specially designed for building high-performance static analyzers based on the theory of Abstract Interpretation.

## Abstract Interpretation

[Abstract Interpretation](https://en.wikipedia.org/wiki/Abstract_interpretation) is a theory of semantic approximation that provides a foundational framework for the design of static program analyzers. Static analyzers built following the methodology of Abstract Interpretation are mathematically sound, i.e., the semantic information they compute is guaranteed to hold in all possible execution contexts considered. Moreover, these analyzers are able to infer complex properties of programs, the expressiveness of which can be finely tuned in order to control the analysis time. Static analyzers based on Abstract Interpretation are routinely used to perform the formal verification of flight software in the aerospace industry, for example.

## Why SPARTA?

Building an industrial-grade static analysis tool based on Abstract Interpretation from scratch is a daunting task that requires the attention of experts in the field. The purpose of SPARTA is to drastically simplify the engineering of Abstract Interpretation by providing a set of software components that have a simple API, are highly performant and can be easily assembled to build a production-quality static analyzer. By encapsulating the complex implementation details of Abstract Interpretation, SPARTA lets the tool developer focus on the three fundamental axes in the design of an analysis:

* **Semantics:** the program properties to analyze (range of numerical variables, aliasing relation, etc.)
* **Partitioning:** the granularity of the properties analyzed (intraprocedural, interprocedural, context-sensitive, etc.)
* **Abstraction:** the representation of the program properties (sign, interval, alias graph, etc.)

SPARTA is an acronym that stands for **S**emantics, **PART**itioning and **A**bstraction.

SPARTA for C++ is the analytic engine that powers most optimization passes of the [ReDex](https://github.com/facebook/redex) Android bytecode optimizer. The ReDex codebase contains multiple examples of analyses built with SPARTA that run at industrial scale. The [interprocedural constant propagation](https://github.com/facebook/redex/tree/main/service/constant-propagation) illustrates how to assemble the building blocks provided by SPARTA in order to implement a sophisticated yet scalable analysis.

## SPARTA in Rust

SPARTA for Rust is published as a crate on [crates.io](https://crates.io/crates/sparta). It is still in an experimental stage and there's no guarantee that the API won't change. So far, we have reimplemented the basic functions found in the C++ version, namely:

* **Abstract Domains**: The SPARTA crate models abstract domains as a trait. The C++ version uses CRTP and `static_asserts` to ensure the user defined type satisfies the quality of an abstract domain. This is no longer necessary in Rust.
* **Data Structures**: We have implemented PatriciaTree based Set and Map containers along with the abstract domains.
* **Graph Trait**: SPARTA fixpoint iterator can operate on user defined graph types as long as they implement the `sparta::graph::Graph` trait. This is analogous to the Graph interface in C++, which uses CRTP for compile-time polymorphism.
* **Weak Partial Ordering**: A replacement for Weak Topological Ordering described in the paper. Sung Kook Kim, Arnaud J. Venet, and Aditya V. Thakur. 2019. Deterministic parallel fixpoint computation. Proc. ACM Program. Lang. 4, POPL, Article 14 (January 2020), 33 pages. https://doi.org/10.1145/3371082https://dl.acm.org/doi/10.1145/3371082
* **Fixpoint Iteration**: A single threaded fixpoint iterator based on Weak Partial Ordering.

## Issues

Issues on GitHub are assigned priorities which reflect their urgency and how soon they are likely to be addressed.

* P0: Unbreak now! A serious issue which should have someone working on it right now.
* P1: High Priority. An important issue that someone should be actively working on.
* P2: Mid Priority. An important issue which is in the queue to be processed soon.
* P3: Low Priority. An important issue which may get dealt with at a later date.
* P4: Wishlist. An issue with merit but low priority which is up for grabs but likely to be pruned if not addressed after a reasonable period.

## License

SPARTA is MIT-licensed, see the [LICENSE](LICENSE) file in the root directory of this source tree.
