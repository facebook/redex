/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OptData.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "OptDataDefs.h"
#include "Trace.h"

namespace {

/**
 * Returns: true when a line number is successfully found.
 * Updates: line_num.
 *
 * When insn == null, we return the first found line number.
 * When insn != null, we return the line number last encountered before
 * the given insn. The given method must have code when insn != null.
 */
bool get_line_num(DexMethod* method, IRInstruction* insn, size_t* line_num) {
  auto code = method->get_code();
  always_assert_log(!insn || insn && code,
                    "Logged method with instructions must contain code\n");
  if (code == nullptr) {
    return false;
  }
  // If a target instruction isn't specified, just get the first position.
  bool find_first_pos = insn == nullptr;
  size_t cur_line = 0;
  for (const auto& mie : *code) {
    if (mie.type == MFLOW_POSITION) {
      cur_line = mie.pos->line;
      if (find_first_pos) {
        *line_num = cur_line;
        return true;
      }
    }
    if (mie.type == MFLOW_OPCODE && mie.insn == insn) {
      // We want to get the last position found before the insn we care about.
      *line_num = cur_line;
      return true;
    }
  }
  return false;
}

} // namespace

namespace opt_metadata {

bool log_enabled(TraceModule module) {
  return OptDataMapper::get_instance().log_enabled(module);
}

void log_opt(OptReason opt, DexMethod* method, IRInstruction* insn) {
  OptDataMapper::get_instance().log_opt(opt, method, insn);
}

void log_opt(OptReason opt, DexMethod* method) {
  OptDataMapper::get_instance().log_opt(opt, method);
}

void log_opt(OptReason opt, DexClass* cls) {
  OptDataMapper::get_instance().log_opt(opt, cls);
}

InsnOptData::InsnOptData(DexMethod* method, IRInstruction* insn)
    : m_method(method), m_insn(insn) {
  m_has_line_num = get_line_num(method, insn, &m_line_num);
}

void InsnOptData::add_opt_data(OptReason opt) { m_opts.emplace_back(opt); }

MethodOptData::MethodOptData(DexMethod* method) : m_method(method) {
  m_has_line_num = get_line_num(m_method, nullptr, &m_line_num);
}

void MethodOptData::add_opt_data(OptReason opt) { m_opts.emplace_back(opt); }

std::shared_ptr<InsnOptData> MethodOptData::get_insn_opt_data(
    IRInstruction* insn) {
  auto kv_pair = m_insn_opt_map.find(insn);
  if (kv_pair == m_insn_opt_map.end()) {
    auto insn_opt_data = std::make_shared<InsnOptData>(m_method, insn);
    m_insn_opt_map.emplace(insn, insn_opt_data);
    return insn_opt_data;
  }
  return kv_pair->second;
}

ClassOptData::ClassOptData(DexClass* cls) : m_cls(cls) {
  m_package = get_package_name(cls->get_type());
  auto source_file = cls->get_source_file();
  if (source_file != nullptr) {
    m_has_srcfile = true;
    m_filename = source_file->str();
  }
}

void ClassOptData::add_opt_data(OptReason opt) { m_opts.emplace_back(opt); }

std::shared_ptr<MethodOptData> ClassOptData::get_meth_opt_data(
    DexMethod* method) {
  auto kv_pair = m_meth_opt_map.find(method);
  if (kv_pair == m_meth_opt_map.end()) {
    auto meth_opt_data = std::make_shared<MethodOptData>(method);
    m_meth_opt_map.emplace(method, meth_opt_data);
    return meth_opt_data;
  }
  return kv_pair->second;
}

std::shared_ptr<ClassOptData> OptDataMapper::get_cls_opt_data(
    DexType* cls_type) {
  auto cls = type_class(cls_type);
  auto kv_pair = m_cls_opt_map.find(cls);
  if (kv_pair == m_cls_opt_map.end()) {
    auto cls_opt_data = std::make_shared<ClassOptData>(cls);
    m_cls_opt_map.emplace(cls, cls_opt_data);
    return cls_opt_data;
  }
  return kv_pair->second;
}

bool OptDataMapper::log_enabled(TraceModule module) {
  // TODO (anwangster)
  // Add in class/method/insn filter here and call in write_opt_data, since it's
  // probably better to see what info we have first before discarding.
  return true;
}

void OptDataMapper::log_opt(OptReason opt,
                            DexMethod* method,
                            IRInstruction* insn) {
  std::lock_guard<std::mutex> guard(s_opt_log_mutex);
  auto cls_opt_data = get_cls_opt_data(method->get_class());
  auto meth_opt_data = cls_opt_data->get_meth_opt_data(method);
  auto insn_opt_data = meth_opt_data->get_insn_opt_data(insn);
  insn_opt_data->add_opt_data(opt);
}

void OptDataMapper::log_opt(OptReason opt, DexMethod* method) {
  std::lock_guard<std::mutex> guard(s_opt_log_mutex);
  auto cls_opt_data = get_cls_opt_data(method->get_class());
  auto meth_opt_data = cls_opt_data->get_meth_opt_data(method);
  meth_opt_data->add_opt_data(opt);
}

void OptDataMapper::log_opt(OptReason opt, DexClass* cls) {
  std::lock_guard<std::mutex> guard(s_opt_log_mutex);
  auto cls_opt_data = get_cls_opt_data(cls->get_type());
  cls_opt_data->add_opt_data(opt);
}

/**
 * NOTE: We'd rather construct these messages here than in pass code, so that
 *       the set of possible messages is easily accessible in a centralized
 *       location. Also we save memory by avoiding intermediary string storage.
 *       Style here is still up in the air.
 * TODO (anwangster) Maybe outsource all messages to OptDataDefs.
 */
void OptDataMapper::write_insn_opt(std::shared_ptr<InsnOptData> insn_opt_data,
                                   OptReason opt,
                                   FILE* file) {
  switch (opt) {
  case OPT_INLINED:
    static const char* s_opt_inlined = "[OPT] Inlined method\n";
    fprintf(file, "%s", s_opt_inlined);
    return;
  case OPT_CALLSITE_ARGS_REMOVED:
    static const char* s_opt_callsite_args_removed =
        "[OPT] Updated callsite args for invoking updated method\n";
    fprintf(file, "%s", s_opt_callsite_args_removed);
    return;
  default:
    always_assert_log(false, "Tried to write a non-registered insn opt\n");
  }
}

void OptDataMapper::write_meth_opt(std::shared_ptr<MethodOptData> meth_opt_data,
                                   OptReason opt,
                                   FILE* file) {
  switch (opt) {
  case OPT_METHOD_PARAMS_REMOVED:
    static const char* s_opt_method_params_removed =
        "[OPT] Removed unused params and updated method to %s\n";
    fprintf(file, s_opt_method_params_removed, SHOW(meth_opt_data->m_method));
    return;
  default:
    always_assert_log(false, "Tried to write a non-registered method opt\n");
  }
}

void OptDataMapper::write_cls_opt(std::shared_ptr<ClassOptData> cls_opt_data,
                                  OptReason opt,
                                  FILE* file) {
  switch (opt) {
  default:
    always_assert_log(false, "Tried to write a non-registered class opt\n");
  }
}

void OptDataMapper::write_opt_data(const std::string& filename) {
  auto file = fopen(filename.c_str(), "w");
  if (file == nullptr) {
    return;
  }
  // For every registered class, output all its optimizations.
  for (const auto& cls_pair : m_cls_opt_map) {
    auto cls_name = cls_pair.first->c_str();
    auto cls_opt_data = cls_pair.second;

    if (cls_opt_data->m_has_srcfile) {
      fprintf(file,
              "\n\n%s/%s: CLASS %s\n",
              cls_opt_data->m_package.c_str(),
              cls_opt_data->m_filename.c_str(),
              cls_name);
    } else {
      fprintf(file,
              "\n\n%s/: CLASS %s\n",
              cls_opt_data->m_package.c_str(),
              cls_name);
    }
    for (auto cls_opt : cls_opt_data->m_opts) {
      fprintf(file, "\t");
      write_cls_opt(cls_opt_data, cls_opt, file);
    }

    // For every registered method in the class, output all its optimizations.
    for (const auto& meth_pair : cls_opt_data->m_meth_opt_map) {
      auto meth_opt_data = meth_pair.second;
      if (meth_opt_data->m_has_line_num) {
        fprintf(file,
                "\tline %lu: METHOD %s\n",
                meth_opt_data->m_line_num,
                SHOW(meth_opt_data->m_method));
      } else {
        fprintf(file, "\tMETHOD %s\n", SHOW(meth_opt_data->m_method));
      }
      for (auto meth_opt : meth_opt_data->m_opts) {
        fprintf(file, "\t\t");
        write_meth_opt(meth_opt_data, meth_opt, file);
      }

      // For every registered insn in the method, output all its optimizations.
      for (const auto& insn_pair : meth_opt_data->m_insn_opt_map) {
        auto insn = insn_pair.first;
        auto insn_opt_data = insn_pair.second;
        if (insn_opt_data->m_has_line_num) {
          fprintf(file,
                  "\t\tline %lu: INSTRUCTION %s\n",
                  insn_opt_data->m_line_num,
                  SHOW(insn));
        } else {
          fprintf(file, "\t\tINSTRUCTION %s\n", SHOW(insn));
        }
        for (auto insn_opt : insn_opt_data->m_opts) {
          fprintf(file, "\t\t\t");
          write_insn_opt(insn_opt_data, insn_opt, file);
        }
      }
    }
  }
}

std::mutex OptDataMapper::s_opt_log_mutex;
} // namespace opt_metadata
