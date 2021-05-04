/*
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
  uint32_t min_sdk{0};

  /// Emit debug info regardless of min_sdk
  bool disable_min_sdk_opt;

  // We can initialize this guy for free. If this feature is enabled then
  // invoke the methods below.
  IODIMetadata() {}

  // Android builds with min_sdk >= 26 don't need IODI to emit debug info
  explicit IODIMetadata(uint32_t min_sdk, bool disable_min_sdk_opt = false)
      : min_sdk{min_sdk}, disable_min_sdk_opt{disable_min_sdk_opt} {}

  // This fills the internal map of stack trace name -> method. This must be
  // called after the last pass and before anything starts to get lowered.
  void mark_methods(DexStoresVector& scope);

  // This is called while lowering to dex to note that a method has been
  // determined to be too big for a given dex.
  void mark_method_huge(const DexMethod* method);

  bool is_huge(const DexMethod* m) const {
    return m_huge_methods.count(m) != 0;
  }

  // Write to disk, pretty usual. Does nothing if filename len is 0.
  using MethodToIdMap = std::unordered_map<DexMethod*, uint64_t>;
  void write(const std::string& iodi_metadata_filename,
             const MethodToIdMap& method_to_id);

  // Write to out. Underlying logic used by write(const std::string&, ...)
  // but exposed for testing.
  void write(std::ostream& ofs, const MethodToIdMap& method_to_id);

  static std::string get_iodi_name(const DexMethod* m);
  static const std::string& get_layered_name(const std::string& base_name,
                                             size_t layer,
                                             std::string& storage);

  const DexMethod* get_canonical_method(const DexMethod* m) const {
    return m_canonical.at(m);
  }

  const std::unordered_map<const DexMethod*,
                           std::unordered_set<const DexMethod*>>&
  get_name_clusters() const {
    return m_name_clusters;
  }
  const std::unordered_set<const DexMethod*>& get_cluster(
      const DexMethod* m) const {
    return m_name_clusters.at(get_canonical_method(m));
  }

  void set_iodi_layer(const DexMethod* method, size_t layer);
  size_t get_iodi_layer(const DexMethod* method) const;

 private:
  std::unordered_map<const DexMethod*, std::unordered_set<const DexMethod*>>
      m_name_clusters;
  std::unordered_map<const DexMethod*, const DexMethod*> m_canonical;

  std::unordered_map<const DexMethod*, std::pair<std::string, size_t>>
      m_iodi_method_layers;

  // These exists for can_safely_use_iodi
  std::unordered_map<const DexMethod*, std::string> m_method_to_name;
  std::unordered_set<const DexMethod*> m_huge_methods;

  bool m_marked{false};
};
