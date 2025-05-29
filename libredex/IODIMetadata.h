/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DeterministicContainers.h"
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

  enum class IODILayerMode : uint8_t {
    // Write all IODI programs.
    kFull,
    // For API level 26 and above, ART defaults to printing PCs
    // in place of line numbers so IODI debug programs aren't needed.
    kSkipLayer0AtApi26,
    // Always skip the layer 0 programs except for primary. Mostly for testing.
    kAlwaysSkipLayer0ExceptPrimary,
    // Always skip the layer 0 programs. Mostly for testing.
    kAlwaysSkipLayer0,
  };
  IODILayerMode layer_mode{IODILayerMode::kFull};

  static IODILayerMode parseLayerMode(const std::string& v);

  // We can initialize this guy for free. If this feature is enabled then
  // invoke the methods below.
  IODIMetadata() {}

  // Android builds with min_sdk >= 26 don't need IODI to emit debug info
  explicit IODIMetadata(uint32_t min_sdk,
                        IODILayerMode layer_mode = IODILayerMode::kFull)
      : min_sdk{min_sdk} {
    this->layer_mode = min_sdk <= 19 ? IODILayerMode::kFull : layer_mode;
  }

  // This fills the internal map of stack trace name -> method. This must be
  // called after the last pass and before anything starts to get lowered.
  void mark_methods(DexStoresVector& scope, bool iodi_layers);

  // This is called while lowering to dex to note that a method has been
  // determined to be too big for a given dex.
  void mark_method_huge(const DexMethod* method);

  bool is_huge(const DexMethod* m) const {
    return m_huge_methods.count(m) != 0;
  }

  // Write to disk, pretty usual. Does nothing if filename len is 0.
  using MethodToIdMap = UnorderedMap<DexMethod*, uint64_t>;
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
    auto it = m_canonical.find(m);
    if (it == m_canonical.end()) {
      return m;
    }

    return it->second;
  }

  const UnorderedSet<const DexMethod*>&
  get_too_large_cluster_canonical_methods() const {
    return m_too_large_cluster_canonical_methods;
  }

  bool is_in_global_cluster(const DexMethod* m) const {
    return m_canonical.find(m) != m_canonical.end();
  }

  void set_iodi_layer(const DexMethod* method, size_t layer);
  size_t get_iodi_layer(const DexMethod* method) const;
  bool has_iodi_layer(const DexMethod* method) const;

 private:
  UnorderedSet<const DexMethod*> m_too_large_cluster_canonical_methods;
  UnorderedMap<const DexMethod*, const DexMethod*> m_canonical;

  UnorderedMap<const DexMethod*, size_t> m_iodi_method_layers;

  // These exists for can_safely_use_iodi
  UnorderedSet<const DexMethod*> m_huge_methods;

  bool m_marked{false};
};
