/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IRAssembler.h"

#include <boost/functional/hash.hpp>
#include <boost/optional/optional.hpp>
#include <sstream>
#include <string>
#include <unordered_map>

using namespace sparta;

namespace {

#define OP(OP, KIND, STR) {OPCODE_##OP, STR},
std::unordered_map<IROpcode, std::string, boost::hash<IROpcode>>
    opcode_to_string_table = {
        OPS
        {IOPCODE_LOAD_PARAM, "load-param"},
        {IOPCODE_LOAD_PARAM_OBJECT, "load-param-object"},
        {IOPCODE_LOAD_PARAM_WIDE, "load-param-wide"},
        {IOPCODE_MOVE_RESULT_PSEUDO, "move-result-pseudo"},
        {IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, "move-result-pseudo-object"},
        {IOPCODE_MOVE_RESULT_PSEUDO_WIDE, "move-result-pseudo-wide"},
};
#undef OP

#define OP(OP, KIND, STR) {STR, OPCODE_##OP},
std::unordered_map<std::string, IROpcode> string_to_opcode_table = {
    OPS
    {"load-param", IOPCODE_LOAD_PARAM},
    {"load-param-object", IOPCODE_LOAD_PARAM_OBJECT},
    {"load-param-wide", IOPCODE_LOAD_PARAM_WIDE},
    {"move-result-pseudo", IOPCODE_MOVE_RESULT_PSEUDO},
    {"move-result-pseudo-object", IOPCODE_MOVE_RESULT_PSEUDO_OBJECT},
    {"move-result-pseudo-wide", IOPCODE_MOVE_RESULT_PSEUDO_WIDE},
};
#undef OP

using LabelDefs = std::unordered_map<std::string, MethodItemEntry*>;
using LabelRefs =
    std::unordered_map<const IRInstruction*, std::vector<std::string>>;

uint16_t reg_from_str(const std::string& reg_str) {
  always_assert(reg_str.at(0) == 'v');
  uint16_t reg;
  sscanf(&reg_str.c_str()[1], "%hu", &reg);
  return reg;
}

std::string reg_to_str(uint16_t reg) { return "v" + std::to_string(reg); }

s_expr to_s_expr(const IRInstruction* insn, const LabelRefs& label_refs) {
  auto op = insn->opcode();
  auto opcode_str = opcode_to_string_table.at(op);
  std::vector<s_expr> s_exprs{s_expr(opcode_str)};
  if (insn->dests_size()) {
    s_exprs.emplace_back(reg_to_str(insn->dest()));
  }
  if (opcode::has_variable_srcs_size(op)) {
    std::vector<s_expr> src_s_exprs;
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      src_s_exprs.emplace_back(reg_to_str(insn->src(i)));
    }
    s_exprs.emplace_back(src_s_exprs);
  } else {
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      s_exprs.emplace_back(reg_to_str(insn->src(i)));
    }
  }
  switch (opcode::ref(op)) {
  case opcode::Ref::None:
    break;
  case opcode::Ref::Data:
    always_assert_log(false, "Not yet supported");
    break;
  case opcode::Ref::Field:
    s_exprs.emplace_back(show(insn->get_field()));
    break;
  case opcode::Ref::Method:
    s_exprs.emplace_back(show(insn->get_method()));
    break;
  case opcode::Ref::String:
    s_exprs.emplace_back(insn->get_string()->str());
    break;
  case opcode::Ref::Literal:
    s_exprs.emplace_back(std::to_string(insn->get_literal()));
    break;
  case opcode::Ref::Type:
    s_exprs.emplace_back(insn->get_type()->get_name()->str());
    break;
  }

  if (is_branch(op)) {
    const auto& label_strs = label_refs.at(insn);
    if (is_switch(op)) {
      // (switch v0 (:a :b :c))
      std::vector<s_expr> label_exprs;
      for (const auto& label_str : label_strs) {
        label_exprs.emplace_back(label_str);
      }
      s_exprs.emplace_back(label_exprs);
    } else {
      // (if-eqz v0 :a)
      always_assert(label_strs.size() == 1);
      s_exprs.emplace_back(label_strs[0]);
    }
  }
  return s_expr(s_exprs);
}

namespace {

std::string get_dbg_label(uint32_t i) { return ("dbg_" + std::to_string(i)); }

s_expr _to_s_expr(const DexPosition* pos, uint32_t idx, uint32_t parent_idx) {
  auto idx_str = get_dbg_label(idx);
  auto parent_idx_str = get_dbg_label(parent_idx);
  return s_expr({
      s_expr(".pos:" + idx_str),
      s_expr(show(pos->method)),
      s_expr(pos->file->c_str()),
      s_expr(std::to_string(pos->line)),
      s_expr(parent_idx_str),
  });
}
}

std::vector<s_expr> to_s_exprs(
    const DexPosition* pos,
    std::vector<const DexPosition*>* positions_emitted) {
  if (pos->parent) {
    // Get it? snay is redex's dad
    auto snay = pos->parent;
    for (size_t i = 0; i < positions_emitted->size(); i++) {
      auto pos_emitted = positions_emitted->at(i);
      if (*pos_emitted == *snay) {
        // Shane thought he could hide from us... hah! a quick linear search
        // got him
        positions_emitted->push_back(pos);
        return {_to_s_expr(pos, positions_emitted->size() - 1, i)};
      }
    }
    auto result = to_s_exprs(snay, positions_emitted);
    always_assert(positions_emitted->size() > 0);
    auto parent_idx = positions_emitted->size() - 1;
    positions_emitted->push_back(pos);
    auto pos_idx = positions_emitted->size() - 1;
    result.push_back(_to_s_expr(pos, pos_idx, parent_idx));
    return result;
  } else {
    positions_emitted->push_back(pos);
    auto idx_str = get_dbg_label(positions_emitted->size() - 1);
    return {s_expr({
        s_expr(".pos:" + idx_str),
        s_expr(show(pos->method)),
        s_expr(pos->file->c_str()),
        s_expr(std::to_string(pos->line)),
    })};
  }
}

std::unique_ptr<IRInstruction> instruction_from_s_expr(
    const std::string& opcode_str, const s_expr& e, LabelRefs* label_refs) {
  auto op_it = string_to_opcode_table.find(opcode_str);
  always_assert_log(op_it != string_to_opcode_table.end(),
                    "'%s' is not a valid opcode",
                    opcode_str.c_str());
  auto op = op_it->second;
  auto insn = std::make_unique<IRInstruction>(op);
  std::string reg_str;
  s_expr tail = e;
  if (insn->dests_size()) {
    s_patn({s_patn(&reg_str)}, tail)
        .must_match(tail, "Expected dest reg for " + opcode_str);
    insn->set_dest(reg_from_str(reg_str));
  }
  if (opcode::has_variable_srcs_size(op)) {
    auto srcs = tail[0];
    tail = tail.tail(1);
    insn->set_arg_word_count(srcs.size());
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      insn->set_src(i, reg_from_str(srcs[i].get_string()));
    }
  } else {
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      s_patn({s_patn(&reg_str)}, tail)
          .must_match(tail, "Expected src reg for" + opcode_str);
      insn->set_src(i, reg_from_str(reg_str));
    }
  }
  switch (opcode::ref(op)) {
  case opcode::Ref::None:
    break;
  case opcode::Ref::Data:
    always_assert_log(false, "Not yet supported");
    break;
  case opcode::Ref::Field: {
    std::string str;
    s_patn({s_patn(&str)}, tail)
        .must_match(tail, "Expecting string literal for " + opcode_str);
    auto* dex_field = DexField::make_field(str);
    insn->set_field(dex_field);
    break;
  }
  case opcode::Ref::Method: {
    std::string str;
    s_patn({s_patn(&str)}, tail)
        .must_match(tail, "Expecting string literal for " + opcode_str);
    auto* dex_method = DexMethod::make_method(str);
    insn->set_method(dex_method);
    break;
  }
  case opcode::Ref::String: {
    std::string str;
    s_patn({s_patn(&str)}, tail)
        .must_match(tail, "Expecting string literal for " + opcode_str);
    auto* dex_str = DexString::make_string(str);
    insn->set_string(dex_str);
    break;
  }
  case opcode::Ref::Literal: {
    std::string num_str;
    s_patn({s_patn(&num_str)}, tail)
        .must_match(tail, "Expecting numeric literal for " + opcode_str);
    int64_t num;
    std::istringstream in(num_str);
    in >> num;
    insn->set_literal(num);
    break;
  }
  case opcode::Ref::Type: {
    std::string type_str;
    s_patn({s_patn(&type_str)}, tail)
        .must_match(tail, "Expecting type specifier for " + opcode_str);
    DexType* ty = DexType::make_type(type_str.c_str());
    insn->set_type(ty);
    break;
  }
  }

  if (is_branch(op)) {
    std::string label_str;
    if (is_switch(op)) {
      s_expr list;
      s_patn({s_patn(list)}, tail)
          .must_match(tail, "Expecting list of labels for " + opcode_str);
      while (s_patn({s_patn(&label_str)}, list).match_with(list)) {
        (*label_refs)[insn.get()].push_back(label_str);
      }
    } else {
      s_patn({s_patn(&label_str)}, tail)
          .must_match(tail, "Expecting label for " + opcode_str);
      (*label_refs)[insn.get()].push_back(label_str);
    }
  }

  always_assert_log(tail.is_nil(),
                    "Found unexpected trailing items when parsing %s: %s",
                    opcode_str.c_str(),
                    tail.str().c_str());
  return insn;
}

std::unique_ptr<DexPosition> position_from_s_expr(
    const s_expr& e,
    const std::unordered_map<std::string, DexPosition*>& positions) {
  std::string method_str;
  std::string file_str;
  std::string line_str;
  s_expr parent_expr;
  s_patn({
      s_patn(&method_str),
      s_patn(&file_str),
      s_patn(&line_str),
  }, parent_expr).must_match(e, "Expected 3 or 4 args for position directive");
  auto* file = DexString::make_string(file_str);
  uint32_t line;
  std::istringstream in(line_str);
  in >> line;
  auto pos = std::make_unique<DexPosition>(line);
  pos->bind(DexString::make_string(method_str), file);
  if (!parent_expr.is_nil()) {
    std::string parent_str;
    s_patn({
               s_patn(&parent_str),
           })
        .must_match(parent_expr,
                    "Expected 4th arg of pos directive to be a string");

    // Try and find parent matching parent_str at end of expr
    auto iter = positions.find(parent_str);
    if (iter != positions.end()) {
      pos->parent = iter->second;
    } else {
      pos->parent = nullptr;
      fprintf(stderr,
              "Failed to find parent position with label %s\n",
              parent_str.c_str());
    }
  } else {
    pos->parent = nullptr;
  }
  return pos;
}

/*
 * Connect label defs to label refs via creation of MFLOW_TARGET instances
 */
void handle_labels(IRCode* code,
                   const LabelDefs& label_defs,
                   const LabelRefs label_refs) {
  for (auto& mie : InstructionIterable(code)) {
    auto* insn = mie.insn;
    if (label_refs.count(insn)) {
      for (const std::string& label : label_refs.at(insn)) {
        auto target_mie = label_defs.at(label);
        // Since one label can be the target of multiple branches, but one
        // MFLOW_TARGET can only point to one branching opcode, we may need to
        // create additional MFLOW_TARGET items here.
        always_assert(target_mie->type == MFLOW_TARGET);
        BranchTarget* target = target_mie->target;
        if (target->src == nullptr) {
          target->src = &mie;
        } else {
          // First target already filled. Create another
          BranchTarget* new_target =
              (target->type == BRANCH_SIMPLE)
                  ? new BranchTarget(&mie)
                  : new BranchTarget(&mie, target->case_key);
          auto new_target_mie = new MethodItemEntry(new_target);
          code->insert_before(code->iterator_to(*target_mie), *new_target_mie);
        }
      }
    }
  }

  // Clean up any unreferenced labels
  for (auto& mie : *code) {
    if (mie.type == MFLOW_TARGET && mie.target->src == nullptr) {
      delete mie.target;
      mie.type = MFLOW_FALLTHROUGH;
    }
  }
}

std::unordered_map<std::string, MethodItemEntry*> get_catch_name_map(
    const s_expr& insns) {
  std::unordered_map<std::string, MethodItemEntry*> result;
  for (size_t i = 0; i < insns.size(); ++i) {
    std::string keyword;
    s_expr tail;
    if (s_patn({s_patn(&keyword)}, tail).match_with(insns[i])) {
      if (keyword == ".catch") {
        // Catch markers look like this:
        // (.catch (this next) "LCatchType;")
        // where next and "LCatchType;" are optional
        std::string this_catch;
        s_expr maybe_next, type_expr;
        s_patn({s_patn({s_patn(&this_catch)}, maybe_next)}, type_expr)
            .must_match(tail, "catch marker missing a name list");
        // FIXME?
        result.emplace(
            this_catch,
            new MethodItemEntry(static_cast<DexType*>(nullptr)));
      }
    }
  }
  return result;
}

// Can we merge this target into the same label as the previous target?
bool can_merge(IRList::const_iterator prev, IRList::const_iterator it) {
  always_assert(it->type == MFLOW_TARGET);
  return prev->type == MFLOW_TARGET &&
         // can't merge if/goto targets with switch targets
         it->target->type == prev->target->type &&
         // if/goto targets only need to be adjacent in the instruction stream
         // to be merged into a single label
         (it->target->type == BRANCH_SIMPLE ||
          // switch targets also need matching case keys
          it->target->case_key == prev->target->case_key);
}

s_expr create_try_expr(TryEntryType type, const std::string& catch_name) {
  // (.try_start name) and (.try_end name)
  const std::string& type_str = type == TRY_START ? ".try_start" : ".try_end";
  return s_expr({s_expr(type_str), s_expr(catch_name)});
}

s_expr create_catch_expr(const MethodItemEntry* mie,
                         const std::unordered_map<const MethodItemEntry*,
                                                  std::string>& catch_names) {
  // (.catch (this_name next_name) "LCatchType;")
  // where next_name and the "LCatchType;" are optional
  std::vector<s_expr> catch_name_exprs;
  catch_name_exprs.emplace_back(catch_names.at(mie));
  if (mie->centry->next != nullptr) {
    catch_name_exprs.emplace_back(catch_names.at(mie->centry->next));
  }
  std::vector<s_expr> result;
  result.emplace_back(".catch");
  result.emplace_back(catch_name_exprs);
  if (mie->centry->catch_type != nullptr) {
    result.emplace_back(mie->centry->catch_type->get_name()->str());
  }
  return s_expr(result);
}

} // namespace

namespace assembler {

s_expr to_s_expr(const IRCode* code) {
  std::vector<s_expr> exprs;
  LabelRefs label_refs;
  std::unordered_map<const MethodItemEntry*, std::string> catch_names;

  size_t label_ctr{0};
  auto generate_label_name = [&]() {
    return ":L" + std::to_string(label_ctr++);
  };

  size_t catch_ctr{0};
  auto generate_catch_name = [&]() {
    return "c" + std::to_string(catch_ctr++);
  };

  // Gather jump targets and give them string names
  for (auto it = code->cbegin(); it != code->cend(); ++it) {
    switch (it->type) {
      case MFLOW_TARGET: {
        auto bt = it->target;
        always_assert_log(bt->src != nullptr, "%s", SHOW(code));

        // Don't generate redundant labels. If we would duplicate the previous
        // label, steal its name instead of generating another
        if (it != code->begin()) {
          auto prev = std::prev(it);
          if (can_merge(prev, it)) {
            auto& label_strs = label_refs.at(prev->target->src->insn);
            if (label_strs.size() > 0) {
              const auto& label_name = label_strs.back();
              label_refs[bt->src->insn].push_back(label_name);
              break;
            }
          }
        }
        label_refs[bt->src->insn].push_back(generate_label_name());
        break;
      }
      case MFLOW_CATCH:
        catch_names.emplace(&*it, generate_catch_name());
        break;
      default:
        break;
    }
  }

  // Now emit the exprs
  std::unordered_map<IRInstruction*, size_t> unused_label_index;
  std::vector<const DexPosition*> positions_emitted;
  for (auto it = code->begin(); it != code->end(); ++it) {
    switch (it->type) {
      case MFLOW_OPCODE:
        exprs.emplace_back(::to_s_expr(it->insn, label_refs));
        break;
      case MFLOW_TRY:
        exprs.emplace_back(create_try_expr(
            it->tentry->type, catch_names.at(it->tentry->catch_start)));
        break;
      case MFLOW_CATCH:
        exprs.emplace_back(create_catch_expr(&*it, catch_names));
        break;
      case MFLOW_DEBUG:
        always_assert_log(false, "Not yet implemented");
      case MFLOW_POSITION:
        for (const auto& e : ::to_s_exprs(it->pos.get(), &positions_emitted)) {
          exprs.push_back(e);
        }
        break;
      case MFLOW_TARGET: {
        auto branch_target = it->target;
        auto insn = branch_target->src->insn;
        const auto& label_strs = label_refs.at(insn);

        if (branch_target->type == BRANCH_MULTI) {
          // Claim one of the labels.
          // Doesn't matter which one as long as no other s_expr re-uses it.
          auto& index = unused_label_index[insn];
          auto label_str = label_strs[index];
          ++index;

          const s_expr& label =
              s_expr({s_expr(label_str),
                      s_expr(std::to_string(branch_target->case_key))});

          // Don't duplicate labels even if some crazy person has two switches
          // that share targets :O
          if (exprs.empty() || exprs.back() != label) {
            exprs.emplace_back(label);
          }
        } else {
          always_assert(branch_target->type == BRANCH_SIMPLE);
          always_assert_log(
              label_strs.size() == 1,
              "Expecting 1 label string, actually have %d. code:\n%s",
              label_strs.size(),
              SHOW(code));
          const s_expr& label = s_expr({s_expr(label_strs[0])});

          // Two gotos to the same destination will produce two MFLOW_TARGETs
          // but we only need one label in the s expression syntax.
          if (exprs.empty() || exprs.back() != label) {
            exprs.push_back(label);
          }
        }
        break;
      }
      case MFLOW_FALLTHROUGH:
        break;
      case MFLOW_DEX_OPCODE:
        not_reached();
    }
  }

  return s_expr(exprs);
}

static boost::optional<uint16_t> largest_reg_operand(const IRInstruction* insn) {
  boost::optional<uint16_t> max_reg;
  if (insn->dests_size()) {
    max_reg = insn->dest();
  }
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    // boost::none is the smallest element of the ordering.
    // It's smaller than any uint16_t.
    max_reg = std::max(max_reg, boost::optional<uint16_t>(insn->src(i)));
  }
  return max_reg;
}

std::unique_ptr<IRCode> ircode_from_s_expr(const s_expr& e) {
  s_expr insns_expr;
  auto code = std::make_unique<IRCode>();
  always_assert(s_patn({}, insns_expr).match_with(e));
  always_assert_log(insns_expr.size() > 0, "Empty instruction list?! %s");
  LabelDefs label_defs;
  LabelRefs label_refs;
  boost::optional<uint16_t> max_reg;
  std::unordered_map<std::string, DexPosition*> positions;

  // map from catch name to catch marker pointer
  const auto& catches = get_catch_name_map(insns_expr);

  for (size_t i = 0; i < insns_expr.size(); ++i) {
    std::string keyword;
    s_expr tail;
    if (s_patn({s_patn(&keyword)}, tail).match_with(insns_expr[i])) {
      // check if keyword starts with ".pos"
      if (strncmp(keyword.c_str(), ".pos", 4) == 0) {
        auto pos = position_from_s_expr(tail, positions);
        // check if keyword also has dbg label
        if (keyword != ".pos") {
          // get dbg label found after colon in keyword string
          auto key = keyword.substr(keyword.find(":") + 1);
          // insert pos into positions map using dbg label as key
          positions[key] = pos.get();
        }
        code->push_back(std::move(pos));
      } else if (keyword.substr(0, 4) == ".try") {
        // Try markers look like this:
        // (.try_start catch_name)
        // (.try_end catch_name)
        const auto& rest = keyword.substr(4, std::string::npos);
        bool is_start = rest == "_start";
        always_assert_log(is_start ^ (rest == "_end"),
                          "try must be .try_start or .try_end");
        std::string catch_name;
        s_patn({s_patn(&catch_name)})
            .must_match(tail, "try marker is missing a name");
        always_assert(catch_name != "");
        auto try_marker = new MethodItemEntry(is_start ? TRY_START : TRY_END,
                                              catches.at(catch_name));
        code->push_back(*try_marker);

      } else if (keyword == ".catch") {
        // Catch markers look like this:
        // (.catch (this next) "LCatchType;")
        // where next and "LCatchType;" are optional
        std::string this_catch;
        std::string next_catch;
        s_expr type_expr;
        // Check for having both this and next
        if (!s_patn({s_patn({s_patn(&this_catch), s_patn(&next_catch)})},
                    type_expr)
                 .match_with(tail)) {
          // There is no next catch. Match a single name, for example: (this)
          next_catch = "";
          s_patn({s_patn({s_patn(&this_catch)})}, type_expr)
              .must_match(tail, "catch marker is missing a name");
        }
        // Get the type name, for example: "LCatchType;"
        always_assert_log(this_catch != "", "catch marker is missing a name");
        std::string type_name;
        // nullptr is a valid catch type. It means catch all exceptions.
        DexType* catch_type = nullptr;
        if (s_patn({s_patn(&type_name)}).match_with(type_expr)) {
          catch_type = DexType::make_type(DexString::make_string(type_name));
        }
        MethodItemEntry* catch_marker = catches.at(this_catch);
        catch_marker->centry->catch_type = catch_type;
        if (next_catch != "") {
          catch_marker->centry->next = catches.at(next_catch);
        }
        code->push_back(*catch_marker);

      } else if (keyword[0] == ':') {
        const auto& label = keyword;
        always_assert_log(
            label_defs.count(label) == 0, "Duplicate label %s", label.c_str());

        // We insert a MFLOW_TARGET with an empty source mie that may be filled
        // in later if something points to it
        std::string case_key_str;
        BranchTarget* bt = nullptr;
        if (s_patn({s_patn(&case_key_str)}).match_with(tail)) {
          // A switch target like (:label 0)
          bt = new BranchTarget(nullptr, std::stoi(case_key_str));
        } else {
          // An if target like (:label)
          bt = new BranchTarget(nullptr);
        }
        auto maybe_target = new MethodItemEntry(bt);
        label_defs.emplace(label, maybe_target);
        code->push_back(*maybe_target);

      } else {
        auto insn = instruction_from_s_expr(keyword, tail, &label_refs);
        always_assert(insn != nullptr);
        max_reg = std::max(max_reg, largest_reg_operand(insn.get()));
        code->push_back(insn.release());
      }
    }
  }
  handle_labels(code.get(), label_defs, label_refs);

  // FIXME: I don't think this handles wides correctly
  code->set_registers_size(max_reg ? *max_reg + 1 : 0);

  return code;
}

std::unique_ptr<IRCode> ircode_from_string(const std::string& s) {
  std::istringstream input(s);
  s_expr_istream s_expr_input(input);
  s_expr expr;
  while (s_expr_input.good()) {
    s_expr_input >> expr;
    if (s_expr_input.eoi()) {
      break;
    }
    always_assert_log(
        !s_expr_input.fail(), "%s\n", s_expr_input.what().c_str());
  }
  return ircode_from_s_expr(expr);
}

#define AF(uc, lc, val) {ACC_##uc, #lc},
std::unordered_map<DexAccessFlags, std::string, boost::hash<DexAccessFlags>>
    access_to_string_table = {ACCESSFLAGS};
#undef AF

#define AF(uc, lc, val) {#lc, ACC_##uc},
std::unordered_map<std::string, DexAccessFlags> string_to_access_table = {
    ACCESSFLAGS};
#undef AF

DexMethod* method_from_s_expr(const s_expr& e) {
  s_expr tail;
  s_patn({s_patn("method")}, tail)
      .must_match(e, "method definitions must start with 'method'");

  s_expr access_tokens;
  std::string method_name;
  s_patn({s_patn(access_tokens), s_patn(&method_name)}, tail)
      .must_match(tail, "Expecting access list and method name");

  auto method = static_cast<DexMethod*>(DexMethod::make_method(method_name));
  DexAccessFlags access_flags = static_cast<DexAccessFlags>(0);
  for (size_t i = 0; i < access_tokens.size(); ++i) {
    access_flags |= string_to_access_table.at(access_tokens[i].str());
  }

  s_expr code_expr;
  s_patn({s_patn(code_expr)}, tail).match_with(tail);
  always_assert_log(code_expr.is_list(), "Expecting code listing");
  bool is_virtual = !is_static(access_flags) && !is_private(access_flags);
  method->make_concrete(
      access_flags, ircode_from_s_expr(code_expr), is_virtual);

  return method;
}

DexMethod* method_from_string(const std::string& s) {
  std::istringstream input(s);
  s_expr_istream s_expr_input(input);
  s_expr expr;
  while (s_expr_input.good()) {
    s_expr_input >> expr;
    if (s_expr_input.eoi()) {
      break;
    }
    always_assert_log(
        !s_expr_input.fail(), "%s\n", s_expr_input.what().c_str());
  }
  return method_from_s_expr(expr);
}

} // namespace assembler
