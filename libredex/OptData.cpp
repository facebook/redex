/*
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
#include <json/value.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "DexClass.h"
#include "DexPosition.h"
#include "DexUtil.h"
#include "EditableCfgAdapter.h"
#include "IRCode.h"
#include "OptDataDefs.h"
#include "Resolver.h"
#include "Show.h"
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
  const IRCode* code = method->get_code();
  always_assert_log(!insn || (insn && code),
                    "Logged method with instructions must contain code\n");
  if (code == nullptr) {
    return false;
  }
  // If a target instruction isn't specified, just get the first position.
  bool find_first_pos = insn == nullptr;
  size_t cur_line = 0;
  bool success{false};
  editable_cfg_adapter::iterate_all(code, [&](const MethodItemEntry& mie) {
    if (mie.type == MFLOW_POSITION) {
      cur_line = mie.pos->line;
      if (find_first_pos) {
        *line_num = cur_line;
        success = true;
        return editable_cfg_adapter::LOOP_BREAK;
      }
    }
    if (mie.type == MFLOW_OPCODE && mie.insn == insn) {
      // We want to get the last position found before the insn we care about.
      *line_num = cur_line;
      success = true;
      return editable_cfg_adapter::LOOP_BREAK;
    }
    return editable_cfg_adapter::LOOP_CONTINUE;
  });
  return success;
}

/**
 * Strips out package info and the end semicolon from the deobfuscated class
 * name and returns the result.
 * "some/package/class_name;" -> "class_name"
 */
std::string get_deobfuscated_name_substr(const DexClass* cls) {
  auto name = cls->get_deobfuscated_name();
  if (name.empty()) {
    name = SHOW(cls);
  }
  size_t pos_base = name.rfind('/');
  size_t pos_end = name.rfind(';');
  return name.substr(pos_base + 1, pos_end - pos_base - 1);
}

/**
 * Returns the deobfuscated name for the given method.
 */
std::string get_deobfuscated_name(const DexMethod* method) {
  auto name = method->get_deobfuscated_name();
  if (name.empty()) {
    name = SHOW(method);
  }
  always_assert_log(!name.empty(), "A method is always named\n");
  return name;
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

InsnOptData::InsnOptData(const DexMethod* method, const IRInstruction* insn) {
  m_insn_orig = SHOW(insn);
  m_has_line_num = get_line_num(method, insn, &m_line_num);
}

MethodOptData::MethodOptData(const DexMethod* method) : m_method(method) {
  m_method_orig = SHOW(method);
  m_has_line_num = get_line_num(method, nullptr, &m_line_num);
}

std::shared_ptr<InsnOptData> MethodOptData::get_insn_opt_data(
    const IRInstruction* insn) {
  const auto& kv_pair = m_insn_opt_map.find(insn);
  if (kv_pair == m_insn_opt_map.end()) {
    auto insn_opt_data = std::make_shared<InsnOptData>(m_method, insn);
    m_insn_opt_map.emplace(insn, insn_opt_data);
    return insn_opt_data;
  }
  return kv_pair->second;
}

ClassOptData::ClassOptData(const DexClass* cls) : m_cls(cls) {
  m_package = type::get_package_name(cls->get_type());
  auto source_file = cls->get_source_file();
  if (source_file != nullptr) {
    m_has_srcfile = true;
    m_filename = source_file->str();
  }
}

std::shared_ptr<MethodOptData> ClassOptData::get_meth_opt_data(
    const DexMethod* method) {
  const auto& kv_pair = m_meth_opt_map.find(method);
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
  const auto& kv_pair = m_cls_opt_map.find(cls);
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
  if (!m_logs_enabled) {
    return;
  }
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
  if (!m_logs_enabled) {
    return;
  }
  std::lock_guard<std::mutex> guard(s_opt_log_mutex);
  always_assert_log(method != nullptr, "Can't log null method\n");
  always_assert_log(insn != nullptr, "Can't log null instruction\n");
  auto cls_opt_data = get_cls_opt_data(method->get_class());
  auto meth_opt_data = cls_opt_data->get_meth_opt_data(method);
  auto insn_opt_data = meth_opt_data->get_insn_opt_data(insn);
  insn_opt_data->m_nopts.emplace_back(nopt);
}

void OptDataMapper::log_opt(OptReason opt, const DexMethod* method) {
  if (!m_logs_enabled) {
    return;
  }
  std::lock_guard<std::mutex> guard(s_opt_log_mutex);
  always_assert_log(method != nullptr, "Can't log null method\n");
  auto cls_opt_data = get_cls_opt_data(method->get_class());
  auto meth_opt_data = cls_opt_data->get_meth_opt_data(method);
  meth_opt_data->m_opts.emplace_back(opt);
}

void OptDataMapper::log_nopt(NoptReason nopt, const DexMethod* method) {
  if (!m_logs_enabled) {
    return;
  }
  std::lock_guard<std::mutex> guard(s_opt_log_mutex);
  always_assert_log(method != nullptr, "Can't log null method\n");
  auto cls_opt_data = get_cls_opt_data(method->get_class());
  auto meth_opt_data = cls_opt_data->get_meth_opt_data(method);
  meth_opt_data->m_nopts.emplace_back(nopt);
}

void OptDataMapper::log_opt(OptReason opt, const DexClass* cls) {
  if (!m_logs_enabled) {
    return;
  }
  std::lock_guard<std::mutex> guard(s_opt_log_mutex);
  always_assert_log(cls != nullptr, "Can't log null class\n");
  auto cls_opt_data = get_cls_opt_data(cls->get_type());
  cls_opt_data->m_opts.emplace_back(opt);
}

void OptDataMapper::log_nopt(NoptReason nopt, const DexClass* cls) {
  if (!m_logs_enabled) {
    return;
  }
  std::lock_guard<std::mutex> guard(s_opt_log_mutex);
  always_assert_log(cls != nullptr, "Can't log null class\n");
  auto cls_opt_data = get_cls_opt_data(cls->get_type());
  cls_opt_data->m_nopts.emplace_back(nopt);
}

Json::Value OptDataMapper::serialize_sql() {
  constexpr const char* CLASS_OPTS = "class_opts";
  constexpr const char* METHOD_OPTS = "method_opts";
  constexpr const char* INSTRUCTION_OPTS = "instruction_opts";
  constexpr const char* CLASS_NOPTS = "class_nopts";
  constexpr const char* METHOD_NOPTS = "method_nopts";
  constexpr const char* INSTRUCTION_NOPTS = "instruction_nopts";
  constexpr const char* INSTRUCTIONS = "instructions";
  constexpr const char* METHODS = "methods";
  constexpr const char* CLASSES = "classes";
  constexpr const char* OPT_MESSAGES = "opt_messages";
  constexpr const char* NOPT_MESSAGES = "nopt_messages";
  Json::Value top;

  Json::Value opt_msg_arr;
  Json::Value nopt_msg_arr;
  serialize_messages_helper(m_opt_msg_map, &opt_msg_arr);
  serialize_messages_helper(m_nopt_msg_map, &nopt_msg_arr);
  top[OPT_MESSAGES] = opt_msg_arr;
  top[NOPT_MESSAGES] = nopt_msg_arr;

  size_t cls_id{0};
  size_t meth_id{0};
  size_t insn_id{0};
  Json::Value cls_arr;
  Json::Value cls_opt_arr;
  Json::Value cls_nopt_arr;
  Json::Value meth_arr;
  Json::Value meth_opt_arr;
  Json::Value meth_nopt_arr;
  Json::Value insn_arr;
  Json::Value insn_opt_arr;
  Json::Value insn_nopt_arr;

  for (const auto& cls_pair : m_cls_opt_map) {
    auto cls_opt_data = cls_pair.second;
    serialize_class(cls_opt_data, cls_id, &cls_arr, &cls_opt_arr,
                    &cls_nopt_arr);

    for (const auto& meth_pair : cls_opt_data->m_meth_opt_map) {
      auto meth_opt_data = meth_pair.second;
      serialize_method(meth_opt_data, cls_id, meth_id, &meth_arr, &meth_opt_arr,
                       &meth_nopt_arr);

      for (const auto& insn_pair : meth_opt_data->m_insn_opt_map) {
        auto insn_opt_data = insn_pair.second;
        serialize_insn(insn_opt_data, meth_id, insn_id, &insn_arr,
                       &insn_opt_arr, &insn_nopt_arr);
        insn_id++;
      }
      meth_id++;
    }
    cls_id++;
  }

  top[CLASSES] = cls_arr;
  top[METHODS] = meth_arr;
  top[INSTRUCTIONS] = insn_arr;
  top[INSTRUCTION_OPTS] = insn_opt_arr;
  top[METHOD_OPTS] = meth_opt_arr;
  top[CLASS_OPTS] = cls_opt_arr;
  top[INSTRUCTION_NOPTS] = insn_nopt_arr;
  top[METHOD_NOPTS] = meth_nopt_arr;
  top[CLASS_NOPTS] = cls_nopt_arr;
  return top;
}

void OptDataMapper::serialize_messages_helper(
    const std::unordered_map<int, std::string>& msg_map, Json::Value* arr) {
  for (const auto& reason_msg_pair : msg_map) {
    auto reason = reason_msg_pair.first;
    const auto& message = reason_msg_pair.second;
    Json::Value msg_pair;
    msg_pair["reason_code"] = reason;
    msg_pair["message"] = message;
    arr->append(msg_pair);
  }
}

void OptDataMapper::serialize_opt_nopt_helper(
    const std::vector<OptReason>& opts,
    const std::vector<NoptReason>& nopts,
    size_t id,
    Json::Value* opt_arr,
    Json::Value* nopt_arr) {
  for (size_t i = 0; i < opts.size(); ++i) {
    verify_opt(opts.at(i));
    Json::Value opt_data;
    opt_data["reason_idx"] = (uint32_t)i;
    opt_data["id"] = (uint32_t)id;
    opt_data["reason_code"] = opts.at(i);
    opt_arr->append(opt_data);
  }
  for (size_t i = 0; i < nopts.size(); ++i) {
    verify_nopt(nopts.at(i));
    Json::Value nopt_data;
    nopt_data["reason_idx"] = (uint32_t)i;
    nopt_data["id"] = (uint32_t)id;
    nopt_data["reason_code"] = nopts.at(i);
    nopt_arr->append(nopt_data);
  }
}

void OptDataMapper::serialize_class(
    const std::shared_ptr<ClassOptData>& cls_opt_data,
    size_t cls_id,
    Json::Value* arr,
    Json::Value* opt_arr,
    Json::Value* nopt_arr) {
  const auto& name = get_deobfuscated_name_substr(cls_opt_data->m_cls);
  Json::Value cls_data;
  cls_data["id"] = (uint32_t)cls_id;
  cls_data["package"] = cls_opt_data->m_package;
  cls_data["source_file"] =
      cls_opt_data->m_has_srcfile ? cls_opt_data->m_filename : "";
  cls_data["name"] = name.c_str();
  arr->append(cls_data);
  serialize_opt_nopt_helper(cls_opt_data->m_opts, cls_opt_data->m_nopts, cls_id,
                            opt_arr, nopt_arr);
}

void OptDataMapper::serialize_method(
    const std::shared_ptr<MethodOptData>& meth_opt_data,
    size_t cls_id,
    size_t meth_id,
    Json::Value* arr,
    Json::Value* opt_arr,
    Json::Value* nopt_arr) {
  const auto& method = meth_opt_data->m_method;
  Json::Value meth_data;
  meth_data["id"] = (uint32_t)meth_id;
  meth_data["cls_id"] = (uint32_t)cls_id;
  meth_data["has_line_num"] = meth_opt_data->m_has_line_num ? 1 : 0;
  meth_data["line_num"] = (uint32_t)meth_opt_data->m_line_num;
  meth_data["signature"] = get_deobfuscated_name(method);
  meth_data["code_size"] = (uint32_t)(
      method->get_code() ? method->get_code()->sum_opcode_sizes() : 0);
  arr->append(meth_data);
  serialize_opt_nopt_helper(meth_opt_data->m_opts, meth_opt_data->m_nopts,
                            meth_id, opt_arr, nopt_arr);
}

void OptDataMapper::serialize_insn(
    const std::shared_ptr<InsnOptData>& insn_opt_data,
    size_t meth_id,
    size_t insn_id,
    Json::Value* arr,
    Json::Value* opt_arr,
    Json::Value* nopt_arr) {
  Json::Value insn_data;
  insn_data["id"] = (uint32_t)insn_id;
  insn_data["meth_id"] = (uint32_t)meth_id;
  insn_data["has_line_num"] = insn_opt_data->m_has_line_num ? 1 : 0;
  insn_data["line_num"] = (uint32_t)insn_opt_data->m_line_num;
  insn_data["instruction"] = insn_opt_data->m_insn_orig;

  // TODO In case of invokes, we want to show the deobfuscated name for clarity,
  // if possible.
  arr->append(insn_data);
  serialize_opt_nopt_helper(insn_opt_data->m_opts, insn_opt_data->m_nopts,
                            insn_id, opt_arr, nopt_arr);
}

/**
 * NOTE: Double up on single quotes for escaping in sql strings.
 */
void OptDataMapper::init_opt_messages() {
  std::unordered_map<int, std::string> opt_msg_map = {
      {INLINED, "Inlined method"},
      {CALLSITE_ARGS_REMOVED,
       "Updated callsite args for invoking updated method"},
      {METHOD_PARAMS_REMOVED,
       "Removed unused params and updated method signature"},
      {ENUM_OPTIMIZED, "Enum is optimized to Integer objects"}};
  m_opt_msg_map = std::move(opt_msg_map);
}

void OptDataMapper::init_nopt_messages() {
  std::unordered_map<int, std::string> nopt_msg_map = {
      {INL_CROSS_STORE_REFS,
       "Didn''t inline: callee references a DexMember in a dex store different "
       "from the caller''s"},
      {INL_BLOCK_LISTED_CALLEE, "Didn''t inline blocklisted method"},
      {INL_BLOCK_LISTED_CALLER, "Didn''t inline into blocklisted method"},
      {INL_EXTERN_CATCH,
       "Didn''t inline: callee has a non-public external catch type"},
      {INL_TOO_BIG, "Didn''t inline: estimated inlined method size is too big"},
      {INL_REQUIRES_API,
       "Didn''t inline: The callee has a higher required api level."},
      {INL_CREATE_VMETH,
       "Didn''t inline: callee contains invokes of methods not visible to the "
       "caller"},
      {INL_HAS_INVOKE_SUPER,
       "Didn''t inline: callee has a nonrelocatable super call"},
      {INL_UNKNOWN_VIRTUAL,
       "Didn''t inline: callee contains calls to a non-public or unknown "
       "virtual method"},
      {INL_UNKNOWN_FIELD,
       "Didn''t inline: callee references a field unknown to the caller"},
      {INL_MULTIPLE_RETURNS,
       "Didn''t inline: callee has multiple return points"},
      {INL_TOO_MANY_CALLERS,
       "Didn''t inline: this method has too many callers"},
      {INL_DO_NOT_INLINE, "Didn''t inline: the callee should not be inlined"}};
  m_nopt_msg_map = std::move(nopt_msg_map);
}

void OptDataMapper::verify_opt(OptReason reason) {
  always_assert_log(m_opt_msg_map.find(reason) != m_opt_msg_map.end(),
                    "Message not found for reason %s\n",
                    reason);
}

void OptDataMapper::verify_nopt(NoptReason reason) {
  always_assert_log(m_nopt_msg_map.find(reason) != m_nopt_msg_map.end(),
                    "Message not found for reason %s\n",
                    reason);
}

std::mutex OptDataMapper::s_opt_log_mutex;
} // namespace opt_metadata
