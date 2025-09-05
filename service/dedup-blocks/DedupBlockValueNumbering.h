/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "Liveness.h"

namespace DedupBlkValueNumbering {
using value_id_t = uint64_t;

const IROpcode IOPCODE_LOAD_REG = IROpcode(0xFFFF);
const IROpcode IOPCODE_OPERATION_RESULT = IROpcode(0xFFFE);

// Uses Hashvalues based on value number for Basic Block equivalence to consider
// they are equal for Basic Block Deduplication. Blocks will be considered equal
// based on the actual operations and values rather than the names of the
// registers. Therefore it can work with different registers with same values.
//
// For example, followong blocks will be considered equival:
// OPCODE: MOVE_EXCEPTION v17
// OPCODE: MONITOR_EXIT v21
// OPCODE: THROW v17
// and
// OPCODE: MOVE_EXCEPTION v6
// OPCODE: MONITOR_EXIT v21
// OPCODE: THROW v6
//
// BlockValue hash is computed based on: 1) Instruction sequence in the
// block 2) If the instruction is ordered_operations (See
// is_ordered_operation), they maintain position and type in the sequence. 3)
// For, other instructions consider the operands, compute the value number but
// only consider live-out register's values.
//
// TODO There are elements of this is used in InstructionSequenceOutliner and
// CommonSubexpressionElimination that can be consolidated into single reusable
// implementation.

struct IROperationSourceBlock {
  uint32_t src_blk_id;
  const DexString* src_blk_name;
};

inline std::size_t hash_value(IROperationSourceBlock const& sb) {
  size_t hash = sb.src_blk_id;
  boost::hash_combine(hash, sb.src_blk_name);
  return hash;
}

inline bool operator==(const IROperationSourceBlock& a,
                       const IROperationSourceBlock& b) {
  return a.src_blk_id == b.src_blk_id && a.src_blk_name == b.src_blk_name;
}

struct IROperation {
  IROpcode opcode{0};
  std::vector<value_id_t> srcs;
  union {
    // Zero-initialize this union with the struct member instead of a
    // any other member since it will always be the largest
    IROperationSourceBlock src_blk;
    uint64_t literal;
    const DexString* string;
    const DexType* type;
    const DexFieldRef* field;
    const DexMethodRef* method;
    const DexOpcodeData* data;
    reg_t in_reg;
    size_t operation_index;
  };
  IROperation() { memset(&src_blk, 0, sizeof(src_blk)); }
};

struct IROperationHasher {
  size_t operator()(const IROperation& tv) const {
    size_t hash = tv.opcode;
    boost::hash_combine(hash, tv.srcs);
    boost::hash_combine(hash, (size_t)tv.literal);
    boost::hash_combine(hash, tv.src_blk);
    return hash;
  }
};

inline bool operator==(const IROperation& a, const IROperation& b) {
  return a.opcode == b.opcode && a.srcs == b.srcs && a.literal == b.literal &&
         a.src_blk == b.src_blk;
}

struct BlockValue {
  std::vector<IROperation> ordered_operations;
  std::map<reg_t, value_id_t> out_regs;
};

struct BlockValueHasher {
  size_t operator()(const BlockValue& o) const {
    size_t hash = o.ordered_operations.size();
    for (const auto& operation : o.ordered_operations) {
      boost::hash_combine(hash, IROperationHasher()(operation));
    }
    boost::hash_combine(hash, o.out_regs);
    return hash;
  }
};

inline bool operator==(const BlockValue& a, const BlockValue& b) {
  return a.ordered_operations == b.ordered_operations &&
         a.out_regs == b.out_regs;
}

class BlockValues {
 public:
  explicit BlockValues(LivenessFixpointIterator& liveness_fixpoint_iter)
      : m_liveness_fixpoint_iter(liveness_fixpoint_iter) {}

  const BlockValue* get_block_value(cfg::Block* block) const;

 private:
  value_id_t prepare_and_get_reg(std::map<reg_t, value_id_t>& regs,
                                 reg_t reg) const;

  IROperation get_operation(std::map<reg_t, value_id_t>& regs,
                            const IRInstruction* insn) const;

  bool is_ordered_operation(const IROperation& operation) const;
  value_id_t get_value_id(const IROperation& operation) const;

  LivenessFixpointIterator& m_liveness_fixpoint_iter;
  mutable UnorderedMap<const cfg::Block*, std::unique_ptr<BlockValue>>
      m_block_values;
  mutable UnorderedMap<IROperation, value_id_t, IROperationHasher> m_value_ids;
};
} // namespace DedupBlkValueNumbering
