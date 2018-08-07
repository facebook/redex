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
bool get_line_num(const DexMethod* method,
                  const IRInstruction* insn,
                  size_t* line_num) {
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

void log_opt(OptReason opt,
             const DexMethod* method,
             const IRInstruction* insn) {
  OptDataMapper::get_instance().log_opt(opt, method, insn);
}

void log_nopt(NoptReason nopt,
              const DexMethod* method,
              const IRInstruction* insn) {
  OptDataMapper::get_instance().log_nopt(nopt, method, insn);
}

void log_opt(OptReason opt, const DexMethod* method) {
  OptDataMapper::get_instance().log_opt(opt, method);
}

void log_nopt(NoptReason nopt, const DexMethod* method) {
  OptDataMapper::get_instance().log_nopt(nopt, method);
}

void log_opt(OptReason opt, const DexClass* cls) {
  OptDataMapper::get_instance().log_opt(opt, cls);
}

void log_nopt(NoptReason nopt, const DexClass* cls) {
  OptDataMapper::get_instance().log_nopt(nopt, cls);
}

InsnOptData::InsnOptData(const DexMethod* method, const IRInstruction* insn)
    : m_method(method) {
  m_insn_orig = SHOW(insn);
  m_has_line_num = get_line_num(method, insn, &m_line_num);
}

MethodOptData::MethodOptData(const DexMethod* method) : m_method(method) {
  m_method_orig = SHOW(method);
  m_has_line_num = get_line_num(method, nullptr, &m_line_num);
}

std::shared_ptr<InsnOptData> MethodOptData::get_insn_opt_data(
    const IRInstruction* insn) {
  auto kv_pair = m_insn_opt_map.find(insn);
  if (kv_pair == m_insn_opt_map.end()) {
    auto insn_opt_data = std::make_shared<InsnOptData>(m_method, insn);
    m_insn_opt_map.emplace(insn, insn_opt_data);
    return insn_opt_data;
  }
  return kv_pair->second;
}

ClassOptData::ClassOptData(const DexClass* cls) : m_cls(cls) {
  m_package = get_package_name(cls->get_type());
  auto source_file = cls->get_source_file();
  if (source_file != nullptr) {
    m_has_srcfile = true;
    m_filename = source_file->str();
  }
}

std::shared_ptr<MethodOptData> ClassOptData::get_meth_opt_data(
    const DexMethod* method) {
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

void OptDataMapper::log_opt(OptReason opt,
                            const DexMethod* method,
                            const IRInstruction* insn) {
  std::lock_guard<std::mutex> guard(s_opt_log_mutex);
  always_assert_log(method != nullptr, "Can't log null method\n");
  always_assert_log(insn != nullptr, "Can't log null instruction\n");
  auto cls_opt_data = get_cls_opt_data(method->get_class());
  auto meth_opt_data = cls_opt_data->get_meth_opt_data(method);
  auto insn_opt_data = meth_opt_data->get_insn_opt_data(insn);
  insn_opt_data->m_opts.emplace_back(opt);
}

void OptDataMapper::log_nopt(NoptReason nopt,
                             const DexMethod* method,
                             const IRInstruction* insn) {
  std::lock_guard<std::mutex> guard(s_opt_log_mutex);
  always_assert_log(method != nullptr, "Can't log null method\n");
  always_assert_log(insn != nullptr, "Can't log null instruction\n");
  auto cls_opt_data = get_cls_opt_data(method->get_class());
  auto meth_opt_data = cls_opt_data->get_meth_opt_data(method);
  auto insn_opt_data = meth_opt_data->get_insn_opt_data(insn);
  insn_opt_data->m_nopts.emplace_back(nopt);
}

void OptDataMapper::log_opt(OptReason opt, const DexMethod* method) {
  std::lock_guard<std::mutex> guard(s_opt_log_mutex);
  always_assert_log(method != nullptr, "Can't log null method\n");
  auto cls_opt_data = get_cls_opt_data(method->get_class());
  auto meth_opt_data = cls_opt_data->get_meth_opt_data(method);
  meth_opt_data->m_opts.emplace_back(opt);
}

void OptDataMapper::log_nopt(NoptReason nopt, const DexMethod* method) {
  std::lock_guard<std::mutex> guard(s_opt_log_mutex);
  always_assert_log(method != nullptr, "Can't log null method\n");
  auto cls_opt_data = get_cls_opt_data(method->get_class());
  auto meth_opt_data = cls_opt_data->get_meth_opt_data(method);
  meth_opt_data->m_nopts.emplace_back(nopt);
}

void OptDataMapper::log_opt(OptReason opt, const DexClass* cls) {
  std::lock_guard<std::mutex> guard(s_opt_log_mutex);
  always_assert_log(cls != nullptr, "Can't log null class\n");
  auto cls_opt_data = get_cls_opt_data(cls->get_type());
  cls_opt_data->m_opts.emplace_back(opt);
}

void OptDataMapper::log_nopt(NoptReason nopt, const DexClass* cls) {
  std::lock_guard<std::mutex> guard(s_opt_log_mutex);
  always_assert_log(cls != nullptr, "Can't log null class\n");
  auto cls_opt_data = get_cls_opt_data(cls->get_type());
  cls_opt_data->m_nopts.emplace_back(nopt);
}

/**
 * TODO (anwangster)
 * Instead of a file, directly shove info into an xdb. Next diff.
 * When shoving everything into the db, we're gonna let sql take care of
 * counting opt.occurrences instead of us manually ticking up a number.
 */
void OptDataMapper::write_opt(OptReason reason, FILE* file) {
  auto header = "[OPT]";
  const auto& msg = get_opt_msg(reason);
  fprintf(file, "%s %s", header, msg.c_str());
}

void OptDataMapper::write_nopt(NoptReason reason, FILE* file) {
  auto header = "[NOPT]";
  const auto& msg = get_nopt_msg(reason);
  fprintf(file, "%s %s", header, msg.c_str());
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
      write_opt(cls_opt, file);
    }
    for (auto cls_nopt : cls_opt_data->m_nopts) {
      fprintf(file, "\t");
      write_nopt(cls_nopt, file);
    }

    // For every registered method in the class, output all its optimizations.
    for (const auto& meth_pair : cls_opt_data->m_meth_opt_map) {
      auto meth_opt_data = meth_pair.second;
      if (meth_opt_data->m_has_line_num) {
        fprintf(file,
                "\tline %lu: METHOD %s\n",
                meth_opt_data->m_line_num,
                meth_opt_data->m_method_orig.c_str());
      } else {
        fprintf(file, "\tMETHOD %s\n", meth_opt_data->m_method_orig.c_str());
      }
      for (auto meth_opt : meth_opt_data->m_opts) {
        fprintf(file, "\t\t");
        write_opt(meth_opt, file);
      }
      for (auto meth_nopt : meth_opt_data->m_nopts) {
        fprintf(file, "\t\t");
        write_nopt(meth_nopt, file);
      }

      // For every registered insn in the method, output all its optimizations.
      for (const auto& insn_pair : meth_opt_data->m_insn_opt_map) {
        auto insn_opt_data = insn_pair.second;
        if (insn_opt_data->m_has_line_num) {
          fprintf(file,
                  "\t\tline %lu: INSTRUCTION %s\n",
                  insn_opt_data->m_line_num,
                  insn_opt_data->m_insn_orig.c_str());
        } else {
          fprintf(file,
                  "\t\tINSTRUCTION %s\n", insn_opt_data->m_insn_orig.c_str());
        }
        for (auto insn_opt : insn_opt_data->m_opts) {
          fprintf(file, "\t\t\t");
          write_opt(insn_opt, file);
        }
        for (auto insn_nopt : insn_opt_data->m_nopts) {
          fprintf(file, "\t\t\t");
          write_nopt(insn_nopt, file);
        }
      }
    }
  }
}

/**
 * NOTE: We'd rather construct these messages here than in pass code, so that
 *       the set of possible messages is easily accessible in a centralized
 *       location. Also we save memory by avoiding intermediary string storage.
 *       Style here is still up in the air.
 */
void OptDataMapper::init_opt_messages() {
  std::unordered_map<int, std::string> opt_msg_map = {
      {INLINED, "Inlined method\n"},
      {CALLSITE_ARGS_REMOVED,
       "Updated callsite args for invoking updated method\n"},
      {METHOD_PARAMS_REMOVED,
       "Removed unused params and updated method signature\n"}};
  m_opt_msg_map = std::move(opt_msg_map);
}

void OptDataMapper::init_nopt_messages() {
  std::unordered_map<int, std::string> nopt_msg_map = {
      {INL_CROSS_STORE_REFS,
       "Didn't inline: callee references a DexMember in a dex store different "
       "from the caller's\n"},
      {INL_BLACKLISTED_CALLEE, "Didn't inline blacklisted method\n"},
      {INL_BLACKLISTED_CALLER, "Didn't inline into blacklisted method\n"},
      {INL_EXTERN_CATCH,
       "Didn't inline: callee has a non-public external catch type\n"},
      {INL_TOO_BIG,
       "Didn't inline: estimated inlined method size is too big\n"},
      {INL_CREATE_VMETH,
       "Didn't inline: callee contains invokes of methods not visible to the "
       "caller\n"},
      {INL_HAS_INVOKE_SUPER,
       "Didn't inline: callee has a nonrelocatable super call\n"},
      {INL_UNKNOWN_VIRTUAL,
       "Didn't inline: callee contains calls to a non-public or unknown "
       "virtual method\n"},
      {INL_UNKNOWN_FIELD,
       "Didn't inline: callee references a field unknown to the caller\n"},
      {INL_MULTIPLE_RETURNS,
       "Didn't inline: callee has multiple return points\n"},
      {INL_TOO_MANY_CALLERS,
       "Didn't inline: this method has too many callers\n"},
      {INL_2_CALLERS_TOO_BIG,
       "Didn't inline: this method has only 2 callers, but it's too big\n"},
      {INL_3_CALLERS_TOO_BIG,
       "Didn't inline: this method has only 3 callers, but it's too big\n"}};
  m_nopt_msg_map = std::move(nopt_msg_map);
}

std::string OptDataMapper::get_opt_msg(OptReason reason) {
  const auto& kv_pair = m_opt_msg_map.find(reason);
  always_assert_log(kv_pair != m_opt_msg_map.end(),
                    "Message not found for reason %s\n",
                    reason);
  return kv_pair->second;
}

std::string OptDataMapper::get_nopt_msg(NoptReason reason) {
  const auto& kv_pair = m_nopt_msg_map.find(reason);
  always_assert_log(kv_pair != m_nopt_msg_map.end(),
                    "Message not found for reason %s\n",
                    reason);
  return kv_pair->second;
}

std::mutex OptDataMapper::s_opt_log_mutex;
} // namespace opt_metadata
