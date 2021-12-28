/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Macros.h"

#if !defined(NDEBUG) && !IS_WINDOWS

#include "InteractiveDebugging.h"

#include "ControlFlow.h"
#include "DexClass.h"
#include "IRCode.h"
#include "Show.h"
#include "Trace.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "TraceContextAccess.h"

namespace {

const char* g_dump_file = "stdout";

const char* g_open_mode = "a";

void write(const char* format, ...) {
  FILE* dump_file_ptr = stdout;
  bool not_stdout = strcmp(g_dump_file, "stdout") != 0;
  if (not_stdout) {
    FILE* tmp = fopen(g_dump_file, g_open_mode);
    if (!tmp) {
      printf("%s does not exist, writing to stdout\n", g_dump_file);
    } else {
      dump_file_ptr = tmp;
    }
  }

  va_list args;
  va_start(args, format);
  vfprintf(dump_file_ptr, format, args);
  va_end(args);

  if (not_stdout) {
    fclose(dump_file_ptr);
  }
}

const DexMethod* get_current_dex_method() {
  auto* trace_context = TraceContextAccess::get_s_context();
  if (!trace_context) {
    return nullptr;
  }
  auto* dex_method_ref = trace_context->get_dex_method_ref();
  if (!dex_method_ref) {
    write("No DexMethodRef set in current TraceContext\n");
    return nullptr;
  }

  auto dex_method = dex_method_ref->as_def();
  if (!dex_method) {
    write("DexMethodRef (%s) in current TraceContext is not a DexMethod\n",
          dex_method_ref->c_str());
    return nullptr;
  }

  return dex_method;
}

const IRCode* get_current_ir_code() {
  auto dex_method = get_current_dex_method();
  if (!dex_method) {
    return nullptr;
  }

  auto code = dex_method->get_code();
  if (!code) {
    write("DexMethod (%s) has no IRCode\n", dex_method->c_str());
    return nullptr;
  }

  return code;
}

class CFGHolder {
 public:
  explicit CFGHolder(const IRCode* ir_code);
  const cfg::ControlFlowGraph& get();
  ~CFGHolder();

 private:
  IRCode* m_ir_code;
  bool m_cfg_was_built;
};

CFGHolder::CFGHolder(const IRCode* ir_code)
    : m_ir_code(const_cast<IRCode*>(ir_code)) {

  m_cfg_was_built = m_ir_code->cfg_built();
  if (!m_cfg_was_built) {
    m_ir_code->build_cfg(true);
  }
}

const cfg::ControlFlowGraph& CFGHolder::get() { return m_ir_code->cfg(); }

CFGHolder::~CFGHolder() {
  if (!m_cfg_was_built) {
    m_ir_code->clear_cfg();
  }
}

} // namespace

void dumpcfg(const cfg::ControlFlowGraph& cfg) {
  write("\n%s\n", show(cfg).c_str());
}

void dumpcfg() {
  const IRCode* ir_code = get_current_ir_code();
  if (!ir_code) {
    return;
  }

  CFGHolder cfgHolder(ir_code);
  const auto& cfg = cfgHolder.get();

  dumpcfg(cfg);
}

void dumpblock(const cfg::Block* block) {
  write("\n%s\n", show(block).c_str());
}

void dumpblock(cfg::BlockId block_id) {
  const IRCode* ir_code = get_current_ir_code();
  if (!ir_code) {
    return;
  }

  CFGHolder cfgHolder(ir_code);
  const auto& cfg = cfgHolder.get();
  const auto& blocks = cfg.blocks();
  auto it =
      std::find_if(blocks.begin(), blocks.end(),
                   [block_id](const auto* b) { return b->id() == block_id; });
  if (it == blocks.end()) {
    return;
  }
  const auto* block = *it;

  dumpblock(block);
}

void dumpir(const IRCode* ir_code) {
  if (!ir_code) {
    return;
  }

  write("\n%s\n", show(ir_code).c_str());
}

void dumpir() {
  const IRCode* ir_code = get_current_ir_code();

  dumpir(ir_code);
}

void setdumpfile(const char* file_name) {
  g_dump_file = strdup(file_name);
  printf("Set dump file to %s\n", g_dump_file);
}

void setdumpfilemode(const char* mode) {
  if (strcmp(mode, "append") == 0 || strcmp(mode, "a") == 0) {
    g_open_mode = "a";
  } else if (strcmp(mode, "truncate") == 0 || strcmp(mode, "w") == 0) {
    g_open_mode = "w";
  } else {
    printf("Error setting dump file mode: argument, \"%s\", unrecognized\n",
           mode);
  }
}

const char* methname() {
  auto dex_method = get_current_dex_method();
  if (!dex_method) {
    return "";
  }

  return strdup(show(dex_method).c_str());
}

void dumpmethname() { write("%s\n", methname()); }
#endif
