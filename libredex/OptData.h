/**
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

/**
 * Usage:
 * To log an optimization/non-opt:
 *  1. Register a reason for an optimization:
 *     - Determine which level you want to log at: class/method/insn.
 *     - Add a value to the enum OptReason in OptDataDefs.h
 *     - In OptData.cpp, add a case w/message to the relevant
 *       write_(class/method/insn)_opt function.
 *  2. In your code, call
 *     - For insn-level: log_opt(opt, method, insn)
 *     - For method-level: log_opt(opt, method)
 *     - For class-level: log_opt(opt, cls)
 */
namespace opt_metadata {
class InsnOptData;
class MethodOptData;
class ClassOptData;

/**
 * Per-instruction logging functions. We require each insn log to be
 * associated with a method.
 */
void log_opt(OptReason opt, DexMethod* method, IRInstruction* insn);

/**
 * Per-method logging functions.
 * TODO (anwangster) Is method-method opt data needed?
 * For example: log_opt(opt, method1, method2) if methods interact
 *              with each other.
 */
void log_opt(OptReason opt, DexMethod* method);

/**
 * Per-class logging functions.
 * TODO (anwangster) Is class-class opt data needed?
 * For example: log_opt(opt, cls1, cls2) if classes interact with each other.
 */
void log_opt(OptReason opt, DexClass* cls);

/**
 * Stores per-insn optimization data.
 */
class InsnOptData {
  friend class MethodOptData;
  friend class OptDataMapper;

 public:
  InsnOptData(DexMethod* method, IRInstruction* insn);
  void add_opt_data(OptReason opt);

 private:
  DexMethod* m_method;
  bool m_has_line_num{false};
  IRInstruction* m_insn;
  size_t m_line_num{0};
  std::vector<OptReason> m_opts;
};

/**
 * Stores per-method optimization data.
 */
class MethodOptData {
  friend class InsnOptData;
  friend class ClassOptData;
  friend class OptDataMapper;

 public:
  MethodOptData(DexMethod* method);
  void add_opt_data(OptReason opt);
  std::shared_ptr<InsnOptData> get_insn_opt_data(IRInstruction* insn);

 private:
  DexMethod* m_method;
  bool m_has_line_num{false};
  size_t m_line_num{0};
  std::vector<OptReason> m_opts;
  std::unordered_map<IRInstruction*, std::shared_ptr<InsnOptData>>
      m_insn_opt_map;
};

/**
 * Stores per-class optimization data.
 */
class ClassOptData {
  friend class OptDataMapper;

 public:
  ClassOptData(DexClass* cls);
  void add_opt_data(OptReason opt);
  std::shared_ptr<MethodOptData> get_meth_opt_data(DexMethod* method);

 private:
  DexClass* m_cls;
  bool m_has_srcfile{false};
  std::string m_package;
  std::string m_filename;
  std::vector<OptReason> m_opts;
  std::unordered_map<DexMethod*, std::shared_ptr<MethodOptData>> m_meth_opt_map;
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
   * For now, every log attempt succeeds.
   */
  bool log_enabled(TraceModule module);

  /**
   * Records the given opt and attributes it to the given class/method/insn.
   */
  void log_opt(OptReason opt, DexMethod* method, IRInstruction* insn);
  void log_opt(OptReason opt, DexMethod* method);
  void log_opt(OptReason opt, DexClass* cls);

  /**
   * Writes the gathered optimization data in human-readable format. Relies on
   * write_insn_opt/write_meth_opt/write_cls_opt.
   */
  void write_opt_data(const std::string& filename);

 private:
  std::unordered_map<DexClass*, std::shared_ptr<ClassOptData>> m_cls_opt_map;

  OptDataMapper() {}

  /**
   * Finds and returns a ClassOptData for the given class type. If the
   * ClassOptData doesn't yet exist, construct it and return.
   */
  std::shared_ptr<ClassOptData> get_cls_opt_data(DexType* cls_type);

  /**
   * For the given InsnOptData and opt code, write a useful optimization message
   * to the given file.
   */
  void write_insn_opt(std::shared_ptr<InsnOptData> insn_opt_data,
                      OptReason opt,
                      FILE* file);

  /**
   * For the given MethodOptData and opt code, write a useful optimization
   * message to the given file.
   */
  void write_meth_opt(std::shared_ptr<MethodOptData> meth_opt_data,
                      OptReason opt,
                      FILE* file);

  /**
   * For the given ClassOptData and opt code, write a useful optimization
   * message to the given file.
   */
  void write_cls_opt(std::shared_ptr<ClassOptData> cls_opt_data,
                     OptReason opt,
                     FILE* file);
};
} // namespace opt_metadata
