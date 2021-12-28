/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LocalPointersAnalysis.h"

#include "DexUtil.h"
#include "PatriciaTreeSet.h"
#include "Resolver.h"
#include "Walkers.h"
#include "WorkQueue.h"

using namespace local_pointers;

namespace {

using namespace ir_analyzer;

/*
 * Whether the dest of an instruction may be a pointer value. The only time
 * there is an uncertainty as to whether the dest is a pointer or not is when
 * we have a `const 0` instruction, since that may be either a null pointer or
 * a zero integer.
 */
bool dest_may_be_pointer(const IRInstruction* insn) {
  auto op = insn->opcode();
  switch (op) {
  case OPCODE_NOP:
    not_reached_log("No dest");
  case OPCODE_MOVE:
  case OPCODE_MOVE_WIDE:
    return false;
  case OPCODE_MOVE_OBJECT:
    return true;
  case OPCODE_MOVE_RESULT:
  case OPCODE_MOVE_RESULT_WIDE:
    return false;
  case OPCODE_MOVE_RESULT_OBJECT:
  case OPCODE_MOVE_EXCEPTION:
    return true;
  case OPCODE_RETURN_VOID:
  case OPCODE_RETURN:
  case OPCODE_RETURN_WIDE:
  case OPCODE_RETURN_OBJECT:
    not_reached_log("No dest");
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT:
  case OPCODE_THROW:
  case OPCODE_GOTO:
    not_reached_log("No dest");
  case OPCODE_NEG_INT:
  case OPCODE_NOT_INT:
  case OPCODE_NEG_LONG:
  case OPCODE_NOT_LONG:
  case OPCODE_NEG_FLOAT:
  case OPCODE_NEG_DOUBLE:
  case OPCODE_INT_TO_LONG:
  case OPCODE_INT_TO_FLOAT:
  case OPCODE_INT_TO_DOUBLE:
  case OPCODE_LONG_TO_INT:
  case OPCODE_LONG_TO_FLOAT:
  case OPCODE_LONG_TO_DOUBLE:
  case OPCODE_FLOAT_TO_INT:
  case OPCODE_FLOAT_TO_LONG:
  case OPCODE_FLOAT_TO_DOUBLE:
  case OPCODE_DOUBLE_TO_INT:
  case OPCODE_DOUBLE_TO_LONG:
  case OPCODE_DOUBLE_TO_FLOAT:
  case OPCODE_INT_TO_BYTE:
  case OPCODE_INT_TO_CHAR:
  case OPCODE_INT_TO_SHORT:
  case OPCODE_ARRAY_LENGTH:
  case OPCODE_CMPL_FLOAT:
  case OPCODE_CMPG_FLOAT:
  case OPCODE_CMPL_DOUBLE:
  case OPCODE_CMPG_DOUBLE:
  case OPCODE_CMP_LONG:
    return false;
  case OPCODE_IF_EQ:
  case OPCODE_IF_NE:
  case OPCODE_IF_LT:
  case OPCODE_IF_GE:
  case OPCODE_IF_GT:
  case OPCODE_IF_LE:
  case OPCODE_IF_EQZ:
  case OPCODE_IF_NEZ:
  case OPCODE_IF_LTZ:
  case OPCODE_IF_GEZ:
  case OPCODE_IF_GTZ:
  case OPCODE_IF_LEZ:
    not_reached_log("No dest");
  case OPCODE_AGET:
  case OPCODE_AGET_WIDE:
    return false;
  case OPCODE_AGET_OBJECT:
    return true;
  case OPCODE_AGET_BOOLEAN:
  case OPCODE_AGET_BYTE:
  case OPCODE_AGET_CHAR:
  case OPCODE_AGET_SHORT:
    return false;
  case OPCODE_APUT:
  case OPCODE_APUT_WIDE:
  case OPCODE_APUT_OBJECT:
  case OPCODE_APUT_BOOLEAN:
  case OPCODE_APUT_BYTE:
  case OPCODE_APUT_CHAR:
  case OPCODE_APUT_SHORT:
    not_reached_log("No dest");
  case OPCODE_ADD_INT:
  case OPCODE_SUB_INT:
  case OPCODE_MUL_INT:
  case OPCODE_DIV_INT:
  case OPCODE_REM_INT:
  case OPCODE_AND_INT:
  case OPCODE_OR_INT:
  case OPCODE_XOR_INT:
  case OPCODE_SHL_INT:
  case OPCODE_SHR_INT:
  case OPCODE_USHR_INT:
  case OPCODE_ADD_LONG:
  case OPCODE_SUB_LONG:
  case OPCODE_MUL_LONG:
  case OPCODE_DIV_LONG:
  case OPCODE_REM_LONG:
  case OPCODE_AND_LONG:
  case OPCODE_OR_LONG:
  case OPCODE_XOR_LONG:
  case OPCODE_SHL_LONG:
  case OPCODE_SHR_LONG:
  case OPCODE_USHR_LONG:
  case OPCODE_ADD_FLOAT:
  case OPCODE_SUB_FLOAT:
  case OPCODE_MUL_FLOAT:
  case OPCODE_DIV_FLOAT:
  case OPCODE_REM_FLOAT:
  case OPCODE_ADD_DOUBLE:
  case OPCODE_SUB_DOUBLE:
  case OPCODE_MUL_DOUBLE:
  case OPCODE_DIV_DOUBLE:
  case OPCODE_REM_DOUBLE:
  case OPCODE_ADD_INT_LIT16:
  case OPCODE_RSUB_INT:
  case OPCODE_MUL_INT_LIT16:
  case OPCODE_DIV_INT_LIT16:
  case OPCODE_REM_INT_LIT16:
  case OPCODE_AND_INT_LIT16:
  case OPCODE_OR_INT_LIT16:
  case OPCODE_XOR_INT_LIT16:
  case OPCODE_ADD_INT_LIT8:
  case OPCODE_RSUB_INT_LIT8:
  case OPCODE_MUL_INT_LIT8:
  case OPCODE_DIV_INT_LIT8:
  case OPCODE_REM_INT_LIT8:
  case OPCODE_AND_INT_LIT8:
  case OPCODE_OR_INT_LIT8:
  case OPCODE_XOR_INT_LIT8:
  case OPCODE_SHL_INT_LIT8:
  case OPCODE_SHR_INT_LIT8:
  case OPCODE_USHR_INT_LIT8:
    return false;
  case OPCODE_CONST:
    return insn->get_literal() == 0;
  case OPCODE_FILL_ARRAY_DATA:
  case OPCODE_SWITCH:
    not_reached_log("No dest");
  case OPCODE_CONST_WIDE:
  case OPCODE_IGET:
  case OPCODE_IGET_WIDE:
    return false;
  case OPCODE_IGET_OBJECT:
    return true;
  case OPCODE_IGET_BOOLEAN:
  case OPCODE_IGET_BYTE:
  case OPCODE_IGET_CHAR:
  case OPCODE_IGET_SHORT:
    return false;
  case OPCODE_IPUT:
  case OPCODE_IPUT_WIDE:
  case OPCODE_IPUT_OBJECT:
  case OPCODE_IPUT_BOOLEAN:
  case OPCODE_IPUT_BYTE:
  case OPCODE_IPUT_CHAR:
  case OPCODE_IPUT_SHORT:
    not_reached_log("No dest");
  case OPCODE_SGET:
  case OPCODE_SGET_WIDE:
    return false;
  case OPCODE_SGET_OBJECT:
    return true;
  case OPCODE_SGET_BOOLEAN:
  case OPCODE_SGET_BYTE:
  case OPCODE_SGET_CHAR:
  case OPCODE_SGET_SHORT:
    return false;
  case OPCODE_SPUT:
  case OPCODE_SPUT_WIDE:
  case OPCODE_SPUT_OBJECT:
  case OPCODE_SPUT_BOOLEAN:
  case OPCODE_SPUT_BYTE:
  case OPCODE_SPUT_CHAR:
  case OPCODE_SPUT_SHORT:
    not_reached_log("No dest");
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_INTERFACE:
    return !type::is_primitive(insn->get_method()->get_proto()->get_rtype());
  case OPCODE_CONST_STRING:
  case OPCODE_CONST_CLASS:
  case OPCODE_CHECK_CAST:
    return true;
  case OPCODE_INSTANCE_OF:
    return false;
  case OPCODE_NEW_INSTANCE:
  case OPCODE_NEW_ARRAY:
  case OPCODE_FILLED_NEW_ARRAY:
    return true;
  case IOPCODE_LOAD_PARAM:
    return false;
  case IOPCODE_LOAD_PARAM_OBJECT:
    return true;
  case IOPCODE_LOAD_PARAM_WIDE:
    return false;
  case IOPCODE_MOVE_RESULT_PSEUDO:
    return false;
  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
    return true;
  case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
    return false;
  default:
    not_reached_log("Unknown opcode %02x\n", op);
  }
}

void analyze_invoke_with_summary(const EscapeSummary& summary,
                                 const IRInstruction* insn,
                                 Environment* env) {
  for (auto src_idx : summary.escaping_parameters) {
    env->set_may_escape(insn->src(src_idx), insn);
  }

  switch (summary.returned_parameters.kind()) {
  case sparta::AbstractValueKind::Value: {
    PointerSet returned_ptrs;
    for (auto src_idx : summary.returned_parameters.elements()) {
      if (src_idx == FRESH_RETURN) {
        returned_ptrs.add(insn);
      } else {
        returned_ptrs.join_with(env->get_pointers(insn->src(src_idx)));
      }
    }
    env->set_pointers(RESULT_REGISTER, returned_ptrs);
    break;
  }
  case sparta::AbstractValueKind::Top:
  case sparta::AbstractValueKind::Bottom: {
    // We are intentionally handling Bottom by setting the result register to
    // Top. This is a loss of precision but it makes it easier to implement
    // dead code elimination. See UsedVarsTest_noReturn for details.
    escape_dest(insn, RESULT_REGISTER, env);
    break;
  }
  }
}

/*
 * Analyze an invoke instruction in the absence of an available summary.
 */
void analyze_generic_invoke(const IRInstruction* insn,
                            EnvironmentWithStore* env) {
  escape_invoke_params(insn, env);
  escape_dest(insn, RESULT_REGISTER, env);
}

} // namespace

namespace local_pointers {

void escape_heap_referenced_objects(const IRInstruction* insn,
                                    EnvironmentWithStore* env) {
  auto op = insn->opcode();
  // Since we don't model instance fields / array elements, any pointers
  // written to them must be treated as escaping.
  if (op == OPCODE_APUT_OBJECT || op == OPCODE_SPUT_OBJECT ||
      op == OPCODE_IPUT_OBJECT) {
    env->set_may_escape(insn->src(0), insn);
  } else if (op == OPCODE_FILLED_NEW_ARRAY &&
             !type::is_primitive(
                 type::get_array_component_type(insn->get_type()))) {
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      env->set_may_escape(insn->src(i), insn);
    }
  }
}

void escape_dest(const IRInstruction* insn,
                 reg_t dest,
                 EnvironmentWithStore* env) {
  // While the analysis would still work if we treated all non-pointer-values
  // as escaping pointers, it would bloat the size of our abstract domain and
  // incur a runtime performance tax.
  if (dest_may_be_pointer(insn)) {
    env->set_may_escape_pointer(dest, insn, insn);
  } else {
    env->set_pointers(dest, PointerSet::top());
  }
}

void escape_invoke_params(const IRInstruction* insn,
                          EnvironmentWithStore* env) {
  size_t idx{0};
  if (insn->opcode() != OPCODE_INVOKE_STATIC) {
    env->set_may_escape(insn->src(0), insn);
    ++idx;
  }
  const auto* arg_types = insn->get_method()->get_proto()->get_args();
  for (const auto* arg : *arg_types) {
    if (!type::is_primitive(arg)) {
      env->set_may_escape(insn->src(idx), insn);
    }
    ++idx;
  }
}

void default_instruction_handler(const IRInstruction* insn,
                                 EnvironmentWithStore* env) {
  auto op = insn->opcode();
  if (opcode::is_an_invoke(op)) {
    analyze_generic_invoke(insn, env);
  } else if (opcode::is_a_move(op)) {
    env->set_pointers(insn->dest(), env->get_pointers(insn->src(0)));
  } else if (op == OPCODE_CHECK_CAST) {
    env->set_pointers(RESULT_REGISTER, env->get_pointers(insn->src(0)));
  } else if (opcode::is_move_result_any(op)) {
    env->set_pointers(insn->dest(), env->get_pointers(RESULT_REGISTER));
  } else if (insn->has_dest()) {
    escape_dest(insn, insn->dest(), env);
  } else if (insn->has_move_result_any()) {
    escape_dest(insn, RESULT_REGISTER, env);
  }
}

void FixpointIterator::analyze_instruction(const IRInstruction* insn,
                                           Environment* env) const {
  escape_heap_referenced_objects(insn, env);

  auto op = insn->opcode();
  if (opcode::is_an_invoke(op)) {
    if (m_invoke_to_summary_map.count(insn)) {
      const auto& summary = m_invoke_to_summary_map.at(insn);
      analyze_invoke_with_summary(summary, insn, env);
    } else {
      default_instruction_handler(insn, env);
    }
  } else if (may_alloc(op)) {
    env->set_fresh_pointer(RESULT_REGISTER, insn);
  } else if (op == IOPCODE_LOAD_PARAM_OBJECT) {
    env->set_fresh_pointer(insn->dest(), insn);
  } else {
    default_instruction_handler(insn, env);
    if (m_escape_check_cast && op == OPCODE_CHECK_CAST) {
      env->set_may_escape(insn->src(0), insn);
    }
  }
}

void FixpointIteratorMapDeleter::operator()(FixpointIteratorMap* map) {
  // Deletion is actually really expensive due to the reference counts of the
  // shared_ptrs in the Patricia trees, so we do it in parallel.
  auto wq = workqueue_foreach<FixpointIterator*>(
      [](FixpointIterator* fp_iter) { delete fp_iter; });
  for (auto& pair : *map) {
    wq.add_item(pair.second);
  }
  wq.run_all();
}

static void analyze_method_recursive(
    const DexMethod* method,
    const call_graph::Graph& call_graph,
    sparta::PatriciaTreeSet<const DexMethodRef*> visiting,
    FixpointIteratorMap* fp_iter_map,
    SummaryCMap* summary_map) {
  if (!method || summary_map->count(method) != 0 || visiting.contains(method) ||
      method->get_code() == nullptr) {
    return;
  }
  visiting.insert(method);

  std::unordered_map<const IRInstruction*, EscapeSummary> invoke_to_summary_map;
  if (call_graph.has_node(method)) {
    const auto& callee_edges = call_graph.node(method)->callees();
    for (const auto& edge : callee_edges) {
      auto* callee = edge->callee()->method();
      analyze_method_recursive(callee, call_graph, visiting, fp_iter_map,
                               summary_map);
      if (summary_map->count(callee) != 0) {
        invoke_to_summary_map.emplace(edge->invoke_insn(),
                                      summary_map->at(callee));
      }
    }
  }

  auto* code = method->get_code();
  auto& cfg = code->cfg();
  auto fp_iter = new FixpointIterator(cfg, std::move(invoke_to_summary_map));
  fp_iter->run(Environment());

  // The following updates form a critical section.
  {
    boost::lock_guard<boost::mutex> lock(fp_iter_map->get_lock(method));
    fp_iter_map->update_unsafe(method,
                               [&](auto, FixpointIterator*& v, bool exists) {
                                 redex_assert(!(exists ^ (v != nullptr)));
                                 delete v;
                                 v = fp_iter;
                               });
    summary_map->update(method, [&](auto, EscapeSummary& v, bool) {
      v = get_escape_summary(*fp_iter, *code);
    });
  }
}

FixpointIteratorMapPtr analyze_scope(const Scope& scope,
                                     const call_graph::Graph& call_graph,
                                     SummaryCMap* summary_map_ptr) {
  FixpointIteratorMapPtr fp_iter_map(new FixpointIteratorMap());
  SummaryCMap summary_map;
  if (summary_map_ptr == nullptr) {
    summary_map_ptr = &summary_map;
  }
  summary_map_ptr->emplace(
      DexMethod::get_method("Ljava/lang/Object;.<init>:()V"), EscapeSummary{});

  walk::parallel::code(scope, [&](const DexMethod* method, IRCode& code) {
    sparta::PatriciaTreeSet<const DexMethodRef*> visiting;
    analyze_method_recursive(method, call_graph, visiting, fp_iter_map.get(),
                             summary_map_ptr);
  });
  return fp_iter_map;
}

void collect_exiting_pointers(const FixpointIterator& fp_iter,
                              const IRCode& code,
                              PointerSet* returned_ptrs,
                              PointerSet* thrown_ptrs) {
  auto& cfg = code.cfg();
  PointerSet rv = PointerSet::bottom();
  returned_ptrs->set_to_bottom();
  thrown_ptrs->set_to_bottom();
  for (auto* block : cfg.blocks()) {
    auto last_insn_it = block->get_last_insn();
    if (last_insn_it == block->end()) {
      continue;
    }
    auto insn = last_insn_it->insn;
    const auto& state =
        fp_iter.get_exit_state_at(const_cast<cfg::Block*>(block));
    if (opcode::is_a_return_value(insn->opcode())) {
      returned_ptrs->join_with(state.get_pointers(insn->src(0)));
    } else if (insn->opcode() == OPCODE_THROW) {
      thrown_ptrs->join_with(state.get_pointers(insn->src(0)));
    }
  }
}

EscapeSummary get_escape_summary(const FixpointIterator& fp_iter,
                                 const IRCode& code) {
  EscapeSummary summary;

  PointerSet returned_ptrs;
  PointerSet thrown_ptrs;
  collect_exiting_pointers(fp_iter, code, &returned_ptrs, &thrown_ptrs);

  auto& cfg = code.cfg();
  // FIXME: fix cfg's GraphInterface so this const_cast isn't necessary
  const auto& exit_state =
      fp_iter.get_exit_state_at(const_cast<cfg::Block*>(cfg.exit_block()));
  uint16_t idx{0};
  std::unordered_map<const IRInstruction*, uint16_t> param_indexes;
  for (auto& mie : InstructionIterable(code.get_param_instructions())) {
    auto* insn = mie.insn;
    if (insn->opcode() == IOPCODE_LOAD_PARAM_OBJECT) {
      param_indexes.emplace(insn, idx);

      // Unlike returned pointers, we don't model thrown pointers specially in
      // our EscapeSummary; they are treated as escaping pointers.
      if (exit_state.may_have_escaped(insn) || thrown_ptrs.contains(insn)) {
        summary.escaping_parameters.emplace(idx);
      }
    }
    ++idx;
  }

  switch (returned_ptrs.kind()) {
  case sparta::AbstractValueKind::Value: {
    for (auto insn : returned_ptrs.elements()) {
      if (insn->opcode() == IOPCODE_LOAD_PARAM_OBJECT) {
        summary.returned_parameters.add(param_indexes.at(insn));
      } else if (!exit_state.may_have_escaped(insn)) {
        summary.returned_parameters.add(FRESH_RETURN);
      } else {
        // We are returning a pointer that did not originate from an input
        // parameter. We have no way of representing these values in our
        // summary, hence we set the return value to Top.
        summary.returned_parameters.set_to_top();
        break;
      }
    }
    break;
  }
  case sparta::AbstractValueKind::Top: {
    summary.returned_parameters.set_to_top();
    break;
  }
  case sparta::AbstractValueKind::Bottom: {
    summary.returned_parameters.set_to_bottom();
    break;
  }
  }
  return summary;
}

std::ostream& operator<<(std::ostream& o, const EscapeSummary& summary) {
  o << "Escaping parameters: ";
  bool first{true};
  for (auto p_idx : summary.escaping_parameters) {
    if (!first) {
      o << ", ";
    }
    o << p_idx;
    first = false;
  }
  o << " Returned parameters: " << summary.returned_parameters;
  return o;
}

sparta::s_expr to_s_expr(const EscapeSummary& summary) {
  std::vector<sparta::s_expr> escaping_params_s_exprs;
  std::vector<uint16_t> escaping_parameters(summary.escaping_parameters.begin(),
                                            summary.escaping_parameters.end());
  // Sort in order that the output is deterministic.
  std::sort(escaping_parameters.begin(), escaping_parameters.end());
  escaping_params_s_exprs.reserve(escaping_parameters.size());
  for (auto idx : escaping_parameters) {
    escaping_params_s_exprs.emplace_back(idx);
  }
  sparta::s_expr returned_params_s_expr;
  switch (summary.returned_parameters.kind()) {
  case sparta::AbstractValueKind::Top:
    returned_params_s_expr = sparta::s_expr("Top");
    break;
  case sparta::AbstractValueKind::Bottom:
    returned_params_s_expr = sparta::s_expr("Bottom");
    break;
  case sparta::AbstractValueKind::Value: {
    std::vector<sparta::s_expr> idx_s_exprs;
    const auto& elems = summary.returned_parameters.elements();
    std::vector<uint16_t> returned_parameters(elems.begin(), elems.end());
    std::sort(returned_parameters.begin(), returned_parameters.end());
    idx_s_exprs.reserve(returned_parameters.size());
    for (auto idx : returned_parameters) {
      idx_s_exprs.emplace_back(idx);
    }
    returned_params_s_expr = sparta::s_expr(idx_s_exprs);
    break;
  }
  }
  return sparta::s_expr{sparta::s_expr(escaping_params_s_exprs),
                        returned_params_s_expr};
}

EscapeSummary EscapeSummary::from_s_expr(const sparta::s_expr& expr) {
  EscapeSummary summary;
  always_assert(expr.is_list() && expr.size() == 2);
  auto escaping_params_s_expr = expr[0];
  always_assert(escaping_params_s_expr.is_list());
  for (size_t i = 0; i < escaping_params_s_expr.size(); ++i) {
    summary.escaping_parameters.emplace(escaping_params_s_expr[i].get_int32());
  }
  auto returned_params_s_expr = expr[1];
  if (returned_params_s_expr.is_string()) {
    const auto& s = returned_params_s_expr.get_string();
    if (s == "Top") {
      summary.returned_parameters.set_to_top();
    } else {
      redex_assert(s == "Bottom");
      summary.returned_parameters.set_to_bottom();
    }
  } else {
    always_assert(returned_params_s_expr.is_list());
    for (size_t i = 0; i < returned_params_s_expr.size(); ++i) {
      summary.returned_parameters.add(returned_params_s_expr[i].get_int32());
    }
  }
  return summary;
}

} // namespace local_pointers
