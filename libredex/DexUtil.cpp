/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexUtil.h"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include <deque>
#include <unordered_set>

#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "EditableCfgAdapter.h"
#include "Resolver.h"
#include "Trace.h"
#include "UnknownVirtuals.h"

DexAccessFlags merge_visibility(uint32_t vis1, uint32_t vis2) {
  vis1 &= VISIBILITY_MASK;
  vis2 &= VISIBILITY_MASK;
  if ((vis1 & ACC_PUBLIC) || (vis2 & ACC_PUBLIC)) return ACC_PUBLIC;
  if (vis1 == 0 || vis2 == 0) return static_cast<DexAccessFlags>(0);
  if ((vis1 & ACC_PROTECTED) || (vis2 & ACC_PROTECTED)) return ACC_PROTECTED;
  return ACC_PRIVATE;
}

void create_runtime_exception_block(DexString* except_str,
                                    std::vector<IRInstruction*>& block) {
  // clang-format off
  // new-instance v0, Ljava/lang/RuntimeException; // type@3852
  // const-string v1, "Exception String e.g. Too many args" // string@7a6d
  // invoke-direct {v0, v1}, Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V
  // throw v0
  // clang-format on
  auto new_inst =
      (new IRInstruction(OPCODE_NEW_INSTANCE))
          ->set_type(DexType::make_type("Ljava/lang/RuntimeException;"));
  new_inst->set_dest(0);
  IRInstruction* const_inst =
      (new IRInstruction(OPCODE_CONST_STRING))->set_string(except_str);
  const_inst->set_dest(1);
  auto ret = DexType::make_type("V");
  auto arg = DexType::make_type("Ljava/lang/String;");
  auto args = DexTypeList::make_type_list({arg});
  auto proto = DexProto::make_proto(ret, args);
  auto meth =
      DexMethod::make_method(DexType::make_type("Ljava/lang/RuntimeException;"),
                             DexString::make_string("<init>"), proto);
  auto invk = new IRInstruction(OPCODE_INVOKE_DIRECT);
  invk->set_method(meth);
  invk->set_srcs_size(2);
  invk->set_src(0, 0);
  invk->set_src(1, 1);
  IRInstruction* throwinst = new IRInstruction(OPCODE_THROW);
  block.emplace_back(new_inst);
  block.emplace_back(const_inst);
  block.emplace_back(invk);
  block.emplace_back(throwinst);
}

bool passes_args_through(IRInstruction* insn,
                         const IRCode& code,
                         int ignore /* = 0 */
) {
  size_t src_idx{0};
  size_t param_count{0};
  for (const auto& mie : InstructionIterable(code.get_param_instructions())) {
    auto load_param = mie.insn;
    ++param_count;
    if (src_idx >= insn->srcs_size()) {
      continue;
    }
    if (load_param->dest() != insn->src(src_idx++)) {
      return false;
    }
  }
  return insn->srcs_size() + ignore == param_count;
}

Scope build_class_scope(const DexStoresVector& stores) {
  return build_class_scope(DexStoreClassesIterator(stores));
}

template <typename PrefixIt>
bool starts_with_any_prefix(const std::string& str,
                            const PrefixIt& begin,
                            const PrefixIt& end) {
  auto it = begin;
  while (it != end) {
    if (boost::algorithm::starts_with(str, *it)) {
      return true;
    }
    it++;
  }
  return false;
}

Scope build_class_scope_for_packages(
    const DexStoresVector& stores,
    const std::unordered_set<std::string>& package_names) {
  Scope v;
  for (auto const& store : stores) {
    for (auto& dex : store.get_dexen()) {
      for (auto& clazz : dex) {
        if (starts_with_any_prefix(clazz->get_deobfuscated_name(),
                                   package_names.begin(),
                                   package_names.end())) {
          v.push_back(clazz);
        }
      }
    }
  }
  return v;
}

void post_dexen_changes(const Scope& v, DexStoresVector& stores) {
  DexStoreClassesIterator iter(stores);
  post_dexen_changes(v, iter);
}

void load_root_dexen(DexStore& store,
                     const std::string& dexen_dir_str,
                     bool balloon,
                     bool verbose,
                     int support_dex_version) {
  namespace fs = boost::filesystem;
  fs::path dexen_dir_path(dexen_dir_str);
  redex_assert(fs::is_directory(dexen_dir_path));

  // Discover dex files
  auto end = fs::directory_iterator();
  std::vector<fs::path> dexen;
  for (fs::directory_iterator it(dexen_dir_path); it != end; ++it) {
    auto file = it->path();
    if (fs::is_regular_file(file) &&
        !file.extension().compare(std::string(".dex"))) {
      dexen.emplace_back(file);
    }
  }

  /*
   * Comparator for dexen filename. 'classes.dex' should sort first,
   * followed by [^\d]*[\d]+.dex ordered by N numerically.
   */
  auto dex_comparator = [](const fs::path& a, const fs::path& b) {
    boost::regex s_dex_regex("[^0-9]*([0-9]+)\\.dex");

    auto as = a.filename().string();
    auto bs = b.filename().string();
    boost::smatch amatch;
    boost::smatch bmatch;
    bool amatched = boost::regex_match(as, amatch, s_dex_regex);
    bool bmatched = boost::regex_match(bs, bmatch, s_dex_regex);

    if (!amatched && bmatched) {
      return true;
    } else if (amatched && !bmatched) {
      return false;
    } else if (!amatched && !bmatched) {
      // Compare strings, probably the same
      return strcmp(as.c_str(), bs.c_str()) > 0;
    } else {
      // Compare captures as integers
      auto anum = std::stoi(amatch[1]);
      auto bnum = std::stoi(bmatch[1]);
      return bnum > anum;
    }
  };

  // Sort all discovered dex files
  std::sort(dexen.begin(), dexen.end(), dex_comparator);
  // Load all discovered dex files
  for (const auto& dex : dexen) {
    if (verbose) {
      TRACE(MAIN, 1, "Loading %s", dex.string().c_str());
    }
    // N.B. throaway stats for now
    DexClasses classes = load_classes_from_dex(dex.string().c_str(), balloon,
                                               support_dex_version);
    store.add_classes(std::move(classes));
  }
}

void create_store(const std::string& store_name,
                  DexStoresVector& stores,
                  DexClasses classes) {
  // First, remove the classes from other stores.
  for (auto& store : stores) {
    store.remove_classes(classes);
  }

  // Create a new store and add it to the list of stores.
  DexStore store(store_name);
  store.set_generated();
  store.add_classes(std::move(classes));
  stores.emplace_back(std::move(store));
}

void relocate_method(DexMethod* method, DexType* to_type) {
  change_visibility(method, to_type);
  auto from_cls = type_class(method->get_class());
  auto to_cls = type_class(to_type);
  from_cls->remove_method(method);
  DexMethodSpec spec;
  spec.cls = to_type;
  method->change(spec, true /* rename on collision */);
  to_cls->add_method(method);
}

VisibilityChanges get_visibility_changes(const DexMethod* method,
                                         DexType* scope) {
  return get_visibility_changes(method->get_code(), scope, method);
}

void VisibilityChanges::insert(const VisibilityChanges& other) {
  classes.insert(other.classes.begin(), other.classes.end());
  fields.insert(other.fields.begin(), other.fields.end());
  methods.insert(other.methods.begin(), other.methods.end());
}

void VisibilityChanges::apply() const {
  for (auto cls : classes) {
    set_public(cls);
  }
  for (auto field : fields) {
    set_public(field);
  }
  for (auto method : methods) {
    set_public(method);
  }
}

bool VisibilityChanges::empty() const {
  return classes.empty() && fields.empty() && methods.empty();
}

namespace {

struct VisibilityChangeGetter {
  VisibilityChanges& changes;
  DexType* scope;
  const DexMethod* effective_caller_resolved_from;
  void process_insn(IRInstruction* insn) {
    if (insn->has_field()) {
      auto cls = type_class(insn->get_field()->get_class());
      if (cls != nullptr && !cls->is_external() && !is_public(cls)) {
        changes.classes.insert(cls);
      }
      auto field = resolve_field(insn->get_field(),
                                 opcode::is_an_sfield_op(insn->opcode())
                                     ? FieldSearch::Static
                                     : FieldSearch::Instance);
      if (field != nullptr && field->is_concrete()) {
        if (!is_public(field)) {
          changes.fields.insert(field);
        }
        cls = type_class(field->get_class());
        if (!is_public(cls)) {
          changes.classes.insert(cls);
        }
      }
    } else if (insn->has_method()) {
      auto cls = type_class(insn->get_method()->get_class());
      if (cls != nullptr && !cls->is_external() && !is_public(cls)) {
        changes.classes.insert(cls);
      }
      auto current_method =
          resolve_method(insn->get_method(), opcode_to_search(insn),
                         effective_caller_resolved_from);
      if (current_method != nullptr && current_method->is_concrete() &&
          (scope == nullptr || current_method->get_class() != scope)) {
        if (!is_public(current_method)) {
          changes.methods.insert(current_method);
        }
        cls = type_class(current_method->get_class());
        if (cls != nullptr && !cls->is_external() && !is_public(cls)) {
          changes.classes.insert(cls);
        }
      }
    } else if (insn->has_type()) {
      auto type = insn->get_type();
      auto cls = type_class(type);
      if (cls != nullptr && !cls->is_external() && !is_public(cls)) {
        changes.classes.insert(cls);
      }
    }
  }

  void process_catch_types(const std::vector<DexType*>& types) {
    for (auto type : types) {
      auto cls = type_class(type);
      if (cls != nullptr && !cls->is_external() && !is_public(cls)) {
        changes.classes.insert(cls);
      }
    }
  }
};

} // namespace

VisibilityChanges get_visibility_changes(
    const IRCode* code,
    DexType* scope,
    const DexMethod* effective_caller_resolved_from) {
  always_assert(code != nullptr);
  VisibilityChanges changes;
  VisibilityChangeGetter getter{changes, scope, effective_caller_resolved_from};
  editable_cfg_adapter::iterate(const_cast<IRCode*>(code),
                                [&getter](MethodItemEntry& mie) {
                                  getter.process_insn(mie.insn);
                                  return editable_cfg_adapter::LOOP_CONTINUE;
                                });

  std::vector<DexType*> types;
  code->gather_catch_types(types);
  getter.process_catch_types(types);
  return changes;
}

VisibilityChanges get_visibility_changes(
    const cfg::ControlFlowGraph& cfg,
    DexType* scope,
    const DexMethod* effective_caller_resolved_from) {
  VisibilityChanges changes;
  VisibilityChangeGetter getter{changes, scope, effective_caller_resolved_from};
  for (auto& mie :
       cfg::InstructionIterable(const_cast<cfg::ControlFlowGraph&>(cfg))) {
    getter.process_insn(mie.insn);
  }
  std::vector<DexType*> types;
  cfg.gather_catch_types(types);
  getter.process_catch_types(types);
  return changes;
}

// Check that visibility / accessibility changes to the current method
// won't need to change a referenced method into a virtual or static one.
bool gather_invoked_methods_that_prevent_relocation(
    const DexMethod* method,
    std::unordered_set<DexMethodRef*>* methods_preventing_relocation) {
  auto code = method->get_code();
  always_assert(code);

  bool can_relocate = true;
  for (const auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    auto opcode = insn->opcode();
    if (opcode::is_an_invoke(opcode)) {
      auto meth =
          resolve_method(insn->get_method(), opcode_to_search(insn), method);
      if (!meth && opcode == OPCODE_INVOKE_VIRTUAL &&
          unknown_virtuals::is_method_known_to_be_public(insn->get_method())) {
        continue;
      }
      if (meth) {
        always_assert(meth->is_def());
        if (meth->is_external() && !is_public(meth)) {
          meth = nullptr;
        } else if (opcode == OPCODE_INVOKE_DIRECT && !method::is_init(meth)) {
          meth = nullptr;
        }
      }
      if (!meth) {
        can_relocate = false;
        if (!methods_preventing_relocation) {
          break;
        }
        methods_preventing_relocation->emplace(insn->get_method());
      }
    }
  }

  return can_relocate;
}

bool relocate_method_if_no_changes(DexMethod* method, DexType* to_type) {
  if (!gather_invoked_methods_that_prevent_relocation(method)) {
    return false;
  }

  set_public(method);
  relocate_method(method, to_type);
  change_visibility(method);

  return true;
}

bool is_valid_identifier(const std::string& s) {
  return is_valid_identifier(s, 0, s.length());
}

bool is_valid_identifier(const std::string& s, size_t start, size_t len) {
  if (len == 0) {
    // Identifiers must not be empty.
    return false;
  }
  for (size_t i = start; i != start + len; ++i) {
    switch (s.at(i)) {
    // Forbidden characters. This may not work for UTF encodings.
    case '/':
    case ';':
    case '.':
    case '[':
      return false;
    // Allow everything else.
    default:
      break;
    }
  }
  return true;
}
