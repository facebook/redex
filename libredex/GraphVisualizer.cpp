/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "GraphVisualizer.h"

#include <fstream>
#include <iostream>

#include <boost/algorithm/string.hpp>
#include <boost/range/adaptors.hpp>

#include "ControlFlow.h"
#include "Debug.h"
#include "IRCode.h"

// The "Hotspot Client Compiler Visualizer" (c1visualizer) is a tool consuming
// Hotspot C1 compiler debug info to display control flow graphs of compilation
// passes. The format is not well-specified, the best definition is the parser
// itself, which can be found here:
//
//   https://github.com/zakkak/c1visualizer/blob/master/Compilation%20Model/src/at/ssw/visualizer/parser/CompilerOutput.atg
//
// as well as accompanying code on how to parse some textual data.
//
// A cfg file contains a set of compilations which are denoted by a compilation
// header ("begin_compilation" to "end_compilation") and associated CFGs
// ("begin_cfg" to "end_cfg"). CFGs are made up of connected blocks that contain
// different forms of supported representation (HIR, LR, IR, bytecode; we only
// use HIR at this point).
//
// A (shortened) sample file may look like this:
//
//  begin_compilation
//    name "static void foo.bar()"
//    method "static void foo.bar()"
//    date 1576632329
//  end_compilation
//  begin_cfg
//    name "Initial"
//    begin_block
//      name "B18446744073709551615"
//      from_bci -1
//      to_bci -1
//      predecessors
//      successors "B0"
//      xhandlers
//      flags
//      begin_states
//        begin_locals
//          size 0
//          method "none"
//        end_locals
//      end_states
//      begin_HIR
//        0 0 info0 INFO data:static void foo.bar() <|@
//      end_HIR
//    end_block
//    begin_block
//      name "B0"
//      from_bci -1
//      to_bci -1
//      predecessors
//      successors
//      xhandlers
//      flags
//      begin_states
//        [...]
//      end_states
//      begin_HIR
//        0 0 i0 CONST [v0] literal:0 <|@
//        [...]
//        0 0 i9 RETURN_VOID <|@
//      end_HIR
//    end_block
//  end_cfg
//  begin_cfg
//    name "First pass"
//    [...]

namespace visualizer {

using namespace cfg;

namespace {

const char* method_item_type_str(MethodItemType t) {
  switch (t) {
  case MFLOW_TRY:
    return "try";
  case MFLOW_CATCH:
    return "catch";
  case MFLOW_OPCODE:
    return "opcode";
  case MFLOW_DEX_OPCODE:
    return "dex-opcode";
  case MFLOW_TARGET:
    return "target";
  case MFLOW_DEBUG:
    return "debug";
  case MFLOW_POSITION:
    return "position";
  case MFLOW_FALLTHROUGH:
    return "fallthrough";
  }
  not_reached();
}

// A "container" that helps with formatting list. Use as a helper instance, add
// elements to the stream given by next(), and create the final format with the
// supplied operator<<.
class List {
 public:
  explicit List() : m_empty(true) {}

  std::ostream& next() {
    if (m_empty) {
      m_empty = false;
    } else {
      m_stream << ",";
    }
    return m_stream;
  }

 private:
  bool m_empty;
  std::ostringstream m_stream;

  friend std::ostream& operator<<(std::ostream& os, const List& list);
};

std::ostream& operator<<(std::ostream& os, const List& list) {
  os << '[' << list.m_stream.str() << ']';
  return os;
}

// Base class for "tagged" element formatting. Knows how to format blocks (and
// provides an RAII wrapper) as well as some of the textual format (plain
// values that may be quoted, attributes that are key-value pairs).
class TaggedBase {
 public:
  explicit TaggedBase(std::ostream& o) : m_output(o), m_indent(0) {}

  void start_tag(const char* name) {
    indent();
    m_output << "begin_" << name << std::endl;
    m_indent++;
  }

  void end_tag(const char* name) {
    m_indent--;
    indent();
    m_output << "end_" << name << std::endl;
  }

  struct TagRAII {
    explicit TagRAII(TaggedBase* p, const char* tag) : p(p), tag(tag) {
      p->start_tag(tag);
    }
    ~TagRAII() { p->end_tag(tag); }
    TaggedBase* p;
    const char* tag;
  };

  void indent() {
    for (size_t i = 0; i < m_indent; ++i) {
      m_output << "  ";
    }
  }

  // QuotedEnum and ValueStream are helpers for printing (complex) values.
  // ValueStream will take care of quotation, printing the right string on
  // _destruction, so ensure that a ValueStream does not exist longer than
  // necessary, i.e., best-practice is to never keep one explicitly.
  enum QuotedEnum {
    QUOTED,
    NOT_QUOTED,
  };
  template <QuotedEnum quoted>
  struct ValueStream final {
    std::ostringstream oss;
    std::ostream* trg = nullptr;
    bool disposed = false;

    explicit ValueStream(std::ostream* trg) : trg(trg) {}
    // Only exists for resolution. Will fail when used.
    ValueStream(const ValueStream&) : disposed(true) { not_reached(); }
    ValueStream(ValueStream&& rhs) noexcept
        : oss(std::move(rhs.oss)), trg(rhs.trg) {
      rhs.disposed = true;
    }

    ~ValueStream() {
      if (disposed) {
        return;
      }
      auto tmp = oss.str();
      if (!tmp.empty()) {
        bool is_quoted = quoted == QUOTED;
        *trg << (is_quoted ? "\"" : "") << tmp << (is_quoted ? "\"" : "");
      }
      *trg << std::endl;
    }

    // See copy constructor. Assignment operators for lint.
    ValueStream& operator=(const ValueStream&) { not_reached(); }
    ValueStream& operator=(ValueStream&& rhs) {
      oss(std::move(rhs.oss));
      trg(rhs.trg);
      rhs.disposed = true;
      return *this;
    }

    template <typename T>
    ValueStream& operator<<(const T& val) {
      oss << val;
      return *this;
    }
  };

  template <QuotedEnum quoted = NOT_QUOTED>
  ValueStream<quoted> value(const std::string& name) {
    indent();
    m_output << name << ' ';
    return ValueStream<quoted>(&m_output);
  }

  std::ostream& attribute() {
    m_output << " ";
    return m_output;
  }
  std::ostream& attribute(const std::string& attr) {
    m_output << " ";
    always_assert_log(!attr.empty(), "Attribute must be non-empty");
    always_assert_log(attr.find(" ") == std::string::npos,
                      "Attribute must not have spaces");
    return m_output << attr << ":";
  }

 protected:
  std::ostream& m_output;
  size_t m_indent;
};

// Base printer that can format c1visualizer blocks filled with Redex elements
// as HIR. Generic to allow both ControlFlowGraph as well as IRList input in
// specialized implementations.
template <typename T>
class CodeVisualizer : public TaggedBase {
 public:
  explicit CodeVisualizer(std::ostream& output) : TaggedBase(output) {}
  virtual ~CodeVisualizer() {}

  static void dex_string(std::ostream& os, const DexString* s) {
    os << (s ? "<null>" : s->str());
  }

  void instruction(IRInstruction* insn) {
    m_output << SHOW(insn->opcode());

    if (insn->srcs_size() || insn->has_dest()) {
      List input_list;
      if (insn->has_dest()) {
        input_list.next() << "v" << insn->dest();
      }
      for (reg_t src : insn->srcs()) {
        input_list.next() << "v" << src;
      }
      attribute() << input_list;
    }

    if (insn->has_method()) {
      attribute("method_name") << show(insn->get_method());
    }
    if (insn->has_field()) {
      attribute("field_name") << show(insn->get_field());
    }
    if (insn->has_type()) {
      attribute("type") << show(insn->get_type());
    }
    if (insn->has_literal()) {
      attribute("literal") << insn->get_literal();
    }
    if (insn->has_string()) {
      dex_string(attribute("string"), insn->get_string());
    }
  }

  void mie_position(const MethodItemEntry& mie) {
    m_output << method_item_type_str(mie.type);
    if (mie.pos) {
      m_output << " \"";
      DexPosition& pos = *mie.pos;
      if (pos.method != nullptr) {
        m_output << pos.method->str();
      } else {
        m_output << "<unnamed-method>";
      }
      m_output << "(";
      if (pos.file != nullptr) {
        m_output << pos.file->str() << ":" << pos.line;
      } else {
        m_output << "<no-file>";
      }
      m_output << ")\"";
    }
  }

  template <typename... Args>
  void mie(const MethodItemEntry& mie, Args... args) {
    switch (mie.type) {
    case MFLOW_TRY:
    case MFLOW_CATCH:
    case MFLOW_DEX_OPCODE:
    case MFLOW_TARGET:
    case MFLOW_DEBUG:
    case MFLOW_FALLTHROUGH:
      m_output << method_item_type_str(mie.type);
      return;

    case MFLOW_POSITION:
      static_cast<T*>(this)->mie_position(mie);
      return;

    case MFLOW_OPCODE:
      static_cast<T*>(this)->instruction(mie.insn, args...);
      return;
    }
  }

  // bci (bytecode index) & num_uses are required by the format.
  void mie_prefix(size_t bci, size_t num_uses) {
    indent();
    m_output << bci << " " << num_uses;
  }
  void mie_prefix(size_t bci, size_t num_uses, size_t insn_id) {
    mie_prefix(bci, num_uses);
    m_output << " i" << insn_id << " ";
  }

  void mie_suffix() { m_output << " <|@" << std::endl; }

  template <typename C, typename... Other>
  void mie_list(C* b, Other... other) {
    for (auto& m : *b) {
      // Using 0 for bci and num_uses as we don't have/need this.
      mie_prefix(0, 0, static_cast<T*>(this)->mie_id(m));
      mie(m, b, other...);
      mie_suffix();
    }
  }

  template <typename C, typename IdT, typename HIRFn>
  void block(C* block, IdT id, bool exc, HIRFn hir) {
    TagRAII block_tag(this, "block");
    value<QUOTED>("name") << "B" << id;
    value("from_bci") << -1;
    value("to_bci") << -1;
    static_cast<T*>(this)->predecessors(block);
    static_cast<T*>(this)->successors(block);
    static_cast<T*>(this)->exception_handlers(block);

    value<QUOTED>("flags") << (exc ? "catch_block" : "");

    {
      // TODO: Is this really necessary? If so, fill in correctly.
      TagRAII states_tag(this, "states");
      TagRAII locals_tag(this, "locals");
      value("size") << 0;
      value<QUOTED>("method") << "none";
    }

    {
      TagRAII hir_tag(this, "HIR");
      hir(block);
    }
  }
};

using namespace boost::adaptors;

class CFGVisualizer : public CodeVisualizer<CFGVisualizer> {
 public:
  CFGVisualizer(ControlFlowGraph* cfg, std::ostream& output)
      : CodeVisualizer(output), m_cfg(cfg) {
    if (m_cfg) {
      prepare();
    }
  }
  virtual ~CFGVisualizer() {}

  template <typename T>
  void block_list(const std::string& name, const T& blocks) {
    indent();
    m_output << name;
    for (auto b : blocks) {
      m_output << " \"B" << b->id() << "\" ";
    }
    m_output << std::endl;
  }

  void predecessors(Block* block) {
    block_list("predecessors",
               transform(block->preds(), [](Edge* e) { return e->src(); }));
  }

  static bool is_throw_edge(const Edge* e) { return e->type() == EDGE_THROW; }
  static bool is_not_throw_edge(const Edge* e) { return !is_throw_edge(e); }

  template <typename T>
  std::vector<Edge*> get_succ_edges(Block* block, T& fn) {
    return m_cfg ? m_cfg->get_succ_edges_if(block, fn) : std::vector<Edge*>{};
  }

  void successors(Block* block) {
    block_list("successors",
               transform(get_succ_edges(block, is_not_throw_edge),
                         [](Edge* e) { return e->target(); }));
  }

  void exception_handlers(Block* block) {
    block_list("xhandlers",
               transform(get_succ_edges(block, is_throw_edge),
                         [](Edge* e) { return e->target(); }));
  }

  void instruction(IRInstruction* insn, Block* from) {
    CodeVisualizer::instruction(insn);

    if (is_conditional_branch(insn->opcode())) {
      auto edge = m_cfg->get_succ_edge_if(
          from, [](Edge* e) { return e->type() == EDGE_BRANCH; });
      redex_assert(edge);
      attribute("target") << "B" << edge->target()->id();
    }
  }

  size_t mie_id(const MethodItemEntry& mie) const {
    return m_mie_id_map.at(&mie);
  }

  void prefix_block(Block* b, const std::string& prefix) {
    auto fake_insn = [&](Block*) {
      mie_prefix(0, 0);
      m_output << " info0 INFO";
      attribute("data") << prefix;
      mie_suffix();
    };
    CodeVisualizer::block(b, b->id(), false, fake_insn);
  }

  void prepare() {
    size_t index = 0;
    for (auto block : m_cfg->blocks()) {
      for (auto& mie : *block) {
        m_mie_id_map[&mie] = index;
        if (mie.type == MFLOW_OPCODE) {
          m_insn_id_map[mie.insn] = index;
        }
        ++index;
      }
      for (auto edge : m_cfg->get_succ_edges_if(block, is_throw_edge)) {
        m_exc_blocks.insert(edge->target());
      }
      if (m_cfg->get_pred_edge_if(block, is_throw_edge)) {
        m_exc_blocks.insert(block);
      }
    }
  }

  void cfg(const std::string& name, const optional<std::string>& prefix) {
    {
      TagRAII cfg_tag(this, "cfg");
      value<QUOTED>("name") << name;
      if (prefix) {
        Block fake_block(m_cfg, std::numeric_limits<BlockId>::max());
        auto first_real = (!m_cfg || m_cfg->blocks().empty())
                              ? nullptr
                              : *m_cfg->blocks().begin();
        Edge fake_edge(&fake_block, first_real ? first_real : &fake_block,
                       EDGE_GOTO);
        const_cast<std::vector<Edge*>&>(fake_block.succs())
            .push_back(&fake_edge);
        prefix_block(&fake_block, *prefix);
      }
      if (m_cfg) {
        for (auto b : m_cfg->blocks()) {
          CodeVisualizer::block(b, b->id(), m_exc_blocks.count(b),
                                [&](Block*) { mie_list(b); });
        }
      }
    }
  }

 private:
  ControlFlowGraph* m_cfg;
  std::unordered_map<const MethodItemEntry*, size_t> m_mie_id_map;
  std::unordered_map<const IRInstruction*, size_t> m_insn_id_map;
  std::unordered_set<Block*> m_exc_blocks;
};

class IRCodeVisualizer : public CodeVisualizer<IRCodeVisualizer> {
 public:
  IRCodeVisualizer(IRCode* code, std::ostream& output)
      : CodeVisualizer(output), m_code(code) {
    if (m_code) {
      prepare();
    }
  }
  virtual ~IRCodeVisualizer() {}

  void empty_block_list(const std::string& name) {
    indent();
    m_output << name;
    m_output << std::endl;
  }

  void block_list(const std::string& name, size_t succ_id) {
    indent();
    m_output << name;
    m_output << " \"B" << succ_id << "\" ";
    m_output << std::endl;
  }

  void predecessors(IRCode*) { empty_block_list("predecessors"); }
  void successors(IRCode*) { empty_block_list("successors"); }
  void exception_handlers(IRCode*) { empty_block_list("xhandlers"); }

  void predecessors(const std::string*) { empty_block_list("predecessors"); }
  void successors(const std::string*) { block_list("successors", 0); }
  void exception_handlers(const std::string*) { empty_block_list("xhandlers"); }

  void instruction(IRInstruction* insn, IRCode*) {
    CodeVisualizer::instruction(insn);
  }

  size_t mie_id(const MethodItemEntry& mie) const {
    return m_mie_id_map.at(&mie);
  }

  void prefix_block(const std::string& prefix) {
    auto fake_insn = [&](const std::string*) {
      mie_prefix(0, 0);
      m_output << " info0 INFO";
      attribute("data") << prefix;
      mie_suffix();
    };
    CodeVisualizer::block(&prefix, (size_t)(-1), false, fake_insn);
  }

  void prepare() {
    size_t index = 0;
    for (auto& mie : *m_code) {
      m_mie_id_map[&mie] = index++;
    }
  }

  void code(const std::string& name, const optional<std::string>& prefix) {
    {
      TagRAII cfg_tag(this, "cfg");
      value<QUOTED>("name") << name;
      if (prefix) {
        prefix_block(*prefix);
      }
      if (m_code) {
        CodeVisualizer::block(m_code, 0, false,
                              [&](IRCode*) { mie_list(m_code); });
      }
    }
  }

 private:
  IRCode* m_code;
  std::unordered_map<const MethodItemEntry*, size_t> m_mie_id_map;
};

} // namespace

void print_compilation_header(std::ostream& os,
                              const std::string& name,
                              const std::string& method) {
  TaggedBase printer(os);
  TaggedBase::TagRAII compilation_tag(&printer, "compilation");
  printer.value<TaggedBase::QUOTED>("name") << name;
  printer.value<TaggedBase::QUOTED>("method") << method;
  printer.value("date") << time(nullptr);
}

void print_cfg(std::ostream& os,
               ControlFlowGraph* cfg,
               const std::string& name,
               const optional<std::string>& prefix_block) {
  CFGVisualizer visualizer(cfg, os);
  visualizer.cfg(name, prefix_block);
}

void print_ircode(std::ostream& os,
                  IRCode* code,
                  const std::string& name,
                  const optional<std::string>& prefix_block) {
  if (code && code->cfg_built()) {
    print_cfg(os, &code->cfg(), name, prefix_block);
    return;
  }
  IRCodeVisualizer visualizer(code, os);
  visualizer.code(name, prefix_block);
}

namespace {

// A fake name to allow dedupe.
static constexpr const char* FAKE_PASS_NAME = "FAKE_PASS_NAME_FOR_DEDUPE";

std::vector<DexMethod*> get_all_methods(DexClass* klass) {
  std::vector<DexMethod*> all_methods;
  all_methods.insert(all_methods.end(), klass->get_dmethods().begin(),
                     klass->get_dmethods().end());
  all_methods.insert(all_methods.end(), klass->get_vmethods().begin(),
                     klass->get_vmethods().end());
  return all_methods;
}

} // namespace

// A stream storage for CFG visualization. On request, will not emit a pass if
// the CFG did not change.
MethodCFGStream::MethodCFGStream(DexMethod* m) : m_method(m) {
  m_orig_name = vshow(m, false);
  print_compilation_header(m_ss, m_orig_name.c_str(), m_orig_name.c_str());
}

void MethodCFGStream::add_pass(const std::string& pass_name,
                               Options o,
                               const optional<std::string>& extra_prefix) {
  auto cur_name = vshow(m_method, false);
  auto code = m_method->get_code();
  if (!code) {
    cur_name += " (NO CODE)";
  } else if (!(o & PRINT_CODE)) {
    code = nullptr;
  }
  if (extra_prefix) {
    cur_name = *extra_prefix + cur_name;
  }

  std::stringstream tmp;
  if ((o & FORCE_CFG) && code) {
    bool built_cfg = false;
    if (code && !code->cfg_built()) {
      built_cfg = true;
      code->build_cfg();
    }
    print_cfg(tmp, &code->cfg(), FAKE_PASS_NAME, cur_name);
    if (built_cfg) {
      code->clear_cfg();
    }
  } else {
    print_ircode(tmp, code, FAKE_PASS_NAME, cur_name);
  }

  std::string new_pass = tmp.str();
  if (new_pass != m_last || !(o & SKIP_NO_CHANGE)) {
    m_last = new_pass;

    // Replace pass name.
    auto pos = new_pass.find(FAKE_PASS_NAME);
    redex_assert(pos != std::string::npos);
    new_pass.replace(pos, strlen(FAKE_PASS_NAME), pass_name);

    m_ss << new_pass;
  }
}

ClassCFGStream::ClassCFGStream(DexClass* klass) : m_class(klass) {
  for (auto* method : get_all_methods(klass)) {
    m_methods.push_back(MethodState{method, MethodCFGStream(method), false});
  }
}

void ClassCFGStream::add_pass(const std::string& pass_name, Options o) {
  auto all_methods = get_all_methods(m_class);
  for (auto& m : m_methods) {
    auto it = std::find(all_methods.begin(), all_methods.end(), m.method);
    if (it == all_methods.end()) {
      m.removed = true;
    } else {
      all_methods.erase(it);
    }
  }
  for (auto* method : all_methods) {
    m_methods.push_back(MethodState{method, MethodCFGStream(method), false});
  }

  for (auto& m : m_methods) {
    m.stream.add_pass(pass_name,
                      (Options)(o | (!m.removed ? Options::PRINT_CODE : 0)),
                      m.removed ? optional<std::string>("REMOVED ")
                                : boost::none);
  }
}

void ClassCFGStream::write(std::ostream& os) const {
  for (auto& m : m_methods) {
    os << m.stream.get_output();
  }
}

void Classes::add_all(const std::string& class_names) {
  if (class_names.empty()) {
    return;
  }
  std::vector<std::string> classes;
  boost::algorithm::split(classes, class_names,
                          [](char c) { return c == ';'; });
  for (const auto& c : classes) {
    if (c.empty()) {
      continue;
    }
    auto complete = c + ';';
    if (!add(complete)) {
      std::cerr << "Did not find class " << complete;
    }
  }
}

bool Classes::add(const std::string& class_name) {
  auto type = DexType::make_type(class_name.c_str());
  auto klass = type_class(type);
  if (!klass) {
    return false;
  }
  add(klass);
  return true;
}

void Classes::add(DexClass* klass) {
  m_class_cfgs.emplace_back(klass);
  m_class_cfgs.back().add_pass("Initial");
}

void Classes::add_pass(const std::string& pass_name, Options o) {
  for (auto& class_cfg : m_class_cfgs) {
    class_cfg.add_pass(pass_name, o);
  }
  if (m_write_after_each_pass) {
    write();
  }
}

void Classes::write() const {
  if (m_class_cfgs.empty()) {
    return;
  }
  std::ofstream os(m_file_name);
  for (const auto& c : m_class_cfgs) {
    c.write(os);
  }
}

} // namespace visualizer
