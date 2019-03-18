/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IODIMetadata.h"

#include "ClassHierarchy.h"
#include "DexAccess.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "Resolver.h"
#include "Trace.h"
#include "Walkers.h"

#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>

IODIMetadata::Entry::~Entry() {
  if (m_duplicate) {
    delete m_data.caller_map;
    m_data.caller_map = nullptr;
  }
}

void IODIMetadata::Entry::push_back(const DexMethod* meth) {
  if (m_duplicate) {
    (*m_data.caller_map)[meth];
  } else {
    CallerMap* caller_map = new CallerMap;
    // We call this to make an entry, but don't care about the result
    // for now.
    (*caller_map)[m_data.method];
    (*caller_map)[meth];
    m_duplicate = true;
    m_data.caller_map = caller_map;
  }
}

namespace {
template <typename T>
void emplace_warning_existence(T& map,
                               const DexMethod* m,
                               const std::string& key) {
  if (!map.emplace(m, key).second) {
    TRACE(IODI, 1, "[IODI] Already found method for %s in pretty map\n",
          key.c_str());
  }
}
} // namespace

void IODIMetadata::emplace_entry(const std::string& key,
                                 const DexMethod* method,
                                 bool allow_collision) {
  auto iter = m_entries.find(key);
  auto end = m_entries.end();
  always_assert(allow_collision || iter == end);
  if (iter == end) {
    TRACE(IODI, 6, "[IODI] Found 1 %s\n", key.c_str());
    m_entries.emplace(key, method);
    emplace_warning_existence(m_pretty_map, method, key);
  } else {
    iter->second.push_back(method);
    emplace_warning_existence(m_pretty_map, method, key);
  }
}

namespace {
// Returns com.foo.Bar. for the DexClass Lcom/foo/Bar;. Note the trailing
// '.'.
std::string pretty_prefix_for_cls(const DexClass* cls) {
  std::string pretty_name = JavaNameUtil::internal_to_external(cls->str());
  // Include the . separator
  pretty_name.push_back('.');
  return pretty_name;
}
} // namespace

void IODIMetadata::mark_methods(DexStoresVector& scope) {
  // Calculates the duplicates that will appear in stack traces when using iodi.
  // For example, if a method is overloaded or templating is being used the line
  // emitted in the stack trace may be ambiguous (all that's emitted is the
  // method name without any other type information). Before iodi we de-dup'd
  // by using proguard mapping line numbers (different methods corresponded
  // to different line numbers, so when symbolicating we would find the right
  // method by finding the method who's line number enclose the given one),
  // but now we emit instruction offsets, thus we can't disambiguate this way
  // anymore. With iodi we disambiguate by emitting information about
  // callers of any given method that may have a duplicate. On the symbolication
  // side of things, then, we use iodi_metadata to map from
  // (stack line, previous_method_id, previous_pc) -> method_id and
  // (method_id, insn_offset) -> line_offset (and eventually line offset maps
  // to (file, line number)).
  //
  // We do this linearly for now because otherwise we need locks
  for (auto& store : scope) {
    for (auto& classes : store.get_dexen()) {
      for (auto& cls : classes) {
        m_scope.push_back(cls);
        auto pretty_prefix = pretty_prefix_for_cls(cls);
        // First we need to mark all entries...
        for (DexMethod* m : cls->get_dmethods()) {
          emplace_entry(pretty_prefix + m->str(), m);
        }
        for (DexMethod* m : cls->get_vmethods()) {
          emplace_entry(pretty_prefix + m->str(), m);
        }
      }
    }
  }
}

void IODIMetadata::mark_method_huge(const DexMethod* method, uint32_t size) {
  m_huge_methods.insert(method);
  TRACE(IODI, 3, "[IODI] %s is too large to benefit from IODI: %u\n",
        SHOW(method), size);
}

// Returns whether we can symbolicate using IODI for the given method.
bool IODIMetadata::can_safely_use_iodi(const DexMethod* method) const {
  // We can use IODI if we don't have a collision, if the method isn't virtual
  // and if it isn't too big.
  //
  // It turns out for some methods using IODI isn't beneficial. See
  // comment in emit_instruction_offset_debug_info for more info.
  if (m_huge_methods.count(method) > 0) {
    return false;
  }

  // Eventually we can relax this constraint and calculate the subset of methods
  // that cannot be called externally and use those for IODI as well.
  if (!method->is_virtual()) {
    return true;
  }

  std::string pretty_name;
  {
    auto iter = m_pretty_map.find(method);
    if (iter == m_pretty_map.end()) {
      fprintf(stderr, "[IODI] Warning: didn't find %s in pretty map in %s",
              SHOW(method), __PRETTY_FUNCTION__);
      auto cls = type_class(method->get_class());
      always_assert(cls);
      pretty_name = pretty_prefix_for_cls(cls);
      pretty_name += method->str();
    } else {
      pretty_name = iter->second;
    }
  }
  auto iter = m_entries.find(pretty_name);
  if (iter == m_entries.end()) {
    fprintf(stderr,
            "[IODI] Warning: failing to use IODI on unknown method:"
            " %s\n",
            pretty_name.c_str());
    return false;
  }
  return !iter->second.is_duplicate();
}

namespace {
// This class marks all callers to the callees specified in the CallerMap. That
// is CallerMap specifies what callees we want to know the callers about and
// then CallerMarker will go through and mark any possible caller of said
// callees.
struct CallerMarker {
  using CallerList = std::vector<IODIMetadata::Entry::Caller>;
  // This is the same as Entry::CallerMap but with pointers to the caller
  // lists instead.
  using CallerMap = std::unordered_map<const DexMethod*, CallerList*>;

  CallerMarker(CallerMap& caller_map) : caller_map(caller_map) {}

  boost::shared_mutex resolver_cache_mutex;
  MethodRefCache resolver_cache;

  std::mutex caller_map_mutex;
  CallerMap& caller_map;

  void mark_callers(Scope& scope) {
    // This is safe to do in parallel because the data members mutated have
    // mutexes.
    walk::parallel::methods(
        scope, [&](DexMethod* method) { this->mark_caller(method); });
  }

  void mark_caller(const DexMethod* caller) {
    // Pretty standard algo: walk all the insns looking for referenced methods.
    // Resolve the referenced method if possible and insert in caller_map if
    // necessary.
    auto code = caller->get_dex_code();
    if (!code) {
      return;
    }
    uint32_t pc = 0;
    const auto& insns = code->get_instructions();
    for (const DexInstruction* insn : insns) {
      if (!insn->has_method()) {
        pc += insn->size();
        continue;
      }
      const DexOpcodeMethod* minsn = static_cast<const DexOpcodeMethod*>(insn);
      const DexMethodRef* method = minsn->get_method();
      DexOpcode opcode = minsn->opcode();
      MethodSearch search;
      bool is_super = false;
      switch (opcode) {
      case DOPCODE_INVOKE_VIRTUAL:
      case DOPCODE_INVOKE_VIRTUAL_RANGE:
        search = MethodSearch::Virtual;
        break;
      case DOPCODE_INVOKE_SUPER:
      case DOPCODE_INVOKE_SUPER_RANGE:
        if (auto cls = type_class(method->get_class())) {
          // https://source.android.com/devices/tech/dalvik/dalvik-bytecode:
          //
          // In Dex files version 037 or later, if the method_id refers to an
          // interface method, invoke-super is used to invoke the most specific,
          // non-overridden version of that method defined on that interface.
          search = is_interface(cls) ? MethodSearch::Interface
                                     : MethodSearch::Virtual;
        } else {
          search = MethodSearch::Any;
        }
        is_super = true;
        break;
      case DOPCODE_INVOKE_INTERFACE:
      case DOPCODE_INVOKE_INTERFACE_RANGE:
        search = MethodSearch::Interface;
        break;
      case DOPCODE_INVOKE_DIRECT:
      case DOPCODE_INVOKE_DIRECT_RANGE:
        search = MethodSearch::Direct;
        break;
      case DOPCODE_INVOKE_STATIC:
      case DOPCODE_INVOKE_STATIC_RANGE:
        search = MethodSearch::Static;
        break;
      default:
        always_assert_log(false, "Unexpected opcode with method");
        break;
      }
      DexMethod* callee = nullptr;
      DexMethodRef* unresolved_method = const_cast<DexMethodRef*>(method);
      if (unresolved_method->is_def()) {
        callee = static_cast<DexMethod*>(unresolved_method);
      } else {
        {
          boost::shared_lock<boost::shared_mutex> locker(resolver_cache_mutex);
          auto it = resolver_cache.find(unresolved_method);
          if (it != resolver_cache.end()) {
            callee = it->second;
          }
        }
        if (callee == nullptr) {
          callee = resolve_method(unresolved_method, search);
          {
            boost::unique_lock<boost::shared_mutex> locker(
                resolver_cache_mutex);
            resolver_cache[unresolved_method] =
                callee ?: reinterpret_cast<DexMethod*>(1);
          }
        }
        // We cache 1 if we failed to resolve the method so we don't try to
        // re-resolve it.
        if (reinterpret_cast<uintptr_t>(callee) == 1) {
          callee = nullptr;
        }
      }
      if (search == MethodSearch::Direct || search == MethodSearch::Static) {
        auto iter = caller_map.find(callee);
        if (iter != caller_map.end()) {
          TRACE(IODI, 5, "[IODI] Adding %p, %u to callsite vec for %p\n",
                caller, pc, callee);
          std::lock_guard<std::mutex> locker(caller_map_mutex);
          iter->second->emplace_back(caller, pc);
        }
      }
      pc += minsn->size();
    }
  }
};
} // namespace

void IODIMetadata::mark_callers() {
  // CallerMap sets up the exact set of DexMethods that we care about
  // (any entry that doesn't have any duplicates we don't care about, so
  // it won't get a slot in this map). It points to the vector of callers
  // corresponding to the given DexMethod.
  //
  // For now we're only supporting this form of symbolication for direct/static
  // methods only.
  CallerMarker::CallerMap caller_map;
  for (auto& it : m_entries) {
    if (it.second.is_duplicate()) {
      for (auto& meth_it : it.second.get_caller_map()) {
        // If we're only supporting direct methods then skip any virtual meth
        // as we don't care about it since it'll emit normal debug info.
        if (meth_it.first->is_virtual()) {
          continue;
        }
        caller_map[meth_it.first] = &meth_it.second;
      }
    }
  }

  CallerMarker marker(caller_map);
  marker.mark_callers(m_scope);
}

void IODIMetadata::write(
    const std::string& iodi_metadata_filename,
    const std::unordered_map<DexMethod*, uint64_t>& method_to_id) {
  if (iodi_metadata_filename.empty()) {
    return;
  }
  /*
   * Binary file format
   * {
   *  magic: uint32_t = 0xfaceb001
   *  version: uint32_t = 1
   *  single_count: uint32_t
   *  dup_count: uint32_t
   *  single_entries: single_entry_t[single_count]
   *  dup_entries: dup_entry_t[dup_count]
   * }
   * where
   * single_entry_t = {
   *  klen: uint16_t
   *  method_id: uint64_t
   *  key: char[klen]
   * }
   * dup_entry_t = {
   *  klen: uint16_t
   *  count: uint32_t
   *  key: char[klen]
   *  caller_mappings: caller_mapping_t[count]
   * }
   * caller_mapping_t = {
   *  method_id: uint64_t
   *  count: uint32_t
   *  callsites: callsite_t[count]
   * }
   * callsite_t = {
   *  caller_method_id: uint64_t
   *  pc: uint16_t
   * }
   */
  if (iodi_metadata_filename.empty()) {
    return;
  }
  std::ofstream ofs(iodi_metadata_filename.c_str(),
                    std::ofstream::out | std::ofstream::trunc);
  struct __attribute__((__packed__)) Header {
    uint32_t magic;
    uint32_t version;
    uint32_t single_count;
    uint32_t dup_count;
  };
  ofs.seekp(sizeof(Header));
  uint32_t single_count = 0;
  uint32_t dup_count = 0;

  std::ostringstream dofs;
  struct __attribute__((__packed__)) SingleEntryHeader {
    uint16_t klen;
    uint64_t method_id;
  } seh;
  struct __attribute__((__packed__)) DupEntryHeader {
    uint16_t klen;
    uint32_t count;
  } deh;

  size_t dup_meth_count_not_emitted = 0;
  size_t dup_meth_count_emitted = 0;

  size_t dup_meth_with_dbg_count_not_emitted = 0;
  size_t dup_meth_with_dbg_count_emitted = 0;

  size_t single_huge_count = 0;

  for (const auto& it : m_entries) {
    if (it.second.is_duplicate()) {
      always_assert(it.second.size() > 1);
      // Skip if this isn't a method that's safe to use IODI with.
      const auto& caller_map = it.second.get_caller_map();
      size_t mids_count = caller_map.size();
      if (!can_safely_use_iodi(caller_map.begin()->first)) {
        dup_meth_count_not_emitted += mids_count;
        for (auto& caller_it : caller_map) {
          const DexMethod* callee = caller_it.first;
          const auto dc = callee->get_dex_code();
          if (dc != nullptr && dc->get_debug_item() != nullptr) {
            dup_meth_with_dbg_count_not_emitted += 1;
          }
        }
        continue;
      }
      dup_meth_count_emitted += mids_count;
      dup_count += 1;
      always_assert_log(dup_count != 0, "Too many dups found, overflowed");
      always_assert(it.first.size() < UINT16_MAX);
      deh.klen = it.first.size();
      always_assert(mids_count < UINT32_MAX);
      deh.count = mids_count;
      dofs.write((const char*)&deh, sizeof(DupEntryHeader));
      dofs << it.first;
      for (const auto& caller_it : caller_map) {
        const DexMethod* callee = caller_it.first;
        const auto dc = callee->get_dex_code();
        if (dc != nullptr && dc->get_debug_item() != nullptr) {
          dup_meth_with_dbg_count_emitted += 1;
        }
        always_assert(caller_it.second.size() < UINT32_MAX);
        struct __attribute__((__packed__)) CallerMappingHeader {
          uint64_t method_id;
          uint32_t count;
        } mapping_hdr = {
            .method_id = method_to_id.at(const_cast<DexMethod*>(callee)),
            .count = static_cast<uint32_t>(caller_it.second.size()),
        };
        dofs.write((const char*)&mapping_hdr, sizeof(CallerMappingHeader));
        struct __attribute__((__packed__)) Callsite {
          uint64_t method_id;
          uint16_t pc;
        } callsite;
        for (const auto& caller : caller_it.second) {
          callsite.method_id =
              method_to_id.at(const_cast<DexMethod*>(caller.method));
          always_assert(caller.pc < UINT16_MAX);
          callsite.pc = caller.pc;
          dofs.write((const char*)&callsite, sizeof(Callsite));
        }
      }
    } else {
      if (!can_safely_use_iodi(it.second.get_method())) {
        // This will occur if at some point a method was marked as huge during
        // encoding.
        single_huge_count += 1;
        continue;
      }
      single_count += 1;
      always_assert_log(single_count != 0, "Too many sgls found, overflowed");
      always_assert(it.first.size() < UINT16_MAX);
      seh.klen = it.first.size();
      seh.method_id =
          method_to_id.at(const_cast<DexMethod*>(it.second.get_method()));
      ofs.write((const char*)&seh, sizeof(SingleEntryHeader));
      ofs << it.first;
    }
  }
  ofs << dofs.str();
  // Rewind and write the header now that we know single/dup counts
  ofs.seekp(0);
  Header header = {.magic = 0xfaceb001,
                   .version = 1,
                   // Will rewrite the header later
                   .single_count = single_count,
                   .dup_count = dup_count};
  ofs.write((const char*)&header, sizeof(Header));
  TRACE(IODI, 1,
        "[IODI] Emitted %u singles, %u duplicates, ignored %u duplicates."
        " %u emitted dups had debug items, %u non-emitted dups had debug"
        " items and %u methods were too big, %u were collision free\n",
        single_count, dup_meth_count_emitted, dup_meth_count_not_emitted,
        dup_meth_with_dbg_count_emitted, dup_meth_with_dbg_count_not_emitted,
        m_huge_methods.size(), single_huge_count);
}
