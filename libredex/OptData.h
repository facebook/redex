/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "DexClass.h"
#include "IRInstruction.h"
#include "OptDataDefs.h"
#include "Trace.h"
#include "Util.h"

namespace Json {
class Value;
} // namespace Json

/**
 * Usage:
 * To log an optimization/non-opt:
 *  1. Register a reason for an optimization:
 *     - Determine which level you want to log at: class/method/insn.
 *     - In OptDataDefs.h, add a value to the enum OptReason/NoptReason.
 *     - In OptData.cpp, add a case w/message to init_opt/nopt_messages().
 *  2. In your code, use the namespace opt_metadata to call
 *     - For insn-level: log_opt/nopt(reason, method, insn)
 *     - For method-level: log_opt/nopt(reason, method)
 *     - For class-level: log_opt/nopt(reason, cls)
 */
namespace opt_metadata {
class InsnOptData;
class MethodOptData;
class ClassOptData;

/**
 * Per-instruction logging functions. We require each insn log to be
 * associated with a method.
 */
void log_opt(OptReason opt, const DexMethod* method, const IRInstruction* insn);
void log_nopt(NoptReason opt,
              const DexMethod* method,
              const IRInstruction* insn);

/**
 * Per-method logging functions.
 * TODO (anwangster) Is method-method opt data needed?
 * For example: log_opt(opt, method1, method2) if methods interact
 *              with each other.
 */
void log_opt(OptReason opt, const DexMethod* method);
void log_nopt(NoptReason opt, const DexMethod* method);

/**
 * Per-class logging functions.
 * TODO (anwangster) Is class-class opt data needed?
 * For example: log_opt(opt, cls1, cls2) if classes interact with each other.
 */
void log_opt(OptReason opt, const DexClass* cls);
void log_nopt(NoptReason opt, const DexClass* cls);

/**
 * Stores per-insn optimization data.
 */
class InsnOptData {
  friend class MethodOptData;
  friend class OptDataMapper;

 public:
  InsnOptData(const DexMethod* method, const IRInstruction* insn);

 private:
  const DexMethod* m_method;
  std::string m_insn_orig;
  bool m_has_line_num{false};
  size_t m_line_num{0};
  std::vector<OptReason> m_opts;
  std::vector<NoptReason> m_nopts;
};

/**
 * Stores per-method optimization data.
 */
class MethodOptData {
  friend class InsnOptData;
  friend class ClassOptData;
  friend class OptDataMapper;

 public:
  explicit MethodOptData(const DexMethod* method);
  std::shared_ptr<InsnOptData> get_insn_opt_data(const IRInstruction* insn);

 private:
  const DexMethod* m_method;
  std::string m_method_orig;
  bool m_has_line_num{false};
  size_t m_line_num{0};
  std::vector<OptReason> m_opts;
  std::vector<NoptReason> m_nopts;
  std::unordered_map<const IRInstruction*, std::shared_ptr<InsnOptData>>
      m_insn_opt_map;
};

/**
 * Stores per-class optimization data.
 */
class ClassOptData {
  friend class OptDataMapper;

 public:
  explicit ClassOptData(const DexClass* cls);
  std::shared_ptr<MethodOptData> get_meth_opt_data(const DexMethod* method);

 private:
  const DexClass* m_cls;
  bool m_has_srcfile{false};
  std::string m_package;
  std::string m_filename;
  std::vector<OptReason> m_opts;
  std::vector<NoptReason> m_nopts;
  std::unordered_map<const DexMethod*, std::shared_ptr<MethodOptData>>
      m_meth_opt_map;
};

/**
 * Records and expresses optimization data.
 */
class OptDataMapper {
 public:
  static std::mutex s_opt_log_mutex;

  static OptDataMapper& get_instance() {
    static OptDataMapper instance;
    return instance;
  }
  OptDataMapper(OptDataMapper const&) = delete;
  void operator=(OptDataMapper const&) = delete;

  /**
   * Enable logging for the rest of this build.
   */
  void enable_logs() { m_logs_enabled = true; };

  /**
   * Records the given opt and attributes it to the given class/method/insn.
   */
  void log_opt(OptReason opt,
               const DexMethod* method,
               const IRInstruction* insn);
  void log_nopt(NoptReason opt,
                const DexMethod* method,
                const IRInstruction* insn);
  void log_opt(OptReason opt, const DexMethod* method);
  void log_nopt(NoptReason opt, const DexMethod* method);
  void log_opt(OptReason opt, const DexClass* cls);
  void log_nopt(NoptReason opt, const DexClass* cls);

  /**
   * Writes the gathered optimization data in terms of sql queries, separated
   * by newlines for easy parsing later on.
   * 11 tables are created:
   *  - opt_messages maps an optimization reason_code to a message.
   *  - nopt_messages maps a non-optimization reason_code to a message.
   *  - 6 tables that are effectively edges between level and type,
   *    where level = (class|method|instruction), type = (opts|nopts).
   *    {level}_{type} maps {level}_id to corresponding {type} reason codes,
   *    with an additional column reason_idx indicating the opt order for that
   *    specific {level}_id.
   *  - classes/methods/instructions contain basic information: a unique id,
   *    names, and in the case of instructions, the instruction itself.
   */
  Json::Value serialize_sql();

 private:
  bool m_logs_enabled{false};
  std::unordered_map<const DexClass*, std::shared_ptr<ClassOptData>>
      m_cls_opt_map;
  std::unordered_map<int /*OptReason*/, std::string> m_opt_msg_map;
  std::unordered_map<int /*NoptReason*/, std::string> m_nopt_msg_map;

  OptDataMapper() {
    init_opt_messages();
    init_nopt_messages();
  }

  /**
   * Finds and returns a ClassOptData for the given class type. If the
   * ClassOptData doesn't yet exist, construct it and return.
   */
  std::shared_ptr<ClassOptData> get_cls_opt_data(DexType* cls_type);

  /**
   * For the table {msg_type}_messages, append each row as an entry to arr.
   */
  void serialize_messages_helper(
      const std::unordered_map<int, std::string>& msg_map, Json::Value* arr);

  /**
   * For the tables {level}_opts and {level}_nopts, append each row as an
   * entry in the corresponding json arr.
   */
  void serialize_opt_nopt_helper(const std::vector<OptReason>& opts,
                                 const std::vector<NoptReason>& nopts,
                                 size_t id,
                                 Json::Value* opt_arr,
                                 Json::Value* nopt_arr);

  /**
   * For the table 'classes', append class info to arr. Append cls opt info to
   * opt/nopt arrs.
   */
  void serialize_class(const std::shared_ptr<ClassOptData>& cls_opt_data,
                       size_t cls_id,
                       Json::Value* arr,
                       Json::Value* opt_arr,
                       Json::Value* nopt_arr);

  /**
   * For the table 'methods', append method info to arr. Append meth opt info to
   * opt/nopt arrs.
   */
  void serialize_method(const std::shared_ptr<MethodOptData>& meth_opt_data,
                        size_t cls_id,
                        size_t meth_id,
                        Json::Value* arr,
                        Json::Value* opt_arr,
                        Json::Value* nopt_arr);

  /**
   * For the table 'instructions', append instruction info to arr. Append insn
   * opt info to opt/nopt arrs.
   */
  void serialize_insn(const std::shared_ptr<InsnOptData>& insn_opt_data,
                      size_t meth_id,
                      size_t insn_id,
                      Json::Value* arr,
                      Json::Value* opt_arr,
                      Json::Value* nopt_arr);

  /**
   * NOTE: Register an opt/non-opt message to the corresponding init_ function.
   * Adds messages to m_opt_msg_map, m_nopt_msg_map.
   */
  void init_opt_messages();
  void init_nopt_messages();

  /**
   * Verifies that a message has been registered for the given reason.
   */
  void verify_opt(OptReason reason);
  void verify_nopt(NoptReason reason);
};
} // namespace opt_metadata
