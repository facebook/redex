/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexHasher.h"

#include "DexAccess.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IROpcode.h"
#include "Show.h"
#include "Walkers.h"

namespace hashing {

std::string hash_to_string(size_t hash) {
  std::ostringstream result;
  result << std::hex << std::setfill('0') << std::setw(sizeof(size_t) * 2)
         << hash;
  return result.str();
}

DexHash DexScopeHasher::run() {
  std::unordered_map<DexClass*, size_t> class_indices;
  walk::classes(m_scope, [&](DexClass* cls) {
    class_indices.emplace(cls, class_indices.size());
  });
  std::vector<size_t> class_registers_hashes(class_indices.size());
  std::vector<size_t> class_code_hashes(class_indices.size());
  std::vector<size_t> class_signature_hashes(class_indices.size());
  walk::parallel::classes(m_scope, [&](DexClass* cls) {
    DexClassHasher class_hasher(cls);
    DexHash class_hash = class_hasher.run();
    auto index = class_indices.at(cls);
    class_registers_hashes.at(index) = class_hash.registers_hash;
    class_code_hashes.at(index) = class_hash.code_hash;
    class_signature_hashes.at(index) = class_hash.signature_hash;
  });

  return DexHash{boost::hash_value(class_registers_hashes),
                 boost::hash_value(class_code_hashes),
                 boost::hash_value(class_signature_hashes)};
}

void DexClassHasher::hash(const std::string& str) {
  TRACE(HASHER, 4, "[hasher] %s", str.c_str());
  boost::hash_combine(m_hash, str);
}

void DexClassHasher::hash(const DexString* s) { hash(s->str()); }

void DexClassHasher::hash(bool value) {
  TRACE(HASHER, 4, "[hasher] %u", value);
  boost::hash_combine(m_hash, value);
}
void DexClassHasher::hash(uint8_t value) {
  TRACE(HASHER, 4, "[hasher] %u", value);
  boost::hash_combine(m_hash, value);
}

void DexClassHasher::hash(uint16_t value) {
  TRACE(HASHER, 4, "[hasher] %u", value);
  boost::hash_combine(m_hash, value);
}

void DexClassHasher::hash(uint32_t value) {
  TRACE(HASHER, 4, "[hasher] %u", value);
  boost::hash_combine(m_hash, value);
}

void DexClassHasher::hash(uint64_t value) {
  TRACE(HASHER, 4, "[hasher] %lu", value);
  boost::hash_combine(m_hash, value);
}

void DexClassHasher::hash(int value) { hash((uint)value); }

void DexClassHasher::hash(const IRInstruction* insn) {
  hash((uint16_t)insn->opcode());

  auto old_hash = m_hash;
  m_hash = 0;
  hash(insn->srcs());
  if (insn->has_dest()) {
    hash(insn->dest());
  }
  boost::hash_combine(m_registers_hash, m_hash);
  m_hash = old_hash;

  if (insn->has_literal()) {
    hash((uint64_t)insn->get_literal());
  } else if (insn->has_string()) {
    hash(insn->get_string());
  } else if (insn->has_type()) {
    hash(insn->get_type());
  } else if (insn->has_field()) {
    hash(insn->get_field());
  } else if (insn->has_method()) {
    hash(insn->get_method());
  } else if (insn->has_callsite()) {
    hash(insn->get_callsite());
  } else if (insn->has_methodhandle()) {
    hash(insn->get_methodhandle());
  } else if (insn->has_data()) {
    auto data = insn->get_data();
    hash(data->data_size());
    for (auto i = 0; i < data->data_size(); i++) {
      hash(data->data()[i]);
    }
  }
}

void DexClassHasher::hash(const IRCode* c) {
  if (!c) {
    return;
  }

  auto old_hash = m_hash;
  m_hash = 0;

  std::unordered_map<MethodItemEntry*, uint32_t> ids;
  auto get_id = [&ids](MethodItemEntry* mie) {
    auto it = ids.find(mie);
    if (it != ids.end()) {
      return it->second;
    } else {
      auto id = (uint32_t)ids.size();
      ids.emplace(mie, id);
      return id;
    }
  };

  hash(c->get_registers_size());
  for (const MethodItemEntry& mie : *c) {
    hash((uint8_t)mie.type);
    switch (mie.type) {
    case MFLOW_OPCODE:
      hash(mie.insn);
      break;
    case MFLOW_TRY:
      hash((uint8_t)mie.tentry->type);
      hash(get_id(mie.tentry->catch_start));
      break;
    case MFLOW_CATCH:
      if (mie.centry->catch_type) hash(mie.centry->catch_type);
      hash(get_id(mie.centry->next));
      break;
    case MFLOW_TARGET:
      hash((uint8_t)mie.target->type);
      hash(get_id(mie.target->src));
      break;
    case MFLOW_DEBUG:
      hash(mie.dbgop->opcode());
      hash(mie.dbgop->uvalue());
      break;
    case MFLOW_POSITION:
      if (mie.pos->method) hash(mie.pos->method);
      if (mie.pos->file) hash(mie.pos->file);
      hash(mie.pos->line);
      break;
    case MFLOW_FALLTHROUGH:
      break;
    default:
      always_assert(false);
    }
  }

  boost::hash_combine(m_code_hash, m_hash);
  m_hash = old_hash;
}

void DexClassHasher::hash(const DexProto* p) {
  hash(p->get_rtype());
  hash(p->get_args());
  hash(p->get_shorty());
}

void DexClassHasher::hash(const DexMethodRef* m) {
  hash(m->get_class());
  hash(m->get_name());
  hash(m->get_proto());
  hash(m->is_concrete());
  hash(m->is_external());
}

void DexClassHasher::hash(const DexMethod* m) {
  hash(static_cast<const DexMethodRef*>(m));
  hash(m->get_anno_set());
  hash(m->get_access());
  hash(m->get_deobfuscated_name());
  hash(m->get_param_anno());
  hash(m->get_code());
}

void DexClassHasher::hash(const DexFieldRef* f) {
  hash(f->get_name());
  hash(f->is_concrete());
  hash(f->is_external());
  hash(f->get_type());
}

void DexClassHasher::hash(const DexType* t) { hash(t->get_name()); }

void DexClassHasher::hash(const DexTypeList* l) { hash(l->get_type_list()); }

void DexClassHasher::hash(const ParamAnnotations* m) {
  if (m) {
    hash(*m);
  }
}

void DexClassHasher::hash(const DexAnnotationElement& elem) {
  hash(elem.string);
  hash(elem.encoded_value);
}

void DexClassHasher::hash(const EncodedAnnotations* a) {
  if (a) {
    hash(*a);
  }
}

void DexClassHasher::hash(const DexAnnotation* a) {
  if (a) {
    hash(&a->anno_elems());
    hash(a->type());
    hash((uint8_t)a->viz());
  }
}

void DexClassHasher::hash(const DexAnnotationSet* s) {
  if (s) {
    hash(s->get_annotations());
  }
}

void DexClassHasher::hash(const DexEncodedValue* v) {
  if (!v) {
    return;
  }

  auto evtype = v->evtype();
  hash((uint8_t)evtype);
  switch (evtype) {
  case DEVT_STRING: {
    auto s = static_cast<const DexEncodedValueString*>(v);
    hash(s->string());
    break;
  }
  case DEVT_TYPE: {
    auto t = static_cast<const DexEncodedValueType*>(v);
    hash(t->type());
    break;
  }
  case DEVT_FIELD:
  case DEVT_ENUM: {
    auto f = static_cast<const DexEncodedValueField*>(v);
    hash(f->field());
    break;
  }
  case DEVT_METHOD: {
    auto f = static_cast<const DexEncodedValueMethod*>(v);
    hash(f->method());
    break;
  }
  case DEVT_ARRAY: {
    auto a = static_cast<const DexEncodedValueArray*>(v);
    hash(*a->evalues());
    break;
  }
  case DEVT_ANNOTATION: {
    auto a = static_cast<const DexEncodedValueAnnotation*>(v);
    hash(a->type());
    hash(a->annotations()); // const EncodedAnnotations*
    break;
  };
  default:
    hash(v->value());
    break;
  }
}

void DexClassHasher::hash(const DexField* f) {
  hash(static_cast<const DexFieldRef*>(f));
  hash(f->get_anno_set());
  hash(f->get_static_value());
  hash(f->get_access());
  hash(f->get_deobfuscated_name());
}

DexHash DexClassHasher::run() {
  TRACE(HASHER, 2, "[hasher] ==== hashing class %s", SHOW(m_cls->get_type()));

  hash(m_cls->get_access());
  hash(m_cls->get_type());
  if (m_cls->get_super_class()) {
    hash(m_cls->get_super_class());
  }
  hash(m_cls->get_interfaces());
  hash(m_cls->get_anno_set());

  TRACE(HASHER, 3, "[hasher] === dmethods: %zu", m_cls->get_dmethods().size());
  hash(m_cls->get_dmethods());

  TRACE(HASHER, 3, "[hasher] === vmethods: %zu", m_cls->get_vmethods().size());
  hash(m_cls->get_vmethods());

  TRACE(HASHER, 3, "[hasher] === sfields: %zu", m_cls->get_sfields().size());
  hash(m_cls->get_sfields());

  TRACE(HASHER, 3, "[hasher] === ifields: %zu", m_cls->get_ifields().size());
  hash(m_cls->get_ifields());

  return DexHash{m_registers_hash, m_code_hash, m_hash};
}

} // namespace hashing
