/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>

#include "DexClass.h"
#include "DexStore.h"

// This class contains the infra required to use IODI (instruction based
// debug information). The infra includes the following:
//
// - Rename any methods whose external name (i.e. com.foo.class.method) collides
//   with another method's external name as long as said method is not
//   an initializer.
// - Keep a mapping of external name to list of dex methods who have said
//   external name (the above step is to minimize the number of methods that
//   have more than one entry per external name). For each external name with
//   multiple methods it will compute a list of callers for it as long as its
//   a non-virtual method.
// - Based on the above mapping return whether a method can use IODI
//   - Right now a method can use IODI if either:
//     - The method's external name doesn't collide with any other method's
//     - The method is non-virtual
// - Write out the mapping specified in the second bullet point to disk
class IODIMetadata {
 public:
  // This is used as an entry to the external name -> list of methods with that
  // external name mapping (see m_entries below). This is a tagged union with
  // m_duplicate as the tag.
  class Entry {
   public:
    struct Caller {
      const DexMethod* method;
      uint32_t pc;

      Caller(const DexMethod* meth, uint32_t p) : method(meth), pc(p) {}
    };
    // If there are duplicates then we want list of the duplicates and all
    // the callers to the given duplicate.
    using CallerMap = std::unordered_map<const DexMethod*, std::vector<Caller>>;
    union Data {
      const DexMethod* method;
      CallerMap* caller_map;
    };

   private:
    bool m_duplicate;
    Data m_data;

   public:
    // By default there is no duplicate. In order to make a duplicate we
    // push_back.
    Entry(const DexMethod* meth) : m_duplicate(false), m_data{.method = meth} {}
    ~Entry();

    void push_back(const DexMethod* meth);

    // This is used to reverse what push_back does (required after we rename
    // a method).
    void flatten() {
      if (is_duplicate()) {
        always_assert(size() != 0);
        if (size() == 1) {
          const DexMethod* meth = get_caller_map().begin()->first;
          m_duplicate = false;
          m_data.method = meth;
        }
      }
    }

    bool is_duplicate() const { return m_duplicate; }

    size_t size() const {
      return is_duplicate() ? m_data.caller_map->size() : 1;
    }

    CallerMap& get_caller_map() {
      always_assert(is_duplicate());
      return *m_data.caller_map;
    }

    const CallerMap& get_caller_map() const {
      always_assert(is_duplicate());
      return *m_data.caller_map;
    }

    const DexMethod* get_method() const {
      always_assert(!is_duplicate());
      return m_data.method;
    }

    friend struct CallerMarker;
  };
  using EntryMap = std::unordered_map<std::string, Entry>;
  using MethodToPrettyMap = std::unordered_map<const DexMethod*, std::string>;

 private:
  EntryMap m_entries;
  // This exists for can_safely_use_iodi
  MethodToPrettyMap m_pretty_map;
  std::unordered_set<const DexMethod*> m_huge_methods;
  Scope m_scope;
  bool m_enable_overloaded_methods;

  // Internal helper:
  // This will properly push_back a duplicate if method is a duplicate and
  // allow_collision is true. If allow collision is false and there is a
  // collision then will assert.
  void emplace_entry(const std::string& key,
                     const DexMethod* method,
                     bool allow_collision = true);

 public:
  // We can initialize this guy for free. If this feature is enabled then
  // invoke the methods below.
  IODIMetadata(bool enable_overloaded_methods = false)
      : m_enable_overloaded_methods(enable_overloaded_methods) {}

  // This fills the internal map of stack trace name -> method. This must be
  // called after the last pass and before anything starts to get lowered.
  void mark_methods(DexStoresVector& scope);

  // This is called while lowering to dex to note that a method has been
  // determined to be too big for a given dex.
  void mark_method_huge(const DexMethod* method, uint32_t size);

  // Returns whether we can symbolicate using IODI for the given method.
  bool can_safely_use_iodi(const DexMethod* method) const;

  // This must be called after all Scopes have been marked above with
  // mark_methods.
  void mark_callers();

  // Write to disk, pretty usual. Does nothing if filename len is 0.
  void write(const std::string& iodi_metadata_filename,
             const std::unordered_map<DexMethod*, uint64_t>& method_to_id);
};
