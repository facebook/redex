/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexHasher.h"

#include <cinttypes>
#include <ostream>

#include "Debug.h"
#include "DexAccess.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexPosition.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "Sha1.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace hashing {

// A local implementation. Avoids heap allocation for the scope version.

namespace {

class Impl final {
 public:
  explicit Impl(DexClass* cls) : m_cls(cls) {}
  DexHash run();
  void print(std::ostream&);

 private:
  DexHash get_hash() const;
  void hash_metadata();
  void hash(const std::string_view str);
  void hash(const std::string& str);
  void hash(int value);
  void hash(uint64_t value);
  void hash(uint32_t value);
  void hash(uint16_t value);
  void hash(uint8_t value);
  void hash(bool value);
  void hash(const IRCode* c);
  void hash(const cfg::ControlFlowGraph& cfg);
  void hash_code_init(
      const IRList::const_iterator& begin,
      const IRList::const_iterator& end,
      std::unordered_map<const MethodItemEntry*, uint32_t>* mie_ids,
      std::unordered_map<DexPosition*, uint32_t>* pos_ids);
  void hash_code_flush(
      const IRList::const_iterator& begin,
      const IRList::const_iterator& end,
      const std::unordered_map<const MethodItemEntry*, uint32_t>& mie_ids,
      const std::unordered_map<DexPosition*, uint32_t>& pos_ids);
  void hash(const IRInstruction* insn);
  void hash(const EncodedAnnotations* a);
  void hash(const ParamAnnotations* m);
  void hash(const DexAnnotation* a);
  void hash(const DexAnnotationSet* s);
  void hash(const DexAnnotationElement& elem);
  void hash(const DexEncodedValue* v);
  void hash(const DexProto* p);
  void hash(const DexMethodRef* m);
  void hash(const DexMethod* m);
  void hash(const DexFieldRef* f);
  void hash(const DexField* f);
  void hash(const DexType* t);
  void hash(const DexTypeList* l);
  void hash(const DexString* s);
  template <class T>
  void hash(const std::vector<T>& l) {
    hash((uint64_t)l.size());
    for (const auto& elem : l) {
      hash(elem);
    }
  }
  template <class T>
  void hash(const std::deque<T>& l) {
    hash((uint64_t)l.size());
    for (const auto& elem : l) {
      hash(elem);
    }
  }
  template <typename T>
  void hash(const std::unique_ptr<T>& uptr) {
    hash(uptr.get());
  }
  template <class K, class V>
  void hash(const std::map<K, V>& l) {
    hash((uint64_t)l.size());
    for (const auto& p : l) {
      hash(p.first);
      hash(p.second);
    }
  }

  // Template magic.
  template <typename, typename = void>
  struct has_size : std::false_type {};
  template <typename T>
  struct has_size<T, std::void_t<decltype(&T::size)>> : std::true_type {};

  // Not applying to map-like things (like unordered_map) should be done by
  // failures to hash the elements.
  template <typename T,
            typename std::enable_if<has_size<T>::value, T>::type* = nullptr>
  void hash(const T& c) {
    hash((uint64_t)c.size());
    for (const auto& elem : c) {
      hash(elem);
    }
  }

  DexClass* m_cls;
  size_t m_hash{0};
  size_t m_code_hash{0};
  size_t m_registers_hash{0};
  size_t m_positions_hash{0};
};

void Impl::hash(const std::string_view str) {
  TRACE(HASHER, 4, "[hasher] %s", str_copy(str).c_str());
  boost::hash_combine(m_hash, str);
}

void Impl::hash(const std::string& str) {
  TRACE(HASHER, 4, "[hasher] %s", str.c_str());
  boost::hash_combine(m_hash, str);
}

void Impl::hash(const DexString* s) { hash(s->str()); }

void Impl::hash(bool value) {
  TRACE(HASHER, 4, "[hasher] %u", value);
  boost::hash_combine(m_hash, value);
}
void Impl::hash(uint8_t value) {
  TRACE(HASHER, 4, "[hasher] %" PRIu8, value);
  boost::hash_combine(m_hash, value);
}

void Impl::hash(uint16_t value) {
  TRACE(HASHER, 4, "[hasher] %" PRIu16, value);
  boost::hash_combine(m_hash, value);
}

void Impl::hash(uint32_t value) {
  TRACE(HASHER, 4, "[hasher] %" PRIu32, value);
  boost::hash_combine(m_hash, value);
}

void Impl::hash(uint64_t value) {
  TRACE(HASHER, 4, "[hasher] %" PRIu64, value);
  boost::hash_combine(m_hash, value);
}

void Impl::hash(int value) {
  if (sizeof(int) == 8) {
    hash((uint64_t)value);
  } else {
    hash((uint32_t)value);
  }
}

void Impl::hash(const IRInstruction* insn) {
  hash((uint16_t)insn->opcode());

  auto old_hash = m_hash;
  m_hash = 0;
  hash(insn->srcs_vec());
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
    hash((uint32_t)data->data_size());
    for (size_t i = 0; i < data->data_size(); i++) {
      hash(data->data()[i]);
    }
  }
}

void Impl::hash(const IRCode* c) {
  if (!c) {
    return;
  }

  auto old_hash = m_hash;
  m_hash = 0;

  if (c->editable_cfg_built()) {
    hash(c->cfg());
  } else {
    hash(c->get_registers_size());

    std::unordered_map<const MethodItemEntry*, uint32_t> mie_ids;
    std::unordered_map<DexPosition*, uint32_t> pos_ids;

    hash_code_init(c->begin(), c->end(), &mie_ids, &pos_ids);
    hash_code_flush(c->begin(), c->end(), mie_ids, pos_ids);
  }

  boost::hash_combine(m_code_hash, m_hash);
  m_hash = old_hash;
}

void Impl::hash(const cfg::ControlFlowGraph& cfg) {
  hash(cfg.get_registers_size());
  hash((uint32_t)cfg.entry_block()->id());
  std::unordered_map<const MethodItemEntry*, uint32_t> mie_ids;
  std::unordered_map<DexPosition*, uint32_t> pos_ids;
  for (auto b : cfg.blocks()) {
    hash((uint32_t)b->id());
    hash_code_init(b->begin(), b->end(), &mie_ids, &pos_ids);
    for (auto e : b->succs()) {
      hash((uint32_t)e->target()->id());
      hash((uint8_t)e->type());
      if (e->type() == cfg::EDGE_THROW) {
        auto throw_info = e->throw_info();
        hash(throw_info->index);
        if (throw_info->catch_type) {
          hash(throw_info->catch_type);
        }
        continue;
      }
      auto case_key = e->case_key();
      if (case_key) {
        hash((uint32_t)*case_key);
      }
    }
  }
  // mie-ids would only be generated by MethodItemEntries that are not present
  // in editable cfgs.
  always_assert(mie_ids.empty());
  for (auto b : cfg.blocks()) {
    hash_code_flush(b->begin(), b->end(), mie_ids, pos_ids);
  }
}

void Impl::hash_code_init(
    const IRList::const_iterator& begin,
    const IRList::const_iterator& end,
    std::unordered_map<const MethodItemEntry*, uint32_t>* mie_ids,
    std::unordered_map<DexPosition*, uint32_t>* pos_ids) {
  auto get_mie_id = [mie_ids](const MethodItemEntry* mie) {
    auto it = mie_ids->find(mie);
    if (it != mie_ids->end()) {
      return it->second;
    } else {
      auto id = (uint32_t)mie_ids->size();
      mie_ids->emplace(mie, id);
      return id;
    }
  };

  auto get_pos_id = [pos_ids](DexPosition* pos) {
    auto it = pos_ids->find(pos);
    if (it != pos_ids->end()) {
      return it->second;
    } else {
      auto id = (uint32_t)pos_ids->size();
      pos_ids->emplace(pos, id);
      return id;
    }
  };

  for (auto code_it = begin; code_it != end; code_it++) {
    const MethodItemEntry& mie = *code_it;
    switch (mie.type) {
    case MFLOW_OPCODE:
      hash((uint8_t)MFLOW_OPCODE);
      hash(mie.insn);
      break;
    case MFLOW_TRY:
      hash((uint8_t)MFLOW_TRY);
      hash((uint8_t)mie.tentry->type);
      hash(get_mie_id(mie.tentry->catch_start));
      break;
    case MFLOW_CATCH:
      hash((uint8_t)MFLOW_CATCH);
      if (mie.centry->catch_type) hash(mie.centry->catch_type);
      hash(get_mie_id(mie.centry->next));
      break;
    case MFLOW_TARGET:
      hash((uint8_t)MFLOW_TARGET);
      hash((uint8_t)mie.target->type);
      hash(get_mie_id(mie.target->src));
      break;
    case MFLOW_DEBUG:
      hash((uint8_t)MFLOW_DEBUG);
      hash(mie.dbgop->opcode());
      hash(mie.dbgop->uvalue());
      break;
    case MFLOW_POSITION: {
      auto old_hash2 = m_hash;
      m_hash = 0;
      hash((uint8_t)MFLOW_POSITION);
      if (mie.pos->method) hash(mie.pos->method);
      if (mie.pos->file) hash(mie.pos->file);
      hash(mie.pos->line);
      if (mie.pos->parent) hash(get_pos_id(mie.pos->parent));
      boost::hash_combine(m_positions_hash, m_hash);
      m_hash = old_hash2;
      break;
    }
    case MFLOW_SOURCE_BLOCK:
      hash((uint8_t)MFLOW_SOURCE_BLOCK);
      for (auto* sb = mie.src_block.get(); sb != nullptr; sb = sb->next.get()) {
        hash(sb->src);
        hash(sb->id);
      }
      break;
    case MFLOW_FALLTHROUGH:
      hash((uint8_t)MFLOW_FALLTHROUGH);
      break;
    case MFLOW_DEX_OPCODE:
      not_reached();
    default:
      not_reached();
    }
  }
}

void Impl::hash_code_flush(
    const IRList::const_iterator& begin,
    const IRList::const_iterator& end,
    const std::unordered_map<const MethodItemEntry*, uint32_t>& mie_ids,
    const std::unordered_map<DexPosition*, uint32_t>& pos_ids) {
  uint32_t mie_index = 0;
  for (auto code_it = begin; code_it != end; code_it++) {
    const MethodItemEntry& mie = *code_it;
    auto it = mie_ids.find(&mie);
    if (it != mie_ids.end()) {
      hash(it->second);
      hash(mie_index);
    }
    if (mie.type == MFLOW_POSITION) {
      auto it2 = pos_ids.find(mie.pos.get());
      if (it2 != pos_ids.end()) {
        auto old_hash2 = m_hash;
        m_hash = 0;
        hash(it2->second);
        hash(mie_index);
        boost::hash_combine(m_positions_hash, m_hash);
        m_hash = old_hash2;
      }
    }
    mie_index++;
  }
}

void Impl::hash(const DexProto* p) {
  hash(p->get_rtype());
  hash(p->get_args());
  hash(p->get_shorty());
}

void Impl::hash(const DexMethodRef* m) {
  hash(m->get_class());
  hash(m->get_name());
  hash(m->get_proto());
  hash(m->is_concrete());
  hash(m->is_external());
}

void Impl::hash(const DexMethod* m) {
  hash(static_cast<const DexMethodRef*>(m));
  hash(m->get_anno_set());
  hash(m->get_access());
  hash(m->get_deobfuscated_name_or_empty());
  hash(m->get_param_anno());
  hash(m->get_code());
}

void Impl::hash(const DexFieldRef* f) {
  hash(f->get_name());
  hash(f->is_concrete());
  hash(f->is_external());
  hash(f->get_type());
}

void Impl::hash(const DexType* t) { hash(t->get_name()); }

void Impl::hash(const DexTypeList* l) { hash(*l); }

void Impl::hash(const ParamAnnotations* m) {
  if (m) {
    hash(*m);
  }
}

void Impl::hash(const DexAnnotationElement& elem) {
  hash(elem.string);
  hash(elem.encoded_value);
}

void Impl::hash(const EncodedAnnotations* a) {
  if (a) {
    hash(*a);
  }
}

void Impl::hash(const DexAnnotation* a) {
  if (a) {
    hash(&a->anno_elems());
    hash(a->type());
    hash((uint8_t)a->viz());
  }
}

void Impl::hash(const DexAnnotationSet* s) {
  if (s) {
    hash(s->get_annotations());
  }
}

void Impl::hash(const DexEncodedValue* v) {
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

void Impl::hash(const DexField* f) {
  hash(static_cast<const DexFieldRef*>(f));
  hash(f->get_anno_set());
  hash(f->get_static_value());
  hash(f->get_access());
  hash(f->get_deobfuscated_name_or_empty());
}

void Impl::hash_metadata() {
  hash(m_cls->get_access());
  hash(m_cls->get_type());
  if (m_cls->get_super_class()) {
    hash(m_cls->get_super_class());
  }
  hash(m_cls->get_interfaces());
  hash(m_cls->get_anno_set());
}

DexHash Impl::get_hash() const {
  return DexHash{m_positions_hash, m_registers_hash, m_code_hash, m_hash};
}

DexHash Impl::run() {
  TRACE(HASHER, 2, "[hasher] ==== hashing class %s", SHOW(m_cls->get_type()));

  hash_metadata();

  TRACE(HASHER, 3, "[hasher] === dmethods: %zu", m_cls->get_dmethods().size());
  hash(m_cls->get_dmethods());

  TRACE(HASHER, 3, "[hasher] === vmethods: %zu", m_cls->get_vmethods().size());
  hash(m_cls->get_vmethods());

  TRACE(HASHER, 3, "[hasher] === sfields: %zu", m_cls->get_sfields().size());
  hash(m_cls->get_sfields());

  TRACE(HASHER, 3, "[hasher] === ifields: %zu", m_cls->get_ifields().size());
  hash(m_cls->get_ifields());

  return get_hash();
}

void Impl::print(std::ostream& ofs) {
  hash_metadata();
  ofs << "type " << show(m_cls) << " #" << hash_to_string(m_hash) << std::endl;
  for (auto field : m_cls->get_ifields()) {
    m_hash = 0;
    hash(field);
    ofs << "ifield " << show(field) << " #" << hash_to_string(m_hash)
        << std::endl;
  }
  for (auto field : m_cls->get_sfields()) {
    m_hash = 0;
    hash(field);
    ofs << "sfield " << show(field) << " #" << hash_to_string(m_hash)
        << std::endl;
  }

  for (auto method : m_cls->get_dmethods()) {
    m_hash = 0;
    hash(method);
    ofs << "dmethod " << show(method) << " " << get_hash() << std::endl;
  }
  for (auto method : m_cls->get_vmethods()) {
    m_hash = 0;
    hash(method);
    ofs << "vmethod " << show(method) << " " << get_hash() << std::endl;
  }
}

} // namespace

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
  std::vector<size_t> class_positions_hashes(class_indices.size());
  std::vector<size_t> class_registers_hashes(class_indices.size());
  std::vector<size_t> class_code_hashes(class_indices.size());
  std::vector<size_t> class_signature_hashes(class_indices.size());
  walk::parallel::classes(m_scope, [&](DexClass* cls) {
    Impl class_hasher(cls);
    DexHash class_hash = class_hasher.run();
    auto index = class_indices.at(cls);
    class_positions_hashes.at(index) = class_hash.positions_hash;
    class_registers_hashes.at(index) = class_hash.registers_hash;
    class_code_hashes.at(index) = class_hash.code_hash;
    class_signature_hashes.at(index) = class_hash.signature_hash;
  });

  return DexHash{boost::hash_value(class_positions_hashes),
                 boost::hash_value(class_registers_hashes),
                 boost::hash_value(class_code_hashes),
                 boost::hash_value(class_signature_hashes)};
}

struct DexClassHasher::Fwd final {
  Impl impl;

  explicit Fwd(DexClass* cls) : impl(cls) {}

  DexHash run() { return impl.run(); }
  void print(std::ostream& os) { impl.print(os); }
};

DexClassHasher::DexClassHasher(DexClass* cls)
    : m_fwd(std::make_unique<Fwd>(cls)) {}
DexClassHasher::~DexClassHasher() = default; // For forwarding.

DexHash DexClassHasher::run() { return m_fwd->run(); }

void DexClassHasher::print(std::ostream& os) { m_fwd->print(os); }

void print_classes(std::ostream& output, const Scope& classes) {
  std::unordered_map<DexClass*, std::stringstream> class_strs;
  walk::classes(classes, [&](DexClass* cls) {
    class_strs.emplace(cls, std::stringstream());
  });
  walk::parallel::classes(classes, [&](DexClass* cls) {
    DexClassHasher(cls).print(class_strs.at(cls));
  });
  walk::classes(classes,
                [&](DexClass* cls) { output << class_strs.at(cls).rdbuf(); });
}

} // namespace hashing

std::ostream& operator<<(std::ostream& os, const hashing::DexHash& hash) {
  os << "(P#" << hashing::hash_to_string(hash.positions_hash) << ", R#"
     << hashing::hash_to_string(hash.registers_hash) << ", C#"
     << hashing::hash_to_string(hash.code_hash) << ", S#"
     << hashing::hash_to_string(hash.signature_hash) << ")";
  return os;
}
