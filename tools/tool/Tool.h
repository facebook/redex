/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/program_options.hpp>
#include <map>
#include <string>
#include <vector>

#include "DexStore.h"
#include "ToolRegistry.h"

namespace po = boost::program_options;

class DexStore;
class DexClass;
using DexClasses = std::vector<DexClass*>;
using DexClassesVector = std::vector<DexClasses>;
using DexStoresVector = std::vector<DexStore>;

class Tool {
 public:
  Tool(const std::string& name, const std::string& desc, bool verbose = true)
      : m_name(name), m_desc(desc), m_verbose(verbose) {
    ToolRegistry::get().register_tool(this);
  }

  virtual ~Tool() {}

  virtual void run(const po::variables_map& options) = 0;

  virtual void add_options(po::options_description& options) const {}

  const std::string& name() const { return m_name; }
  const std::string& desc() const { return m_desc; }

 protected:
  DexStoresVector init(const std::string& system_jar_paths,
                       const std::string& apk_dir,
                       const std::string& dexen_dir,
                       bool balloon = true,
                       bool support_dex_v37 = false);

  void add_standard_options(po::options_description& options) const;

 private:
  std::string m_name;
  std::string m_desc;

 protected:
  bool m_verbose;
};
