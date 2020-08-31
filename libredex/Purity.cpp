/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Purity.h"

#include <sstream>

#include "ConfigFiles.h"
#include "ControlFlow.h"
#include "EditableCfgAdapter.h"
#include "IRInstruction.h"
#include "Resolver.h"
#include "Trace.h"
#include "Walkers.h"
#include "WeakTopologicalOrdering.h"

namespace purity {

CacheConfig& CacheConfig::get_default() {
  static CacheConfig cc = CacheConfig{};
  return cc;
}

void CacheConfig::parse_default(const ConfigFiles& conf) {
  CacheConfig& def = CacheConfig::get_default();
  const auto& json = conf.get_json_config();
  if (json.contains("purity")) {
    auto purity = JsonWrapper(json["purity"]);
    if (purity.contains("cache")) {
      auto cache = JsonWrapper(purity["cache"]);
      cache.get("max_entries", def.max_entries, def.max_entries);
      cache.get("fill_entry_threshold", def.fill_entry_threshold,
                def.fill_entry_threshold);
      cache.get("fill_size_threshold", def.fill_size_threshold,
                def.fill_size_threshold);
    }
  }
}

} // namespace purity

std::ostream& operator<<(std::ostream& o, const CseLocation& l) {
  switch (l.special_location) {
  case CseSpecialLocations::GENERAL_MEMORY_BARRIER:
    o << "*";
    break;
  case CseSpecialLocations::ARRAY_COMPONENT_TYPE_INT:
    o << "(int[])[.]";
    break;
  case CseSpecialLocations::ARRAY_COMPONENT_TYPE_BYTE:
    o << "(byte[])[.]";
    break;
  case CseSpecialLocations::ARRAY_COMPONENT_TYPE_CHAR:
    o << "(char[])[.]";
    break;
  case CseSpecialLocations::ARRAY_COMPONENT_TYPE_WIDE:
    o << "(long|double[])[.]";
    break;
  case CseSpecialLocations::ARRAY_COMPONENT_TYPE_SHORT:
    o << "(short[])[.]";
    break;
  case CseSpecialLocations::ARRAY_COMPONENT_TYPE_OBJECT:
    o << "(Object[])[.]";
    break;
  case CseSpecialLocations::ARRAY_COMPONENT_TYPE_BOOLEAN:
    o << "(boolean[])[.]";
    break;
  default:
    o << SHOW(l.field);
    break;
  }
  return o;
}

std::ostream& operator<<(std::ostream& o, const CseUnorderedLocationSet& ls) {
  o << "{";
  bool first = true;
  for (const auto& l : ls) {
    if (first) {
      first = false;
    } else {
      o << ", ";
    }
    o << l;
  }
  o << "}";
  return o;
}

CseLocation get_field_location(IROpcode opcode, const DexField* field) {
  always_assert(is_ifield_op(opcode) || is_sfield_op(opcode));
  if (field != nullptr && !is_volatile(field)) {
    return CseLocation(field);
  }

  return CseLocation(CseSpecialLocations::GENERAL_MEMORY_BARRIER);
}

CseLocation get_field_location(IROpcode opcode, const DexFieldRef* field_ref) {
  always_assert(is_ifield_op(opcode) || is_sfield_op(opcode));
  DexField* field =
      resolve_field(field_ref, is_sfield_op(opcode) ? FieldSearch::Static
                                                    : FieldSearch::Instance);
  return get_field_location(opcode, field);
}

CseLocation get_read_array_location(IROpcode opcode) {
  switch (opcode) {
  case OPCODE_AGET:
    return CseLocation(CseSpecialLocations::ARRAY_COMPONENT_TYPE_INT);
  case OPCODE_AGET_BYTE:
    return CseLocation(CseSpecialLocations::ARRAY_COMPONENT_TYPE_BYTE);
  case OPCODE_AGET_CHAR:
    return CseLocation(CseSpecialLocations::ARRAY_COMPONENT_TYPE_CHAR);
  case OPCODE_AGET_WIDE:
    return CseLocation(CseSpecialLocations::ARRAY_COMPONENT_TYPE_WIDE);
  case OPCODE_AGET_SHORT:
    return CseLocation(CseSpecialLocations::ARRAY_COMPONENT_TYPE_SHORT);
  case OPCODE_AGET_OBJECT:
    return CseLocation(CseSpecialLocations::ARRAY_COMPONENT_TYPE_OBJECT);
  case OPCODE_AGET_BOOLEAN:
    return CseLocation(CseSpecialLocations::ARRAY_COMPONENT_TYPE_BOOLEAN);
  default:
    always_assert(false);
  }
}

CseLocation get_read_location(const IRInstruction* insn) {
  if (is_aget(insn->opcode())) {
    return get_read_array_location(insn->opcode());
  } else if (is_iget(insn->opcode()) || is_sget(insn->opcode())) {
    return get_field_location(insn->opcode(), insn->get_field());
  } else {
    return CseLocation(CseSpecialLocations::GENERAL_MEMORY_BARRIER);
  }
}

static const char* pure_method_names[] = {
    "Ljava/lang/Boolean;.booleanValue:()Z",
    "Ljava/lang/Boolean;.equals:(Ljava/lang/Object;)Z",
    "Ljava/lang/Boolean;.getBoolean:(Ljava/lang/String;)Z",
    "Ljava/lang/Boolean;.hashCode:()I",
    "Ljava/lang/Boolean;.toString:()Ljava/lang/String;",
    "Ljava/lang/Boolean;.toString:(Z)Ljava/lang/String;",
    "Ljava/lang/Boolean;.valueOf:(Z)Ljava/lang/Boolean;",
    "Ljava/lang/Boolean;.valueOf:(Ljava/lang/String;)Ljava/lang/Boolean;",
    "Ljava/lang/Byte;.byteValue:()B",
    "Ljava/lang/Byte;.equals:(Ljava/lang/Object;)Z",
    "Ljava/lang/Byte;.toString:()Ljava/lang/String;",
    "Ljava/lang/Byte;.toString:(B)Ljava/lang/String;",
    "Ljava/lang/Byte;.valueOf:(B)Ljava/lang/Byte;",
    "Ljava/lang/Character;.valueOf:(C)Ljava/lang/Character;",
    "Ljava/lang/Character;.charValue:()C",
    "Ljava/lang/Class;.getName:()Ljava/lang/String;",
    "Ljava/lang/Class;.getSimpleName:()Ljava/lang/String;",
    "Ljava/lang/Double;.compare:(DD)I",
    "Ljava/lang/Double;.doubleValue:()D",
    "Ljava/lang/Double;.doubleToLongBits:(D)J",
    "Ljava/lang/Double;.doubleToRawLongBits:(D)J",
    "Ljava/lang/Double;.floatValue:()F",
    "Ljava/lang/Double;.hashCode:()I",
    "Ljava/lang/Double;.intValue:()I",
    "Ljava/lang/Double;.isInfinite:(D)Z",
    "Ljava/lang/Double;.isNaN:(D)Z",
    "Ljava/lang/Double;.longBitsToDouble:(J)D",
    "Ljava/lang/Double;.longValue:()J",
    "Ljava/lang/Double;.toString:(D)Ljava/lang/String;",
    "Ljava/lang/Double;.valueOf:(D)Ljava/lang/Double;",
    "Ljava/lang/Enum;.equals:(Ljava/lang/Object;)Z",
    "Ljava/lang/Enum;.name:()Ljava/lang/String;",
    "Ljava/lang/Enum;.ordinal:()I",
    "Ljava/lang/Enum;.toString:()Ljava/lang/String;",
    "Ljava/lang/Float;.doubleValue:()D",
    "Ljava/lang/Float;.floatToRawIntBits:(F)I",
    "Ljava/lang/Float;.floatValue:()F",
    "Ljava/lang/Float;.compare:(FF)I",
    "Ljava/lang/Float;.equals:(Ljava/lang/Object;)Z",
    "Ljava/lang/Float;.hashCode:()I",
    "Ljava/lang/Float;.intBitsToFloat:(I)F",
    "Ljava/lang/Float;.intValue:()I",
    "Ljava/lang/Float;.floatToIntBits:(F)I",
    "Ljava/lang/Float;.isInfinite:(F)Z",
    "Ljava/lang/Float;.isNaN:(F)Z",
    "Ljava/lang/Float;.valueOf:(F)Ljava/lang/Float;",
    "Ljava/lang/Float;.toString:(F)Ljava/lang/String;",
    "Ljava/lang/Integer;.bitCount:(I)I",
    "Ljava/lang/Integer;.byteValue:()B",
    "Ljava/lang/Integer;.compareTo:(Ljava/lang/Integer;)I",
    "Ljava/lang/Integer;.doubleValue:()D",
    "Ljava/lang/Integer;.equals:(Ljava/lang/Object;)Z",
    "Ljava/lang/Integer;.hashCode:()I",
    "Ljava/lang/Integer;.highestOneBit:(I)I",
    "Ljava/lang/Integer;.intValue:()I",
    "Ljava/lang/Integer;.longValue:()J",
    "Ljava/lang/Integer;.lowestOneBit:(I)I",
    "Ljava/lang/Integer;.numberOfLeadingZeros:(I)I",
    "Ljava/lang/Integer;.numberOfTrailingZeros:(I)I",
    "Ljava/lang/Integer;.shortValue:()S",
    "Ljava/lang/Integer;.signum:(I)I",
    "Ljava/lang/Integer;.toBinaryString:(I)Ljava/lang/String;",
    "Ljava/lang/Integer;.toHexString:(I)Ljava/lang/String;",
    "Ljava/lang/Integer;.toString:()Ljava/lang/String;",
    "Ljava/lang/Integer;.toString:(I)Ljava/lang/String;",
    "Ljava/lang/Integer;.toString:(II)Ljava/lang/String;",
    "Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;",
    "Ljava/lang/Long;.bitCount:(J)I",
    "Ljava/lang/Long;.compareTo:(Ljava/lang/Long;)I",
    "Ljava/lang/Long;.doubleValue:()D",
    "Ljava/lang/Long;.equals:(Ljava/lang/Object;)Z",
    "Ljava/lang/Long;.hashCode:()I",
    "Ljava/lang/Long;.intValue:()I",
    "Ljava/lang/Long;.highestOneBit:(J)J",
    "Ljava/lang/Long;.longValue:()J",
    "Ljava/lang/Long;.numberOfTrailingZeros:(J)I",
    "Ljava/lang/Long;.signum:(J)I",
    "Ljava/lang/Long;.toBinaryString:(J)Ljava/lang/String;",
    "Ljava/lang/Long;.toHexString:(J)Ljava/lang/String;",
    "Ljava/lang/Long;.toString:()Ljava/lang/String;",
    "Ljava/lang/Long;.toString:(J)Ljava/lang/String;",
    "Ljava/lang/Long;.valueOf:(J)Ljava/lang/Long;",
    "Ljava/lang/Math;.IEEEremainder:(DD)D",
    "Ljava/lang/Math;.abs:(J)J",
    "Ljava/lang/Math;.abs:(I)I",
    "Ljava/lang/Math;.abs:(F)F",
    "Ljava/lang/Math;.abs:(D)D",
    "Ljava/lang/Math;.acos:(D)D",
    "Ljava/lang/Math;.asin:(D)D",
    "Ljava/lang/Math;.atan:(D)D",
    "Ljava/lang/Math;.atan2:(DD)D",
    "Ljava/lang/Math;.cbrt:(D)D",
    "Ljava/lang/Math;.ceil:(D)D",
    "Ljava/lang/Math;.copySign:(FF)F",
    "Ljava/lang/Math;.copySign:(DD)D",
    "Ljava/lang/Math;.cos:(D)D",
    "Ljava/lang/Math;.cosh:(D)D",
    "Ljava/lang/Math;.exp:(D)D",
    "Ljava/lang/Math;.expm1:(D)D",
    "Ljava/lang/Math;.floor:(D)D",
    "Ljava/lang/Math;.floorDiv:(II)I",
    "Ljava/lang/Math;.floorDiv:(JJ)J",
    "Ljava/lang/Math;.floorMod:(JJ)J",
    "Ljava/lang/Math;.floorMod:(II)I",
    "Ljava/lang/Math;.getExponent:(D)I",
    "Ljava/lang/Math;.getExponent:(F)I",
    "Ljava/lang/Math;.hypot:(DD)D",
    "Ljava/lang/Math;.log:(D)D",
    "Ljava/lang/Math;.log10:(D)D",
    "Ljava/lang/Math;.log1p:(D)D",
    "Ljava/lang/Math;.max:(II)I",
    "Ljava/lang/Math;.max:(JJ)J",
    "Ljava/lang/Math;.max:(FF)F",
    "Ljava/lang/Math;.max:(DD)D",
    "Ljava/lang/Math;.min:(FF)F",
    "Ljava/lang/Math;.min:(DD)D",
    "Ljava/lang/Math;.min:(II)I",
    "Ljava/lang/Math;.min:(JJ)J",
    "Ljava/lang/Math;.nextAfter:(DD)D",
    "Ljava/lang/Math;.nextAfter:(FD)F",
    "Ljava/lang/Math;.nextDown:(D)D",
    "Ljava/lang/Math;.nextDown:(F)F",
    "Ljava/lang/Math;.nextUp:(F)F",
    "Ljava/lang/Math;.nextUp:(D)D",
    "Ljava/lang/Math;.pow:(DD)D",
    "Ljava/lang/Math;.random:()D",
    "Ljava/lang/Math;.rint:(D)D",
    "Ljava/lang/Math;.round:(D)J",
    "Ljava/lang/Math;.round:(F)I",
    "Ljava/lang/Math;.scalb:(FI)F",
    "Ljava/lang/Math;.scalb:(DI)D",
    "Ljava/lang/Math;.signum:(D)D",
    "Ljava/lang/Math;.signum:(F)F",
    "Ljava/lang/Math;.sin:(D)D",
    "Ljava/lang/Math;.sinh:(D)D",
    "Ljava/lang/Math;.sqrt:(D)D",
    "Ljava/lang/Math;.tan:(D)D",
    "Ljava/lang/Math;.tanh:(D)D",
    "Ljava/lang/Math;.toDegrees:(D)D",
    "Ljava/lang/Math;.toRadians:(D)D",
    "Ljava/lang/Math;.ulp:(D)D",
    "Ljava/lang/Math;.ulp:(F)F",
    "Ljava/lang/Object;.getClass:()Ljava/lang/Class;",
    "Ljava/lang/Short;.equals:(Ljava/lang/Object;)Z",
    "Ljava/lang/Short;.shortValue:()S",
    "Ljava/lang/Short;.toString:(S)Ljava/lang/String;",
    "Ljava/lang/Short;.valueOf:(S)Ljava/lang/Short;",
    "Ljava/lang/String;.compareTo:(Ljava/lang/String;)I",
    "Ljava/lang/String;.compareToIgnoreCase:(Ljava/lang/String;)I",
    "Ljava/lang/String;.concat:(Ljava/lang/String;)Ljava/lang/String;",
    "Ljava/lang/String;.endsWith:(Ljava/lang/String;)Z",
    "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z",
    "Ljava/lang/String;.equalsIgnoreCase:(Ljava/lang/String;)Z",
    "Ljava/lang/String;.hashCode:()I",
    "Ljava/lang/String;.indexOf:(I)I",
    "Ljava/lang/String;.isEmpty:()Z",
    "Ljava/lang/String;.indexOf:(Ljava/lang/String;)I",
    "Ljava/lang/String;.indexOf:(II)I",
    "Ljava/lang/String;.indexOf:(Ljava/lang/String;I)I",
    "Ljava/lang/String;.lastIndexOf:(I)I",
    "Ljava/lang/String;.lastIndexOf:(II)I",
    "Ljava/lang/String;.lastIndexOf:(Ljava/lang/String;)I",
    "Ljava/lang/String;.lastIndexOf:(Ljava/lang/String;I)I",
    "Ljava/lang/String;.length:()I",
    "Ljava/lang/String;.replace:(CC)Ljava/lang/String;",
    "Ljava/lang/String;.startsWith:(Ljava/lang/String;)Z",
    "Ljava/lang/String;.startsWith:(Ljava/lang/String;I)Z",
    "Ljava/lang/String;.toLowerCase:()Ljava/lang/String;",
    "Ljava/lang/String;.toLowerCase:(Ljava/util/Locale;)Ljava/lang/String;",
    "Ljava/lang/String;.toString:()Ljava/lang/String;",
    "Ljava/lang/String;.toUpperCase:()Ljava/lang/String;",
    "Ljava/lang/String;.toUpperCase:(Ljava/util/Locale;)Ljava/lang/String;",
    "Ljava/lang/String;.trim:()Ljava/lang/String;",
    "Ljava/lang/String;.valueOf:(C)Ljava/lang/String;",
    "Ljava/lang/String;.valueOf:(D)Ljava/lang/String;",
    "Ljava/lang/String;.valueOf:(F)Ljava/lang/String;",
    "Ljava/lang/String;.valueOf:(I)Ljava/lang/String;",
    "Ljava/lang/String;.valueOf:(J)Ljava/lang/String;",
    "Ljava/lang/String;.valueOf:(Z)Ljava/lang/String;",
    "Ljava/lang/System;.identityHashCode:(Ljava/lang/Object;)I",
    "Ljava/lang/Thread;.currentThread:()Ljava/lang/Thread;",
};

std::unordered_set<DexMethodRef*> get_pure_methods() {
  std::unordered_set<DexMethodRef*> pure_methods;
  for (auto const pure_method_name : pure_method_names) {
    auto method_ref = DexMethod::get_method(std::string(pure_method_name));
    if (method_ref == nullptr) {
      TRACE(CSE, 1, "[get_pure_methods]: Could not find pure method %s",
            pure_method_name);
      continue;
    }

    pure_methods.insert(method_ref);
  }
  return pure_methods;
}

MethodOverrideAction get_base_or_overriding_method_action(
    const DexMethod* method, bool ignore_methods_with_assumenosideeffects) {
  if (method == nullptr || method::is_clinit(method) ||
      method->rstate.no_optimizations()) {
    return MethodOverrideAction::UNKNOWN;
  }

  if ((method->is_virtual() && is_interface(type_class(method->get_class()))) &&
      (root(method) || !can_rename(method))) {
    // We cannot rule out that there are dynamically added classes, created via
    // Proxy.newProxyInstance, that override this method.
    // So we assume the worst.
    return MethodOverrideAction::UNKNOWN;
  }

  if (ignore_methods_with_assumenosideeffects && assumenosideeffects(method)) {
    return MethodOverrideAction::EXCLUDE;
  }

  if (method->is_external() || is_native(method)) {
    return MethodOverrideAction::UNKNOWN;
  }

  if (is_abstract(method)) {
    return MethodOverrideAction::EXCLUDE;
  }

  return MethodOverrideAction::INCLUDE;
}

bool process_base_and_overriding_methods(
    const method_override_graph::Graph* method_override_graph,
    const DexMethod* method,
    bool ignore_methods_with_assumenosideeffects,
    const std::function<bool(DexMethod*)>& handler_func) {
  auto action = get_base_or_overriding_method_action(
      method, ignore_methods_with_assumenosideeffects);
  if (action == MethodOverrideAction::UNKNOWN ||
      (action == MethodOverrideAction::INCLUDE &&
       !handler_func(const_cast<DexMethod*>(method)))) {
    return false;
  }
  if (method->is_virtual() && !(ignore_methods_with_assumenosideeffects &&
                                assumenosideeffects(method))) {
    if (!method_override_graph) {
      return false;
    }
    auto overriding_methods = method_override_graph::get_overriding_methods(
        *method_override_graph, method);
    for (auto overriding_method : overriding_methods) {
      action = get_base_or_overriding_method_action(
          overriding_method, ignore_methods_with_assumenosideeffects);
      if (action == MethodOverrideAction::UNKNOWN ||
          (action == MethodOverrideAction::INCLUDE &&
           !handler_func(const_cast<DexMethod*>(overriding_method)))) {
        return false;
      }
    }
  }

  return true;
}

size_t compute_locations_closure(
    const Scope& scope,
    const method_override_graph::Graph* method_override_graph,
    std::function<boost::optional<LocationsAndDependencies>(DexMethod*)>
        init_func,
    std::unordered_map<const DexMethod*, CseUnorderedLocationSet>* result,
    const purity::CacheConfig& cache_config) {
  // 1. Let's initialize known method read locations and dependencies by
  //    scanning method bodies
  ConcurrentMap<const DexMethod*, LocationsAndDependencies>
      concurrent_method_lads;
  walk::parallel::methods(scope, [&](DexMethod* method) {
    auto lads = init_func(method);
    if (lads) {
      concurrent_method_lads.emplace(method, *lads);
    }
  });

  std::unordered_map<const DexMethod*, LocationsAndDependencies> method_lads(
      concurrent_method_lads.begin(), concurrent_method_lads.end());

  // 2. Compute inverse dependencies so that we know what needs to be recomputed
  // during the fixpoint computation, and determine set of methods that are
  // initially "impacted" in the sense that they have dependencies.
  std::unordered_map<const DexMethod*, std::unordered_set<const DexMethod*>>
      inverse_dependencies;
  std::unordered_set<const DexMethod*> impacted_methods;
  for (const auto& p : method_lads) {
    auto method = p.first;
    auto& lads = p.second;
    if (lads.dependencies.size()) {
      for (auto d : lads.dependencies) {
        inverse_dependencies[d].insert(method);
      }
      impacted_methods.insert(method);
    }
  }

  // 3. Let's try to (semantically) inline locations, computing a fixed
  //    point. Methods for which information is directly or indirectly absent
  //    are equivalent to a general memory barrier, and are systematically
  //    pruned.

  // TODO: Instead of custom fixpoint computation using WTO, consider using the
  // MonotonicFixpointIterator, operating on a callgraph, capture the
  // dependencies, and have the Locations as the abstract domain.

  size_t iterations = 0;
  while (impacted_methods.size()) {
    iterations++;
    Timer t{std::string("Iteration ") + std::to_string(iterations)};

    // We order the impacted methods in a deterministic way that's likely
    // helping to reduce the number of needed iterations.
    std::vector<const DexMethod*> ordered_impacted_methods;

    {
      constexpr bool kCacheDebug = false;

      struct Cache {
        struct CacheData {
          size_t runs{0};
          size_t size_if_populated{0};
          std::vector<const DexMethod*> data;
        };
        std::unordered_map<const DexMethod*, CacheData> cache;
        size_t sum_entries{0};
        size_t pop_miss{0};

        const size_t cache_max_entries;
        const size_t cache_fill_entry_threshold;
        const size_t cache_fill_size_threshold;

        explicit Cache(const purity::CacheConfig& config)
            : cache_max_entries(config.max_entries),
              cache_fill_entry_threshold(config.fill_entry_threshold),
              cache_fill_size_threshold(config.fill_size_threshold) {}

        boost::optional<std::vector<const DexMethod*>> get(const DexMethod* m) {
          auto it = cache.find(m);
          if (it == cache.end()) {
            return boost::none;
          }
          redex_assert(it->second.runs > 0);
          if (it->second.runs < cache_fill_entry_threshold) {
            return boost::none;
          }
          ++it->second.runs;
          if (it->second.size_if_populated > 0) {
            // We did not have space.
            pop_miss++;
            return boost::none;
          }
          return it->second.data;
        }

        void new_data(const DexMethod* m,
                      const std::vector<const DexMethod*>& data) {
          CacheData& cd = cache[m];
          cd.runs++;
          if (cd.runs < cache_fill_entry_threshold) {
            return; // Do not cache, yet.
          }
          size_t size = data.size();
          if (size >= cache_fill_size_threshold &&
              sum_entries + size <= cache_max_entries) {
            redex_assert(cd.size_if_populated == 0);
            cd.data = data;
            sum_entries += size;
          } else {
            redex_assert(cd.size_if_populated == 0 ||
                         cd.size_if_populated == size);
            cd.size_if_populated = size;
          }
        }

        void print_stats() const {
          std::ostringstream oss;

          size_t sum_not_pop{0};
          size_t max_not_pop{0};
          size_t max_pop{0};
          size_t max_not_pop_runs{0};
          size_t max_pop_runs{0};
          size_t below_size_thresh{0};
          size_t runs_below_size_thresh{0};
          for (const auto& p : cache) {
            sum_not_pop += p.second.size_if_populated;
            max_not_pop = std::max(max_not_pop, p.second.size_if_populated);
            if (p.second.size_if_populated > 0) {
              max_not_pop_runs = std::max(max_not_pop_runs, p.second.runs);
              if (p.second.size_if_populated < cache_fill_size_threshold) {
                ++below_size_thresh;
                runs_below_size_thresh += p.second.runs;
              }
            } else {
              max_pop_runs = std::max(max_pop_runs, p.second.runs);
              max_pop = std::max(max_pop, p.second.data.size());
            }
          }
          oss << "  In=" << sum_entries << " Miss=" << pop_miss << std::endl;
          oss << "  CACHED: Max=" << max_pop << " Runs=" << max_pop_runs
              << std::endl;
          oss << " NCACHED: Sum=" << sum_not_pop << " Max=" << max_not_pop
              << " Runs=" << max_not_pop_runs << std::endl;
          oss << " NCACHED: #short=" << below_size_thresh
              << " Runs=" << runs_below_size_thresh;
          TRACE(PURITY, 1, "%s", oss.str().c_str());
        }
      };

      Cache cache(cache_config);
      sparta::WeakTopologicalOrdering<const DexMethod*> wto(
          nullptr,
          [&impacted_methods, &inverse_dependencies,
           &cache](const DexMethod* const& m) {
            auto cached = cache.get(m);
            if (cached && !kCacheDebug) {
              return std::move(*cached);
            }

            std::vector<const DexMethod*> successors;
            if (m == nullptr) {
              std::copy(impacted_methods.begin(), impacted_methods.end(),
                        std::back_inserter(successors));
            } else {
              auto it = inverse_dependencies.find(m);
              if (it != inverse_dependencies.end()) {
                for (auto n : it->second) {
                  if (impacted_methods.count(n)) {
                    successors.push_back(n);
                  }
                }
              }
            }
            // Make number of iterations deterministic
            std::sort(successors.begin(), successors.end(), compare_dexmethods);

            if (kCacheDebug && cached) {
              redex_assert(*cached == successors);
            }

            cache.new_data(m, successors);
            return successors;
          });

      if (traceEnabled(PURITY, 5)) {
        cache.print_stats();
      }

      wto.visit_depth_first([&ordered_impacted_methods](const DexMethod* m) {
        if (m) {
          ordered_impacted_methods.push_back(m);
        }
      });
      impacted_methods.clear();
    }

    std::vector<const DexMethod*> changed_methods;
    for (const DexMethod* method : ordered_impacted_methods) {
      auto& lads = method_lads.at(method);
      bool unknown = false;
      size_t lads_locations_size = lads.locations.size();
      for (const DexMethod* d : lads.dependencies) {
        if (d == method) {
          continue;
        }
        auto it = method_lads.find(d);
        if (it == method_lads.end()) {
          unknown = true;
          break;
        }
        const auto& other_locations = it->second.locations;
        lads.locations.insert(other_locations.begin(), other_locations.end());
      }
      if (unknown || lads_locations_size < lads.locations.size()) {
        // something changed
        changed_methods.push_back(method);
        if (unknown) {
          method_lads.erase(method);
        }
      }
    }

    // Given set of changed methods, determine set of dependents for which
    // we need to re-run the analysis in another iteration.
    for (auto changed_method : changed_methods) {
      auto idit = inverse_dependencies.find(changed_method);
      if (idit == inverse_dependencies.end()) {
        continue;
      }

      // remove inverse dependency entries as appropriate
      auto& entries = idit->second;
      for (auto it = entries.begin(); it != entries.end();) {
        if (!method_lads.count(*it)) {
          it = entries.erase(it);
        } else {
          it++;
        }
      }

      if (!entries.size()) {
        // remove inverse dependency
        inverse_dependencies.erase(changed_method);
      } else {
        // add inverse dependencies entries to impacted methods
        impacted_methods.insert(entries.begin(), entries.end());
      }
    }
  }

  // For all methods which have a known set of locations at this point,
  // persist that information
  for (auto& p : method_lads) {
    result->emplace(p.first, p.second.locations);
  }

  return iterations;
}

// Helper function that invokes compute_locations_closure, providing initial
// set of locations indicating whether a function only reads locations (and
// doesn't write). Via additional flags it can be selected whether...
// - [ignore_methods_with_assumenosideeffects] to ignore invoked methods that
//   are marked with assumenosideeffects
// - [for_conditional_purity] instructions that rule out conditional purity
//   should cause methods to be treated like methods with unknown behavior; in
//   particular, this rules out instructions that create new object instances,
//   as those may leak, and thus multiple invocations of such a method could
//   never be reduced by CSE.
// - [compute_locations] the actual locations that are being read are computed
//   and returned; if false, then an empty set indicates that a particular
//   function only reads (some unknown set of) locations.
static size_t analyze_read_locations(
    const Scope& scope,
    const method_override_graph::Graph* method_override_graph,
    const std::unordered_set<DexMethodRef*>& pure_methods,
    bool ignore_methods_with_assumenosideeffects,
    bool for_conditional_purity,
    bool compute_locations,
    std::unordered_map<const DexMethod*, CseUnorderedLocationSet>* result,
    const purity::CacheConfig& cache_config) {
  std::unordered_set<const DexMethod*> pure_methods_closure;
  for (auto pure_method_ref : pure_methods) {
    auto pure_method = pure_method_ref->as_def();
    if (pure_method == nullptr) {
      continue;
    }
    pure_methods_closure.insert(pure_method);
    if (pure_method->is_virtual() && method_override_graph) {
      const auto overriding_methods =
          method_override_graph::get_overriding_methods(*method_override_graph,
                                                        pure_method);
      pure_methods_closure.insert(overriding_methods.begin(),
                                  overriding_methods.end());
    }
  }

  return compute_locations_closure(
      scope, method_override_graph,
      [&](DexMethod* method) -> boost::optional<LocationsAndDependencies> {
        auto action =
            pure_methods_closure.count(method)
                ? MethodOverrideAction::EXCLUDE
                : get_base_or_overriding_method_action(
                      method, ignore_methods_with_assumenosideeffects);

        if (action == MethodOverrideAction::UNKNOWN) {
          return boost::none;
        }

        LocationsAndDependencies lads;
        if (!process_base_and_overriding_methods(
                method_override_graph, method,
                ignore_methods_with_assumenosideeffects,
                [&](DexMethod* other_method) {
                  if (other_method != method) {
                    lads.dependencies.insert(other_method);
                  }
                  return true;
                })) {
          return boost::none;
        }

        if (action == MethodOverrideAction::EXCLUDE) {
          return lads;
        }

        bool unknown = false;
        editable_cfg_adapter::iterate_with_iterator(
            method->get_code(), [&](const IRList::iterator& it) {
              auto insn = it->insn;
              auto opcode = insn->opcode();
              switch (opcode) {
              case OPCODE_MONITOR_ENTER:
              case OPCODE_MONITOR_EXIT:
              case OPCODE_FILL_ARRAY_DATA:
              case OPCODE_THROW:
                unknown = true;
                break;
              case OPCODE_NEW_ARRAY:
              case OPCODE_NEW_INSTANCE:
              case OPCODE_FILLED_NEW_ARRAY:
                if (for_conditional_purity) {
                  unknown = true;
                }
                break;
              case OPCODE_INVOKE_SUPER:
                // TODO: Support properly.
                unknown = true;
                break;
              default:
                if (is_aput(opcode) || is_iput(opcode) || is_sput(opcode)) {
                  unknown = true;
                } else if (is_aget(opcode) || is_iget(opcode) ||
                           is_sget(opcode)) {
                  auto location = get_read_location(insn);
                  if (location ==
                      CseLocation(
                          CseSpecialLocations::GENERAL_MEMORY_BARRIER)) {
                    unknown = true;
                  } else if (compute_locations) {
                    lads.locations.insert(location);
                  }
                } else if (is_invoke(opcode)) {
                  auto invoke_method = resolve_method(insn->get_method(),
                                                      opcode_to_search(opcode));
                  if (!process_base_and_overriding_methods(
                          method_override_graph, invoke_method,
                          ignore_methods_with_assumenosideeffects,
                          [&](DexMethod* other_method) {
                            if (other_method != method) {
                              lads.dependencies.insert(other_method);
                            }
                            return true;
                          })) {
                    unknown = true;
                  }
                }
                break;
              }

              return unknown ? editable_cfg_adapter::LOOP_BREAK
                             : editable_cfg_adapter::LOOP_CONTINUE;
            });

        if (unknown) {
          return boost::none;
        }

        return lads;
      },
      result, cache_config);
}

size_t compute_conditionally_pure_methods(
    const Scope& scope,
    const method_override_graph::Graph* method_override_graph,
    const std::unordered_set<DexMethodRef*>& pure_methods,
    std::unordered_map<const DexMethod*, CseUnorderedLocationSet>* result,
    const purity::CacheConfig& cache_config) {
  Timer t("compute_conditionally_pure_methods");
  auto iterations = analyze_read_locations(
      scope, method_override_graph, pure_methods,
      /* ignore_methods_with_assumenosideeffects */ false,
      /* for_conditional_purity */ true,
      /* compute_locations */ true, result, cache_config);
  for (auto& p : *result) {
    TRACE(CSE, 4, "[CSE] conditionally pure method %s: %s", SHOW(p.first),
          SHOW(&p.second));
  }
  return iterations;
}

size_t compute_no_side_effects_methods(
    const Scope& scope,
    const method_override_graph::Graph* method_override_graph,
    const std::unordered_set<DexMethodRef*>& pure_methods,
    std::unordered_set<const DexMethod*>* result,
    const purity::CacheConfig& cache_config) {
  Timer t("compute_no_side_effects_methods");
  std::unordered_map<const DexMethod*, CseUnorderedLocationSet>
      method_locations;
  auto iterations = analyze_read_locations(
      scope, method_override_graph, pure_methods,
      /* ignore_methods_with_assumenosideeffects */ true,
      /* for_conditional_purity */ false,
      /* compute_locations */ false, &method_locations, cache_config);
  for (auto& p : method_locations) {
    TRACE(CSE, 4, "[CSE] no side effects method %s", SHOW(p.first));
    result->insert(p.first);
  }
  return iterations;
}
