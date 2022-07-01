/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <iosfwd>
#include <sstream>
#include <vector>

#include <boost/optional/optional.hpp>

class DexClass;
class DexMethod;
class IRCode;

namespace cfg {
class ControlFlowGraph;
} // namespace cfg

namespace visualizer {

template <typename T>
using optional = boost::optional<T>;

void print_compilation_header(std::ostream& os,
                              const std::string& name,
                              const std::string& method);
void print_cfg(std::ostream& os,
               cfg::ControlFlowGraph* cfg,
               const std::string& name,
               const optional<std::string>& prefix_block);

void print_ircode(std::ostream& os,
                  IRCode* code,
                  const std::string& name,
                  const optional<std::string>& prefix_block);

enum Options {
  NONE = 0,
  SKIP_NO_CHANGE = 1,
  PRINT_CODE = 2,
  FORCE_CFG = 4,
};

// A stream storage for CFG visualization. By default will not emit a pass if
// the CFG did not change.
class MethodCFGStream {
 public:
  explicit MethodCFGStream(DexMethod* m);

  void add_pass(const std::string& pass_name,
                Options o = (Options)(SKIP_NO_CHANGE | PRINT_CODE),
                const optional<std::string>& extra_prefix = boost::none);

  std::string get_output() const { return m_ss.str(); }

 private:
  DexMethod* m_method;
  std::string m_orig_name;
  std::string m_last;
  std::stringstream m_ss;
};

// A wrapper managing CFG streams of all methods in a class. Detects when
// methods are added or removed (in which case a non-cfg pass will be
// added).
class ClassCFGStream {
 private:
  struct MethodState {
    DexMethod* method;
    MethodCFGStream stream;
    bool removed;
  };

 public:
  explicit ClassCFGStream(DexClass* klass);

  void add_pass(const std::string& pass_name, Options o = SKIP_NO_CHANGE);

  void write(std::ostream& os) const;

 private:
  DexClass* m_class;
  std::vector<MethodState> m_methods;
};

class Classes {
 public:
  explicit Classes(const std::string& file_name, bool write_after_arch_pass)
      : m_file_name(file_name),
        m_write_after_each_pass(write_after_arch_pass) {}

  bool add(const std::string& class_name, bool add_initial_pass = true);
  void add(DexClass* klass, bool add_initial_pass = true);
  void add_all(const std::string& class_names);

  void add_pass(const std::string& pass_name, Options o = SKIP_NO_CHANGE);
  void add_pass(const std::function<std::string()>& pass_name_lazy,
                Options o = SKIP_NO_CHANGE);

  void write() const;

 private:
  std::vector<visualizer::ClassCFGStream> m_class_cfgs;
  std::vector<std::string> m_not_found;
  const std::string m_file_name;
  const bool m_write_after_each_pass;
};

} // namespace visualizer
