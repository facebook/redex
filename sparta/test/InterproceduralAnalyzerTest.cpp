/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AbstractDomain.h"
#include "Analyzer.h"
#include "FiniteAbstractDomain.h"
#include "HashedSetAbstractDomain.h"
#include "MonotonicFixpointIterator.h"
#include "PatriciaTreeMapAbstractEnvironment.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace language {

enum Opcode {
  THROW,
  CALL,
  CONST,
};

class ControlFlowGraph;

struct Function {
  std::string name;
  std::shared_ptr<ControlFlowGraph> cfg = nullptr;
};

using FunctionId = Function*;

struct Program {
  std::vector<Function*> functions;
  Function* entry;
  Program(std::vector<Function*> program, Function* entry)
      : functions(std::move(program)), entry(entry) {}
};

struct Statement {
  Opcode op;
  Function* callee;
  explicit Statement(Opcode op, Function* callee) : op(op), callee(callee) {}
  explicit Statement(Opcode op) : op(op), callee(nullptr) {}
};

struct ControlPoint {
  std::string label;

  explicit ControlPoint(const std::string& l) : label(l) {}
};

static bool operator==(const ControlPoint& cp1, const ControlPoint& cp2) {
  return cp1.label == cp2.label;
}

} // namespace language

namespace std {

using namespace language;

template <>
struct hash<language::ControlPoint> {
  size_t operator()(const ControlPoint& cp) const noexcept {
    return std::hash<std::string>{}(cp.label);
  }
};

template <>
struct hash<std::pair<Function*, Function*>> {
  size_t operator()(
      const std::pair<Function*, Function*>& pair) const noexcept {
    Function *from, *to;
    std::tie(from, to) = pair;
    return ((size_t)from) ^ ((size_t)to);
  }
};

} // namespace std

namespace language {
/*
 * A program is a control-flow graph where each node is labeled with a
 * statement.
 */
class ControlFlowGraph final {
 public:
  using Edge = std::pair<ControlPoint, ControlPoint>;
  using EdgeId = std::shared_ptr<Edge>;

  explicit ControlFlowGraph(const std::string& entry)
      : m_entry(entry), m_exit(entry) {}

  std::vector<EdgeId> successors(const ControlPoint& node) const {
    auto it = m_successors.find(node);
    if (it != m_successors.end()) {
      return std::vector<EdgeId>(it->second.begin(), it->second.end());
    } else {
      return {};
    }
  }

  std::vector<EdgeId> predecessors(const ControlPoint& node) const {
    auto it = m_predecessors.find(node);
    if (it != m_predecessors.end()) {
      return std::vector<EdgeId>(it->second.begin(), it->second.end());
    } else {
      return {};
    }
  }

  const Statement& statement_at(const ControlPoint& node) const {
    auto it = m_statements.find(node);
    if (it == m_statements.end()) {
      fail(node);
    }
    return it->second;
  }

  void add(const std::string& node, const Statement& stmt) {
    ControlPoint cp(node);
    m_statements.emplace(std::make_pair(cp, stmt));
    // Ensure that the pred/succ entries for the node are initialized
    m_predecessors[cp];
    m_successors[cp];
  }

  void add_edge(const std::string& src, const std::string& dst) {
    ControlPoint src_cp(src);
    ControlPoint dst_cp(dst);
    auto edge = std::make_shared<Edge>(src_cp, dst_cp);
    m_successors[src_cp].insert(edge);
    m_predecessors[dst_cp].insert(edge);
  }

  void set_exit(const std::string& exit) { m_exit = ControlPoint(exit); }

  ControlPoint get_entry_point() const { return m_entry; }
  ControlPoint get_exit_point() const { return m_exit; }

  std::unordered_map<ControlPoint, Statement> statements() const {
    return m_statements;
  }

 private:
  // In gtest, FAIL (or any ASSERT_* statement) can only be called from within a
  // function that returns void.
  void fail(const ControlPoint& node) const {
    FAIL() << "No statement at node " << node.label;
  }

  ControlPoint m_entry;
  ControlPoint m_exit;
  std::unordered_map<ControlPoint, Statement> m_statements;
  std::unordered_map<ControlPoint, std::unordered_set<EdgeId>> m_successors;
  std::unordered_map<ControlPoint, std::unordered_set<EdgeId>> m_predecessors;

  friend class ControlFlowGraphInterface;
};

static const ControlFlowGraph& build_cfg(const Function* f) {
  if (f && f->cfg) {
    return *f->cfg;
  }
  throw std::runtime_error("function doesn't appear to have cfg");
}

class ControlFlowGraphInterface {
 public:
  using Graph = ControlFlowGraph;
  using NodeId = ControlPoint;
  using EdgeId = Graph::EdgeId;

  static NodeId entry(const Graph& graph) { return graph.m_entry; }
  static NodeId exit(const Graph& graph) { return graph.m_exit; }
  static std::vector<EdgeId> predecessors(const Graph& graph,
                                          const NodeId& node) {
    return graph.predecessors(node);
  }
  static std::vector<EdgeId> successors(const Graph& graph,
                                        const NodeId& node) {
    return graph.successors(node);
  }
  static NodeId source(const Graph&, const EdgeId& e) { return e->first; }
  static NodeId target(const Graph&, const EdgeId& e) { return e->second; }
};

struct CallGraph {
  using Node = Function;
  using NodeId = Function*;
  using Edge = std::pair<NodeId, NodeId>;
  using EdgeId = std::shared_ptr<Edge>;

  explicit CallGraph(const NodeId& entry) : m_entry(entry), m_exit(entry) {}

  std::vector<EdgeId> successors(const NodeId& node) const {
    auto it = m_successors.find(node);
    if (it != m_successors.end()) {
      return std::vector<EdgeId>(it->second.begin(), it->second.end());
    } else {
      return {};
    }
  }

  std::vector<EdgeId> predecessors(const NodeId& node) const {
    auto it = m_predecessors.find(node);
    if (it != m_predecessors.end()) {
      return std::vector<EdgeId>(it->second.begin(), it->second.end());
    } else {
      return {};
    }
  }

  void add_edge(const NodeId& src, const NodeId& dst) {
    if (m_edges.find(Edge(src, dst)) == m_edges.end()) {
      auto edge = std::make_shared<Edge>(src, dst);
      m_successors[src].insert(edge);
      m_predecessors[dst].insert(edge);
    }
  }

  void set_exit(const NodeId& exit) { m_exit = exit; }

  NodeId get_entry_point() const { return m_entry; }
  NodeId get_exit_point() const { return m_exit; }

 private:
  NodeId m_entry;
  NodeId m_exit;

  std::unordered_set<Edge> m_edges;
  std::unordered_map<NodeId, std::unordered_set<EdgeId>> m_successors;
  std::unordered_map<NodeId, std::unordered_set<EdgeId>> m_predecessors;

  friend class CallGraphInterface;
};

class CallGraphInterface {
 public:
  using Graph = CallGraph;
  using NodeId = Graph::NodeId;
  using EdgeId = Graph::EdgeId;

  static NodeId entry(const Graph& graph) { return graph.m_entry; }
  static NodeId exit(const Graph& graph) { return graph.m_exit; }
  static std::vector<EdgeId> predecessors(const Graph& graph,
                                          const NodeId& node) {
    return graph.predecessors(node);
  }
  static std::vector<EdgeId> successors(const Graph& graph,
                                        const NodeId& node) {
    return graph.successors(node);
  }
  static NodeId source(const Graph&, const EdgeId& e) { return e->first; }
  static NodeId target(const Graph&, const EdgeId& e) { return e->second; }
};

struct AnalysisAdaptorBase {
  using Function = language::Function*;
  using Program = language::Program*;
  using CallGraphInterface = language::CallGraphInterface;

  template <typename GraphInterface, typename Domain>
  using FixpointIteratorBase =
      sparta::MonotonicFixpointIterator<GraphInterface, Domain>;

  // This structure should own the built graph because nowhere else in the IR
  // owns it
  static std::unordered_map<language::Program*, language::CallGraph>
      call_graph_cache;

  template <typename FunctionSummaries>
  static const language::CallGraph& call_graph_of(language::Program* program,
                                                  FunctionSummaries*) {
    auto it = call_graph_cache.find(program);
    if (it != call_graph_cache.end()) {
      return it->second;
    }

    language::CallGraph graph(program->entry);
    for (const auto& func : program->functions) {
      auto cfg = func->cfg;
      for (auto entry : cfg->statements()) {
        auto stmt = entry.second;
        switch (stmt.op) {
        case language::Opcode::CALL: {
          Function callee = stmt.callee;
          graph.add_edge(func, callee);
          break;
        }
        default:
          break;
        }
      }
    }
    call_graph_cache.emplace(program, std::move(graph));
    return call_graph_cache.at(program);
  }

  static language::Function* function_by_node_id(
      const typename CallGraphInterface::NodeId& node) {
    return node;
  }
};

std::unordered_map<Program*, CallGraph> AnalysisAdaptorBase::call_graph_cache;
} // namespace language

namespace purity_interprocedural {

struct Summary : public sparta::AbstractDomain<Summary> {
  explicit Summary() {
    m_pure = true;
    m_kind = sparta::AbstractValueKind::Bottom;
  }

  virtual bool is_bottom() const {
    return m_kind == sparta::AbstractValueKind::Bottom;
  }

  virtual bool is_value() const {
    return m_kind == sparta::AbstractValueKind::Value;
  }

  virtual bool is_top() const {
    return m_kind == sparta::AbstractValueKind::Top;
  }

  virtual bool leq(const Summary& other) const {
    if (is_bottom()) {
      return true;
    } else if (m_kind == sparta::AbstractValueKind::Value) {
      return other.is_top() ||
             (other.is_value() ? m_pure == other.m_pure : false);
    } else {
      return other.is_top();
    }
  }

  virtual bool equals(const Summary& other) const {
    if (m_kind != other.m_kind) {
      return false;
    } else {
      return (m_kind == sparta::AbstractValueKind::Value)
                 ? m_pure == other.m_pure
                 : true;
    }
  }

  virtual void set_to_bottom() { m_kind = sparta::AbstractValueKind::Bottom; }
  virtual void set_value(bool pure) {
    m_kind = sparta::AbstractValueKind::Value;
    m_pure = pure;
  }
  virtual void set_to_top() { m_kind = sparta::AbstractValueKind::Top; }

  virtual void join_with(const Summary& other) {
    if (is_bottom() || other.is_top()) {
      *this = other;
    } else if (is_value() && other.is_value()) {
      m_pure &= other.m_pure;
    }
  }
  virtual void widen_with(const Summary& other) {}
  virtual void meet_with(const Summary& other) {}
  virtual void narrow_with(const Summary& other) {}

  bool pure() { return m_pure; }

 private:
  bool m_pure;
  sparta::AbstractValueKind m_kind;
};

enum Elements { BOTTOM, PURE, IMPURE, TOP };
using Lattice = sparta::BitVectorLattice<Elements, /* kCardinality */ 4>;
Lattice lattice({BOTTOM, PURE, IMPURE, TOP},
                {{BOTTOM, PURE}, {BOTTOM, IMPURE}, {PURE, TOP}, {IMPURE, TOP}});

using PurityDomain = sparta::
    FiniteAbstractDomain<Elements, Lattice, Lattice::Encoding, &lattice>;

struct CallsiteEdgeTarget {
  // TODO: This can be a Reduced Product with the current calling context?
  using Domain = sparta::HashedSetAbstractDomain<language::Function*>;

  Domain analyze_edge(const language::CallGraph::EdgeId&,
                      const Domain& domain) {
    return domain;
  }
};

using CallerContext = typename CallsiteEdgeTarget::Domain;

template <typename Base>
class SimpleFunctionAnalyzer : public Base {
 private:
  language::Function* m_fun;
  language::ControlFlowGraph m_cfg;
  PurityDomain m_domain;

 public:
  SimpleFunctionAnalyzer(language::Function* fun)
      : m_fun(fun), m_cfg(language::build_cfg(m_fun)), m_domain(PURE) {}

  virtual void analyze() override {
    for (auto entry : m_cfg.statements()) {
      auto stmt = entry.second;
      switch (stmt.op) {
      case language::Opcode::CONST: {
        // do nothing
        break;
      }
      case language::Opcode::CALL: {
        if (m_domain != PurityDomain(IMPURE)) {
          // No action needed if the function is already impure at this control
          // point, otherwise we grab the summary for the callee
          auto func = stmt.callee;
          auto summary = this->get_summaries()->get(func);

          boost::optional<bool> maybe_pure = boost::none;

          if (summary.is_value()) {
            maybe_pure = summary.pure();
          }
          // note: we may not have a useful summary for the callee yet
          if (maybe_pure) {
            if (*maybe_pure) {
              // The callee is pure, doesn't do anything to state
            } else {
              // The callee is impure, set to impure
              m_domain = PurityDomain(IMPURE);
            }
          } else {
            // does not have result for callee, set to top
            m_domain.set_to_top();
          }
        }
        break;
      }
      case language::Opcode::THROW: {
        m_domain = PurityDomain(IMPURE);
        break;
      }
      }
    }
  }

  virtual void summarize() override {
    Summary conclusion;
    conclusion.set_to_top();
    if (m_domain.is_top()) {
      conclusion.set_to_top();
    } else if (m_domain.is_bottom()) {
      conclusion.set_to_bottom();
    } else {
      if (m_domain == PurityDomain(PURE)) {
        conclusion.set_value(true);
      } else {
        conclusion.set_value(false);
      }
    }
    this->get_summaries()->update(m_fun,
                                  [&](const Summary&) { return conclusion; });
  }
};

template <typename Base>
class FunctionFixpoint final : public sparta::MonotonicFixpointIterator<
                                   language::ControlFlowGraphInterface,
                                   PurityDomain>,
                               public Base {

 private:
  language::Function* m_function;
  PurityDomain initial_domain() { return PurityDomain(PURE); }

 public:
  explicit FunctionFixpoint(const language::FunctionId& function)
      : MonotonicFixpointIterator(language::build_cfg(function)) {}

  // Introduced by sparta::Intraprocedural
  virtual void analyze() override {
    this->run(this->initial_domain());

    // We don't care about context in this analysis but the context is relevant
    // for the call graph level to reach fixpoint. When necessary, the
    // intraprocedural part should also update context when hitting call
    // edges.
    this->get_caller_context()->add(m_function);
  }

  virtual void analyze_node(const language::ControlPoint& node,
                            PurityDomain* current_state) const override {
    auto stmt = m_graph.statement_at(node);
    switch (stmt.op) {
    case language::Opcode::CONST: {
      // do nothing
      break;
    }
    case language::Opcode::CALL: {
      if (*current_state != PurityDomain(IMPURE)) {
        // No action needed if the function is already impure at this control
        // point, otherwise we grab the summary for the callee
        auto func = stmt.callee;
        auto summary = this->get_summaries()->get(func);

        boost::optional<bool> maybe_pure = boost::none;

        if (summary.is_value()) {
          maybe_pure = summary.pure();
        }
        // note: we may not have a useful summary for the callee yet
        if (maybe_pure) {
          if (*maybe_pure) {
            // The callee is pure, doesn't do anything to state
          } else {
            // The callee is impure, set to impure
            *current_state = PurityDomain(IMPURE);
          }
        } else {
          // does not have result for callee, set to top
          current_state->set_to_top();
        }
      }
      break;
    }
    case language::Opcode::THROW: {
      *current_state = PurityDomain(IMPURE);
      break;
    }
    }
  }

  virtual PurityDomain analyze_edge(
      const language::ControlFlowGraph::EdgeId&,
      const PurityDomain& exit_state_at_source) const override {
    // Edges have no semantic transformers attached.
    return exit_state_at_source;
  }

  // Introduced by sparta::Intraprocedural
  virtual void summarize() override {
    Summary conclusion;
    conclusion.set_to_top();
    auto domain = get_exit_state_at(m_graph.get_exit_point());
    if (domain.is_top()) {
      conclusion.set_to_top();
    } else if (domain.is_bottom()) {
      conclusion.set_to_bottom();
    } else {
      if (domain == PurityDomain(PURE)) {
        conclusion.set_value(true);
      } else {
        conclusion.set_value(false);
      }
    }
    this->get_summaries()->update(m_function,
                                  [&](const Summary&) { return conclusion; });
  }
};

class AnalysisRegistry : public sparta::AbstractRegistry {
 private:
  sparta::PatriciaTreeMapAbstractEnvironment<language::Function*, Summary>
      m_env;
  bool m_has_update = false;

 public:
  bool has_update() const override { return m_has_update; }

  void materialize_update() override { m_has_update = false; }

  void update(language::Function* func,
              std::function<Summary(const Summary&)> update) {
    m_env.update(func, update);
    m_has_update = true;
  }

  Summary get(language::Function* func) { return m_env.get(func); }
};

struct PurityAnalysisAdaptor : public language::AnalysisAdaptorBase {
  using Registry = AnalysisRegistry;

  template <typename IntraproceduralBase>
  using FunctionAnalyzer = SimpleFunctionAnalyzer<IntraproceduralBase>;
  using Callsite = CallsiteEdgeTarget;

  // Optional: override call graph level fixpoint iterator type
  template <typename GraphInterface, typename Domain>
  using FixpointIteratorBase =
      sparta::WTOMonotonicFixpointIterator<GraphInterface, Domain>;
};

using Analysis = sparta::InterproceduralAnalyzer<PurityAnalysisAdaptor>;

} // namespace purity_interprocedural

static void test1() {
  using namespace language;

  Function fun1, fun2, fun3, fun4, fun5, fun6, fun7, mainfun;
  fun1.name = "fun1";
  fun1.cfg = std::make_shared<ControlFlowGraph>("1");
  fun1.cfg->add("1", Statement(Opcode::CONST));
  fun1.cfg->set_exit("1");

  fun2.name = "fun2";
  fun2.cfg = std::make_shared<ControlFlowGraph>("1");
  fun2.cfg->add("1", Statement(Opcode::THROW));
  fun2.cfg->add("2", Statement(Opcode::CALL, &fun1));
  fun2.cfg->add_edge("1", "2");
  fun2.cfg->set_exit("2");

  fun3.name = "fun3";
  fun3.cfg = std::make_shared<ControlFlowGraph>("1");
  fun3.cfg->add("1", Statement(Opcode::CALL, &fun1));
  fun3.cfg->set_exit("1");

  fun4.name = "fun4";
  fun4.cfg = std::make_shared<ControlFlowGraph>("1");
  fun4.cfg->add("1", Statement(Opcode::CALL, &fun2));
  fun4.cfg->set_exit("1");

  fun5.name = "fun5";
  fun5.cfg = std::make_shared<ControlFlowGraph>("1");
  fun5.cfg->add("1", Statement(Opcode::CALL, &fun6));
  fun5.cfg->set_exit("1");

  fun6.name = "fun6";
  fun6.cfg = std::make_shared<ControlFlowGraph>("1");
  fun6.cfg->add("1", Statement(Opcode::CALL, &fun5));
  fun6.cfg->set_exit("1");

  fun7.name = "fun7";
  fun7.cfg = std::make_shared<ControlFlowGraph>("1");
  fun7.cfg->add("1", Statement(Opcode::CONST));
  fun7.cfg->set_exit("1");

  mainfun.name = "mainfun";
  mainfun.cfg = std::make_shared<ControlFlowGraph>("1");
  mainfun.cfg->add("1", Statement(Opcode::CALL, &fun5));
  mainfun.cfg->add("2", Statement(Opcode::CALL, &fun3));
  mainfun.cfg->add("3", Statement(Opcode::CALL, &fun4));
  mainfun.cfg->add_edge("1", "2");
  mainfun.cfg->add_edge("2", "3");
  mainfun.cfg->add_edge("3", "1");
  mainfun.cfg->set_exit("3");

  std::vector<Function*> functions{&fun1, &fun2, &fun3, &fun4,
                                   &fun5, &fun6, &fun7, &mainfun};
  Program prog(std::move(functions), &mainfun);
  purity_interprocedural::Analysis inter(&prog, 20 /* max iteration */);
  inter.run();

  ASSERT_TRUE(inter.registry.get(&fun1).is_value());
  EXPECT_TRUE(inter.registry.get(&fun1).pure());

  ASSERT_TRUE(inter.registry.get(&fun3).is_value());
  EXPECT_TRUE(inter.registry.get(&fun3).pure());

  ASSERT_TRUE(inter.registry.get(&mainfun).is_value());
  EXPECT_FALSE(inter.registry.get(&mainfun).pure());

  ASSERT_TRUE(inter.registry.get(&fun2).is_value());
  EXPECT_FALSE(inter.registry.get(&fun2).pure());

  ASSERT_TRUE(inter.registry.get(&fun4).is_value());
  EXPECT_FALSE(inter.registry.get(&fun4).pure());

  // 5 and 6 are recursive, this analysis did not handle this case
  EXPECT_TRUE(inter.registry.get(&fun5).is_top());
  EXPECT_TRUE(inter.registry.get(&fun6).is_top());
  // 7 is unreached
  EXPECT_TRUE(inter.registry.get(&fun7).is_top());
}

TEST(AnalyzerTest, test1) { test1(); }
