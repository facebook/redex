/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Instrument.h"

#include "BlockInstrument.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "InterDexPass.h"
#include "InterDexPassPlugin.h"
#include "Match.h"
#include "MethodReference.h"
#include "PassManager.h"
#include "Show.h"
#include "TypeSystem.h"
#include "Walkers.h"

#include <boost/algorithm/string.hpp>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace instrument;

/*
 * This pass performs instrumentation for dynamic (runtime) analysis.
 *
 * Analysis code, which should be a static public method, is written in Java.
 * Its class and method names are specified in the config. This pass then
 * inserts the method to points of interest. For a starting example, we
 * implement the "onMethodBegin" instrumentation.
 */
namespace {

constexpr bool instr_debug = false;

constexpr const char* SIMPLE_METHOD_TRACING = "simple_method_tracing";
constexpr const char* BASIC_BLOCK_TRACING = "basic_block_tracing";
constexpr const char* METHOD_REPLACEMENT = "methods_replacement";

class InstrumentInterDexPlugin : public interdex::InterDexPassPlugin {
 public:
  explicit InstrumentInterDexPlugin(size_t max_analysis_methods)
      : m_max_analysis_methods(max_analysis_methods) {}

  void configure(const Scope& scope, ConfigFiles& cfg) override{};

  bool should_skip_class(const DexClass* clazz) override { return false; }

  void gather_refs(const interdex::DexInfo& dex_info,
                   const DexClass* cls,
                   std::vector<DexMethodRef*>& mrefs,
                   std::vector<DexFieldRef*>& frefs,
                   std::vector<DexType*>& trefs,
                   std::vector<DexClass*>* erased_classes,
                   bool should_not_relocate_methods_of_class) override {}

  size_t reserve_frefs() override {
    // We may introduce a new field
    return 1;
  }

  size_t reserve_trefs() override {
    // We introduce a type reference to the analysis class in each dex
    return 1;
  }

  size_t reserve_mrefs() override {
    // In each dex, we will introduce more method refs from analysis methods.
    // This makes sure that the inter-dex pass keeps space for new method refs.
    return m_max_analysis_methods;
  }

  DexClasses additional_classes(const DexClassesVector& outdex,
                                const DexClasses& classes) override {
    return {};
  }

  void cleanup(const std::vector<DexClass*>& scope) override {}

 private:
  const size_t m_max_analysis_methods;
};

// For example, say that "Lcom/facebook/debug/" is in the set. We match either
// "^Lcom/facebook/debug/*" or "^Lcom/facebook/debug;".
bool match_class_name(std::string cls_name,
                      const std::unordered_set<std::string>& set) {
  always_assert(cls_name.back() == ';');
  // We also support exact class name (e.g., "Lcom/facebook/Debug;")
  if (set.count(cls_name)) {
    return true;
  }
  cls_name.back() = '/';
  size_t pos = cls_name.find('/', 0);
  while (pos != std::string::npos) {
    if (set.count(cls_name.substr(0, pos + 1))) {
      return true;
    }
    pos = cls_name.find('/', pos + 1);
  }
  return false;
}

void instrument_onMethodBegin(DexMethod* method,
                              int index,
                              DexMethod* method_onMethodBegin) {
  IRCode* code = method->get_code();
  assert(code != nullptr);

  IRInstruction* const_inst = new IRInstruction(OPCODE_CONST);
  const_inst->set_literal(index);
  const auto reg_dest = code->allocate_temp();
  const_inst->set_dest(reg_dest);

  IRInstruction* invoke_inst = new IRInstruction(OPCODE_INVOKE_STATIC);
  invoke_inst->set_method(method_onMethodBegin);
  invoke_inst->set_srcs_size(1);
  invoke_inst->set_src(0, reg_dest);

  // TODO(minjang): Consider using get_param_instructions.
  // Try to find a right insertion point: the entry point of the method.
  // We skip any fall throughs and IOPCODE_LOAD_PARRM*.
  auto insert_point = std::find_if_not(
      code->begin(), code->end(), [&](const MethodItemEntry& mie) {
        return mie.type == MFLOW_FALLTHROUGH ||
               (mie.type == MFLOW_OPCODE &&
                opcode::is_a_load_param(mie.insn->opcode()));
      });

  if (insert_point == code->end()) {
    // No load params. So just insert before the head.
    insert_point = code->begin();
  } else if (insert_point->type == MFLOW_DEBUG) {
    // Right after the load params, there could be DBG_SET_PROLOGUE_END.
    // Skip if there is a following POSITION, too. For example:
    // 1: OPCODE: IOPCODE_LOAD_PARAM_OBJECT v1
    // 2: OPCODE: IOPCODE_LOAD_PARAM_OBJECT v2
    // 3: DEBUG: DBG_SET_PROLOGUE_END
    // 4: POSITION: foo.java:42 (this might be optional.)
    // <== Instrumentation code will be inserted here.
    //
    std::advance(insert_point,
                 std::next(insert_point)->type != MFLOW_POSITION ? 1 : 2);
  } else {
    // Otherwise, insert_point can be used directly.
  }

  code->insert_before(code->insert_before(insert_point, invoke_inst),
                      const_inst);

  if (instr_debug) {
    for (auto it = code->begin(); it != code->end(); ++it) {
      if (it == insert_point) {
        TRACE(INSTRUMENT, 9, "<==== insertion");
        TRACE(INSTRUMENT, 9, "%s", SHOW(*it));
        ++it;
        if (it != code->end()) {
          TRACE(INSTRUMENT, 9, "%s", SHOW(*it));
          ++it;
          if (it != code->end()) {
            TRACE(INSTRUMENT, 9, "%s", SHOW(*it));
          }
        }
        TRACE(INSTRUMENT, 9, "");
        break;
      }
      TRACE(INSTRUMENT, 9, "%s", SHOW(*it));
    }
  }
}

void do_simple_method_tracing(DexClass* analysis_cls,
                              DexStoresVector& stores,
                              ConfigFiles& cfg,
                              PassManager& pm,
                              const InstrumentPass::Options& options) {
  const size_t NUM_SHARDS = options.num_shards;
  const auto& array_fields =
      InstrumentPass::patch_sharded_arrays(analysis_cls, NUM_SHARDS);
  always_assert(array_fields.size() == NUM_SHARDS);
  const auto& analysis_methods =
      InstrumentPass::generate_sharded_analysis_methods(
          analysis_cls, options.analysis_method_name, array_fields, NUM_SHARDS);
  const auto& analysis_method_map = analysis_methods.first;
  const auto& analysis_method_names = analysis_methods.second;

  // Write metadata file with more information.
  const auto& file_name = cfg.metafile(options.metadata_file_name);
  std::ofstream ofs(file_name, std::ofstream::out | std::ofstream::trunc);

  // Write meta info of the meta file: the type of the meta file and version.
  ofs << "#,simple-method-tracing,1.0" << std::endl;

  size_t method_id = 0;
  int excluded = 0;
  std::unordered_set<std::string> method_names;
  std::vector<DexMethod*> to_instrument;

  auto worker = [&](DexMethod* method, size_t& total_size) -> int {
    const auto& name = method->get_deobfuscated_name();
    always_assert_log(
        !name.empty(),
        "Deobfuscated method name can't be empty: obfuscated "
        "name: %s, class: \'%s\'(%s)",
        SHOW(method->get_name()),
        SHOW(type_class(method->get_class())->get_deobfuscated_name()),
        SHOW(method->get_class()->get_name()));
    always_assert_log(
        !method_names.count(name),
        "Deobfuscated method names must be unique, but found duplicate: \'%s\'",
        SHOW(name));
    method_names.insert(name);

    if (method->get_code() == nullptr) {
      ofs << "M,-1," << name << ",0,\"" << vshow(method->get_access(), true)
          << "\"\n";
      return 0;
    }

    const size_t sum_opcode_sizes = method->get_code()->sum_opcode_sizes();
    total_size += sum_opcode_sizes;

    // Excluding analysis methods myselves.
    if (analysis_method_names.count(method->get_name()->str()) ||
        method == analysis_cls->get_clinit()) {
      ++excluded;
      TRACE(INSTRUMENT, 2, "Excluding analysis method: %s", SHOW(method));
      ofs << "M,-1," << name << "," << sum_opcode_sizes << ",\""
          << "MYSELF " << vshow(method->get_access(), true) << "\"\n";
      return 0;
    }

    // Handle allowlist and blocklist.
    if (!options.allowlist.empty()) {
      if (InstrumentPass::is_included(method, options.allowlist)) {
        TRACE(INSTRUMENT, 8, "Allowlist: included: %s", SHOW(method));
      } else {
        ++excluded;
        TRACE(INSTRUMENT, 9, "Allowlist: excluded: %s", SHOW(method));
        return 0;
      }
    }

    // In case of a conflict, when an entry is present in both blocklist
    // and allowlist, the blocklist is given priority and the entry
    // is not instrumented.
    if (InstrumentPass::is_included(method, options.blocklist)) {
      ++excluded;
      TRACE(INSTRUMENT, 8, "Blocklist: excluded: %s", SHOW(method));
      ofs << "M,-1," << name << "," << sum_opcode_sizes << ",\""
          << "BLOCKLIST " << vshow(method->get_access(), true) << "\"\n";
      return 0;
    }

    TRACE(INSTRUMENT, 8, "%zu: %s", method_id, SHOW(method));
    assert(to_instrument.size() == method_id);
    to_instrument.push_back(method);

    // Emit metadata to the file.
    ofs << "M," << method_id << "," << name << "," << sum_opcode_sizes << ",\""
        << vshow(method->get_access(), true /*is_method*/) << "\"\n";
    ++method_id;
    return 1;
  };

  auto scope = build_class_scope(stores);
  TypeSystem ts(scope);

  // We now have sharded method stats arrays. We interleave methods into
  // multiple arrays. Say we instrument 11 methods and have 3 arrays. Each array
  // may have up to floor(11/3) + 1 = 4 methods. Their distributions look like:
  //
  //                0                   1
  //   method id    0 1 2 3 4 5 6 7 8 9 0
  //   array id     0 1 2 0 1 2 0 1 2 0 1  <= i % 3
  //   array index  0 0 0 1 1 1 2 2 2 3 3  <= i / 3
  //
  //                  arrays[0]     arrays[1]    arrays[2]
  //   method id    [0, 3, 6, 9]  [1, 4, 7, 10]  [2, 5, 8]
  //
  // Be extremely careful when handling indexes. The Java-side uploader needs to
  // untangle the arrays. The WWW endpoints do not need to know this complexity.
  // So, only devices handle this sharding.
  //
  // In order to do that, we need to know the total number of methods to be
  // instrumented. We don't know this number until iterating all methods while
  // processing exclusions. We take a two-pass approach:
  //  1) For all methods, collect (method id, method) pairs and write meta data.
  //  2) Do actual instrumentation.
  for (const auto& cls : scope) {
    const auto& cls_name = cls->get_deobfuscated_name();
    always_assert_log(
        !method_names.count(cls_name),
        "Deobfuscated class names must be unique, but found duplicate: %s",
        SHOW(cls_name));
    method_names.insert(cls_name);

    int instrumented = 0;
    size_t total_size = 0;
    for (auto dmethod : cls->get_dmethods()) {
      instrumented += worker(dmethod, total_size);
    }
    for (auto vmethod : cls->get_vmethods()) {
      instrumented += worker(vmethod, total_size);
    }

    ofs << "C," << cls_name << "," << total_size << ","
        << (instrumented == 0 ? "NONE" : std::to_string(instrumented)) << ","
        << cls->get_dmethods().size() << "," << cls->get_vmethods().size()
        << ",\"" << vshow(cls->get_access(), false /*is_method*/) << "\"\n";

    // Enumerate all super and interface classes for this class.
    const auto& obj_type = DexType::get_type("Ljava/lang/Object;");
    std::stringstream ss_parents;
    for (const auto& e : ts.parent_chain(cls->get_type())) {
      // Exclude myself and obvious java.lang.Object.
      if (e != obj_type && e != cls->get_type()) {
        ss_parents << show_deobfuscated(e) << " ";
      }
    }
    if (ss_parents.tellp() > 0) {
      ofs << "P," << cls_name << ",\"" << ss_parents.str() << "\"\n";
    }

    std::stringstream ss_interfaces;
    for (const auto& e : ts.get_all_super_interfaces(cls->get_type())) {
      ss_interfaces << show_deobfuscated(e) << " ";
    }
    if (ss_interfaces.tellp() > 0) {
      ofs << "I," << cls_name << ",\"" << ss_interfaces.str() << "\"\n";
    }
  }

  // Now we know the total number of methods to be instrumented. Do some
  // computations and actual instrumentation.
  const size_t kTotalSize = to_instrument.size();
  TRACE(INSTRUMENT, 2, "%zu methods to be instrumented; shard size: %zu (+1)",
        kTotalSize, kTotalSize / NUM_SHARDS);
  for (size_t i = 0; i < kTotalSize; ++i) {
    TRACE(INSTRUMENT, 6, "Sharded %zu => [%zu][%zu] %s", i, (i % NUM_SHARDS),
          (i / NUM_SHARDS), SHOW(to_instrument[i]));
    instrument_onMethodBegin(to_instrument[i],
                             (i / NUM_SHARDS) * options.num_stats_per_method,
                             analysis_method_map.at((i % NUM_SHARDS) + 1));
  }

  TRACE(INSTRUMENT,
        1,
        "%d methods were instrumented (%d methods were excluded)",
        method_id,
        excluded);

  // Patch stat array sizes.
  for (size_t i = 0; i < NUM_SHARDS; ++i) {
    size_t n = kTotalSize / NUM_SHARDS + (i < kTotalSize % NUM_SHARDS ? 1 : 0);
    // Get obfuscated name corresponding to each sMethodStat[1-N] field.
    const auto field_name = array_fields.at(i + 1)->get_name()->str();
    InstrumentPass::patch_array_size(analysis_cls, field_name,
                                     options.num_stats_per_method * n);
  }

  // Patch method count constant.
  always_assert(method_id == kTotalSize);
  auto field = analysis_cls->find_field_from_simple_deobfuscated_name(
      "sNumStaticallyInstrumented");
  always_assert(field != nullptr);
  InstrumentPass::patch_static_field(analysis_cls, field->get_name()->str(),
                                     kTotalSize);

  field =
      analysis_cls->find_field_from_simple_deobfuscated_name("sProfileType");
  always_assert(field != nullptr);
  InstrumentPass::patch_static_field(
      analysis_cls, field->get_name()->str(),
      static_cast<int>(ProfileTypeFlags::SimpleMethodTracing));

  ofs.close();
  TRACE(INSTRUMENT, 2, "Index file was written to: %s", SHOW(file_name));

  pm.incr_metric("Instrumented", method_id);
  pm.incr_metric("Excluded", excluded);
}

std::unordered_set<std::string> load_blocklist_file(
    const std::string& file_name) {
  // Assume the file simply enumerates blocklisted names.
  std::unordered_set<std::string> ret;
  std::ifstream ifs(file_name);
  assert_log(ifs, "Can't open blocklist file: %s\n", SHOW(file_name));

  std::string line;
  while (ifs >> line) {
    ret.insert(line);
  }

  TRACE(INSTRUMENT, 3, "Loaded %zu blocklist entries from %s", ret.size(),
        SHOW(file_name));
  return ret;
}

} // namespace

constexpr const char* InstrumentPass::STATS_FIELD_NAME;

// Find a sequence of opcode that creates a static array. Patch the array size.
void InstrumentPass::patch_array_size(DexClass* analysis_cls,
                                      const std::string& array_name,
                                      const int array_size) {
  DexMethod* clinit = analysis_cls->get_clinit();
  always_assert(clinit != nullptr);

  auto* code = clinit->get_code();
  bool patched = false;
  walk::matching_opcodes_in_block(
      *clinit,
      // Don't find OPCODE_CONST. It might be deduped with others, or changing
      // this const can affect other instructions. (Well, we might have a
      // unique const number though.) So, just create a new const load
      // instruction. LocalDCE can clean up the redundant instructions.
      std::make_tuple(/* m::const_(), */
                      m::new_array_(),
                      m::move_result_pseudo_object_(),
                      m::sput_object_()),
      [&](DexMethod* method,
          cfg::Block*,
          const std::vector<IRInstruction*>& insts) {
        assert(method == clinit);
        if (insts[2]->get_field()->get_name()->str() != array_name) {
          return;
        }

        IRInstruction* const_inst = new IRInstruction(OPCODE_CONST);
        const_inst->set_literal(array_size);
        const auto reg_dest = code->allocate_temp();
        const_inst->set_dest(reg_dest);
        insts[0]->set_src(0, reg_dest);
        for (auto& mie : InstructionIterable(code)) {
          if (mie.insn == insts[0]) {
            code->insert_before(code->iterator_to(mie), const_inst);
            patched = true;
            return;
          }
        }
      });

  if (!patched) {
    std::cerr << "[InstrumentPass] error: cannot patch array size."
              << std::endl;
    std::cerr << show(clinit->get_code()) << std::endl;
    exit(1);
  }

  TRACE(INSTRUMENT, 2, "%s array was patched: %d", SHOW(array_name),
        array_size);
}

void InstrumentPass::patch_static_field(DexClass* analysis_cls,
                                        const std::string& field_name,
                                        const int new_number) {
  DexMethod* clinit = analysis_cls->get_clinit();
  always_assert(clinit != nullptr);

  // Find the sput with the given field name.
  auto* code = clinit->get_code();
  IRInstruction* sput_inst = nullptr;
  IRList::iterator insert_point;
  for (auto& mie : InstructionIterable(code)) {
    auto* insn = mie.insn;
    if (insn->opcode() == OPCODE_SPUT &&
        insn->get_field()->get_name()->str() == field_name) {
      sput_inst = insn;
      insert_point = code->iterator_to(mie);
      break;
    }
  }

  // SPUT can be null if the original field value was encoded in the
  // static_values_off array. And consider simplifying using make_concrete.
  if (sput_inst == nullptr) {
    TRACE(INSTRUMENT, 2, "sput %s was deleted; creating it", SHOW(field_name));
    sput_inst = new IRInstruction(OPCODE_SPUT);
    sput_inst->set_field(
        DexField::make_field(DexType::make_type(analysis_cls->get_name()),
                             DexString::make_string(field_name),
                             DexType::make_type("I")));
    insert_point =
        code->insert_after(code->get_param_instructions().end(), sput_inst);
  }

  // Create a new const instruction just like patch_stat_array_size.
  IRInstruction* const_inst = new IRInstruction(OPCODE_CONST);
  const_inst->set_literal(new_number);
  const auto reg_dest = code->allocate_temp();
  const_inst->set_dest(reg_dest);

  sput_inst->set_src(0, reg_dest);
  code->insert_before(insert_point, const_inst);
  TRACE(INSTRUMENT, 2, "%s was patched: %d", SHOW(field_name), new_number);
}

void InstrumentPass::bind_config() {
  bind("instrumentation_strategy", "", m_options.instrumentation_strategy);
  bind("analysis_class_name", "", m_options.analysis_class_name);
  bind("analysis_method_name", "", m_options.analysis_method_name);
  bind("blocklist", {}, m_options.blocklist);
  bind("allowlist", {}, m_options.allowlist);
  bind("blocklist_file_name", "", m_options.blocklist_file_name);
  bind("metadata_file_name", "redex-instrument-metadata.txt",
       m_options.metadata_file_name);
  bind("num_stats_per_method", 1, m_options.num_stats_per_method);
  bind("num_shards", 1, m_options.num_shards);
  // Note: only_cold_start_class is only used for block tracing.
  bind("only_cold_start_class", false, m_options.only_cold_start_class);
  bind("methods_replacement", {}, m_options.methods_replacement,
       "Replacing instance method call with static method call.",
       Configurable::bindflags::methods::error_if_unresolvable);
  bind("analysis_method_names", {}, m_options.analysis_method_names);
  // 0 means the block tracing is effectively method-only tracing.
  bind("max_num_blocks", 0, m_options.max_num_blocks);
  bind("instrument_catches", false, m_options.instrument_catches);

  size_t max_analysis_methods;
  if (m_options.instrumentation_strategy == SIMPLE_METHOD_TRACING) {
    max_analysis_methods = m_options.num_shards;
  } else if (m_options.instrumentation_strategy == BASIC_BLOCK_TRACING) {
    // Our current DynamicAnalysis has 7 onMethodExits and 1 onMethodBegin.
    max_analysis_methods = 8;
  } else {
    max_analysis_methods = 1;
  }

  after_configuration([this, max_analysis_methods] {
    // Make a small room for additional method refs during InterDex.
    interdex::InterDexRegistry* registry =
        static_cast<interdex::InterDexRegistry*>(
            PluginRegistry::get().pass_registry(interdex::INTERDEX_PASS_NAME));
    registry->register_plugin(
        "INSTRUMENT_PASS_PLUGIN", [max_analysis_methods]() {
          return new InstrumentInterDexPlugin(max_analysis_methods);
        });
    // Currently we only support instance call to static call.
    for (auto& pair : m_options.methods_replacement) {
      always_assert(!is_static(pair.first));
      always_assert(is_static(pair.second));
    }
    if (m_options.instrumentation_strategy == METHOD_REPLACEMENT) {
      always_assert_log(
          !m_options.methods_replacement.empty(),
          "Invalid configuration, `methods_replacement` should not be empty\n");
    }
  });
}

// Check for inclusion in allow/block lists of methods/classes. It supports:
// - "Lcom/fb/foo/" matches "^Lcom/fb/foo/*" or "^Lcom/facebook/debug;"
// - "Lcom/fb/foo;.bar()V" matches exact full method names.
// - "Lcom/fb/foo;.bar*" matches method name prefixes.
bool InstrumentPass::is_included(const DexMethod* method,
                                 const std::unordered_set<std::string>& set) {
  if (set.empty()) {
    return false;
  }

  // Try to check for method by its full name.
  const auto& full_method_name = method->get_deobfuscated_name();
  if (set.count(full_method_name)) {
    return true;
  }

  // Prefix method name matching.
  for (const auto& pattern : set) {
    if (pattern.back() == '*') {
      if (full_method_name.find(pattern.substr(0, pattern.length() - 1)) !=
          std::string::npos) {
        return true;
      }
    }
  }

  return match_class_name(show_deobfuscated(method->get_class()), set);
}

std::pair<std::unordered_map<int /*shard_num*/, DexMethod*>,
          std::unordered_set<std::string>>
InstrumentPass::generate_sharded_analysis_methods(
    DexClass* cls,
    const std::string& template_method_full_name,
    const std::unordered_map<int /*shard_num*/, DexFieldRef*>& array_fields,
    const size_t num_shards) {
  DexMethod* template_method =
      cls->find_method_from_simple_deobfuscated_name(template_method_full_name);

  if (template_method == nullptr) {
    std::cerr << "[InstrumentPass] error: failed to find template method \'"
              << template_method_full_name << "\' in " << show(*cls)
              << std::endl;
    for (const auto& m : cls->get_dmethods()) {
      std::cerr << " " << show(m) << std::endl;
    }
    exit(1);
  }

  const std::string& template_method_name = template_method->get_name()->str();

  std::unordered_map<int /*shard_num*/, DexMethod*> new_analysis_methods;
  std::unordered_set<std::string> method_names;

  // Even if one shard, we create a new method from the template method.
  for (size_t i = 1; i <= num_shards; ++i) {
    const auto new_name = template_method_name + std::to_string(i);
    std::string deobfuscated_name = template_method->get_deobfuscated_name();
    boost::replace_first(deobfuscated_name, template_method_name, new_name);

    DexMethod* new_method =
        DexMethod::make_method_from(template_method,
                                    template_method->get_class(),
                                    DexString::make_string(new_name));
    new_method->set_deobfuscated_name(deobfuscated_name);
    cls->add_method(new_method);

    // Patch the array name in newly created method.
    bool patched = false;
    walk::matching_opcodes_in_block(
        *new_method,
        std::make_tuple(m::sget_object_()),
        [&](DexMethod* method,
            cfg::Block*,
            const std::vector<IRInstruction*>& insts) {
          DexField* field = static_cast<DexField*>(insts[0]->get_field());
          if (field->get_simple_deobfuscated_name() ==
              InstrumentPass::STATS_FIELD_NAME) {
            // Set the new field created from patch_sharded_arrays.
            insts[0]->set_field(array_fields.at(i));
            patched = true;
            return;
          }
        });

    always_assert_log(patched, "Failed to patch sMethodStats1 in %s\n",
                      SHOW(new_method));
    method_names.insert(new_name);
    new_analysis_methods[i] = new_method;
    TRACE(INSTRUMENT, 2, "Created %s with %s", SHOW(new_method),
          SHOW(array_fields.at(i)));
  }

  // Remove template method.
  cls->remove_method(template_method);
  return std::make_pair(new_analysis_methods, method_names);
}

std::unordered_map<int /*shard_num*/, DexFieldRef*>
InstrumentPass::patch_sharded_arrays(
    DexClass* cls,
    const size_t num_shards,
    const std::map<int /*shard_num*/, std::string>& suggested_names) {
  // Insert additional sMethodStatsN into the clinit
  //
  // private static short[] sMethodStats1 = new short[0];
  // private static short[] sMethodStats2 = new short[0]; <= Add
  // ...
  // private static short[] sMethodStatsN = new short[0]; <= Add
  //
  //        OPCODE: CONST v0, 0
  //        OPCODE: NEW_ARRAY v0, [S
  //        OPCODE: IOPCODE_MOVE_RESULT_PSEUDO_OBJECT v1
  //        OPCODE: SPUT_OBJECT v1, Lcom/foo/Bar;.sMethodStats1:[S
  // Add => OPCODE: NEW_ARRAY v0, [S
  // Add => OPCODE: IOPCODE_MOVE_RESULT_PSEUDO_OBJECT v1
  // Add => OPCODE: SPUT_OBJECT v1, Lcom/foo/Bar;.sMethodStats2:[S
  always_assert(num_shards > 0);
  DexMethod* clinit = cls->get_clinit();
  IRCode* code = clinit->get_code();
  std::unordered_map<int /*shard_num*/, DexFieldRef*> fields;
  bool patched = false;
  walk::matching_opcodes_in_block(
      *clinit,
      std::make_tuple(m::new_array_(), m::move_result_pseudo_object_(),
                      m::sput_object_()),
      [&](DexMethod* method,
          cfg::Block*,
          const std::vector<IRInstruction*>& insts) {
        DexField* template_field =
            static_cast<DexField*>(insts[2]->get_field());
        if (template_field->get_simple_deobfuscated_name() !=
            InstrumentPass::STATS_FIELD_NAME) {
          return;
        }

        // Create new sMethodStatsN fields. Even if num_shard is 1, we create
        // new field from the template field. Regarding obfuscation, the rename
        // module runs after InstrumentPass. So, we just need to assign
        // human-readable names here.
        for (size_t i = 1; i <= num_shards; i++) {
          const auto new_name =
              suggested_names.count(i)
                  ? suggested_names.at(i)
                  : InstrumentPass::STATS_FIELD_NAME + std::to_string(i);
          auto deobfuscated_name = template_field->get_deobfuscated_name();
          boost::replace_first(deobfuscated_name,
                               InstrumentPass::STATS_FIELD_NAME, new_name);

          DexField* new_field = static_cast<DexField*>(
              DexField::make_field(template_field->get_class(),
                                   DexString::make_string(new_name),
                                   template_field->get_type()));
          new_field->set_deobfuscated_name(deobfuscated_name);
          new_field->make_concrete(template_field->get_access(),
                                   template_field->get_static_value());
          fields[i] = new_field;
          TRACE(INSTRUMENT, 2, "Created array: %s", SHOW(new_field));
          cls->add_field(new_field);
        }

        // Clone the matched three instructions, but with new field names.
        for (size_t i = num_shards; i >= 1; --i) {
          auto new_insts = {
              (new IRInstruction(OPCODE_NEW_ARRAY))
                  ->set_type(insts[0]->get_type())
                  ->set_src(0, insts[0]->src(0)),
              (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                  ->set_dest(insts[1]->dest()),
              (new IRInstruction(OPCODE_SPUT_OBJECT))
                  ->set_src(0, insts[2]->src(0))
                  ->set_field(fields.at(i))};
          if (i == 1) {
            code->replace_opcode(insts[2], new_insts);
          } else {
            code->insert_after(insts[2], new_insts);
          }
        }
        patched = true;
        cls->remove_field(template_field);
      });

  always_assert_log(patched, "Failed to insert sMethodStatsN:\n%s",
                    SHOW(clinit->get_code()));

  // static short[][] sMethodStatsArray = new short[][] {
  //   sMethodStats1, <== Add
  //   sMethodStats2, <== Add
  //   ...
  // }
  //
  //        OPCODE: NEW_ARRAY v0, [[S  <== Patch
  //        OPCODE: IOPCODE_MOVE_RESULT_PSEUDO_OBJECT vX
  //        OPCODE: SPUT_OBJECT vX, Lcom/foo;.sMethodStatsArray:[[S
  // Add => OPCODE: SGET_OBJECT Lcom/foo;.sMethodStats1:[S
  // Add => OPCODE: IOPCODE_MOVE_RESULT_PSEUDO_OBJECT vY
  // Add => OPCODE: CONST vN, index
  // Add => OPCODE: APUT_OBJECT vY, vX, vN
  //        ...
  // Add => OPCODE: APUT_OBJECT vY, vX, vN
  auto field =
      cls->find_field_from_simple_deobfuscated_name("sMethodStatsArray");
  always_assert(field != nullptr);
  InstrumentPass::patch_array_size(cls, field->get_name()->str(), num_shards);
  patched = false;
  walk::matching_opcodes_in_block(
      *clinit,
      std::make_tuple(m::new_array_(), m::move_result_pseudo_object_(),
                      m::sput_object_()),
      [&](DexMethod* method,
          cfg::Block*,
          const std::vector<IRInstruction*>& insts) {
        DexField* field = static_cast<DexField*>(insts[2]->get_field());
        if (field->get_simple_deobfuscated_name() != "sMethodStatsArray") {
          return;
        }

        const reg_t vX = insts[1]->dest();
        const reg_t vY = code->allocate_temp();
        const reg_t vN = code->allocate_temp();
        for (size_t i = num_shards; i >= 1; --i) {
          code->insert_after(
              insts[2],
              {(new IRInstruction(OPCODE_SGET_OBJECT))->set_field(fields.at(i)),
               (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                   ->set_dest(vY),
               (new IRInstruction(OPCODE_CONST))
                   ->set_literal(i - 1)
                   ->set_dest(vN),
               (new IRInstruction(OPCODE_APUT_OBJECT))
                   ->set_srcs_size(3)
                   ->set_src(0, vY)
                   ->set_src(1, vX)
                   ->set_src(2, vN)});
        }
        patched = true;
      });

  always_assert_log(patched,
                    "Failed to insert sMethodStatsN to sMethodStatsArray:\n%s",
                    SHOW(clinit->get_code()));

  return fields;
}

void InstrumentPass::run_pass(DexStoresVector& stores,
                              ConfigFiles& cfg,
                              PassManager& pm) {
  // TODO(fengliu): We may need change this but leave it here for local test.
  if (m_options.instrumentation_strategy == METHOD_REPLACEMENT) {
    bool exclude_primary_dex =
        pm.get_redex_options().is_art_build ? false : true;
    auto num_wrapped_invocations =
        method_reference::wrap_instance_call_with_static(
            stores, m_options.methods_replacement, exclude_primary_dex);
    pm.set_metric("wrapped_invocations", num_wrapped_invocations);
    return;
  }

  if (!cfg.get_json_config().get("instrument_pass_enabled", false) &&
      !pm.get_redex_options().instrument_pass_enabled) {
    TRACE(INSTRUMENT, 1,
          "--enable-instrument-pass (or \"instrument_pass_enabled\": true) is "
          "not specified.");
    return;
  }

  // Append block listed classes from the file, if exists.
  if (!m_options.blocklist_file_name.empty()) {
    for (const auto& e : load_blocklist_file(m_options.blocklist_file_name)) {
      m_options.blocklist.insert(e);
    }
  }

  if (m_options.analysis_class_name.empty()) {
    std::cerr << "[InstrumentPass] error: empty analysis class name."
              << std::endl;
    exit(1);
  }

  // Get the analysis class.
  DexType* analysis_class_type = g_redex->get_type(
      DexString::get_string(m_options.analysis_class_name.c_str()));
  if (analysis_class_type == nullptr) {
    std::cerr << "[InstrumentPass] error: cannot find analysis class: "
              << m_options.analysis_class_name << std::endl;
    exit(1);
  }

  DexClass* analysis_cls = g_redex->type_class(analysis_class_type);
  always_assert(analysis_cls != nullptr);

  // Check whether the analysis class is in the primary dex. We use a heuristic
  // that looks the last 12 characters of the location of the given dex.
  auto dex_loc = analysis_cls->get_location();
  if (dex_loc.size() < 12 /* strlen("/classes.dex") == 12 */ ||
      dex_loc.substr(dex_loc.size() - 12) != "/classes.dex") {
    std::cerr << "[InstrumentPass] Analysis class must be in the primary dex. "
                 "It was in "
              << dex_loc << std::endl;
    exit(1);
  }

  // Just do the very minimal common work here: load the analysis class.
  // Each instrumentation strategy worker function will do its own job.
  TRACE(INSTRUMENT,
        3,
        "Loaded analysis class: %s (%s)",
        SHOW(m_options.analysis_class_name),
        SHOW(analysis_cls->get_location()));

  if (m_options.instrumentation_strategy == SIMPLE_METHOD_TRACING) {
    do_simple_method_tracing(analysis_cls, stores, cfg, pm, m_options);
  } else if (m_options.instrumentation_strategy == BASIC_BLOCK_TRACING) {
    BlockInstrumentHelper::do_basic_block_tracing(analysis_cls, stores, cfg, pm,
                                                  m_options);
  } else {
    std::cerr << "[InstrumentPass] Unknown instrumentation strategy.\n";
    exit(1);
  }
}

static InstrumentPass s_pass;
