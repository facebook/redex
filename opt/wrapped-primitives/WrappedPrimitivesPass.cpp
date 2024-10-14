/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WrappedPrimitivesPass.h"

#include <inttypes.h>

#include "DexUtil.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"
#include "WrappedPrimitives.h"

namespace cp = constant_propagation;
namespace mog = method_override_graph;
namespace wp = wrapped_primitives;

namespace {

constexpr const char* METRIC_CONSTS_INSERTED = "const_instructions_inserted";
constexpr const char* METRIC_CASTS_INSERTED = "check_casts_inserted";

// Check assumptions about the wrapper class's hierarchy.
void validate_wrapper_type(DexType* type) {
  auto cls = type_class(type);
  always_assert(cls != nullptr);
  always_assert_log(cls->get_interfaces()->empty(),
                    "Wrapper type %s should not implement interfaces",
                    SHOW(type));
  auto super_cls = cls->get_super_class();
  always_assert_log(super_cls == type::java_lang_Object(),
                    "Wrapper type %s should inherit from Object; got %s",
                    SHOW(type), SHOW(super_cls));
}

// A wrapped primitive is assumed to be represented by the only final primitive
// field in the wrapper class.
DexType* get_wrapped_final_field_type(DexType* type) {
  auto cls = type_class(type);
  always_assert_log(cls != nullptr, "Spec class %s not found", SHOW(type));
  std::vector<DexField*> candidates;
  for (auto& f : cls->get_ifields()) {
    if (is_final(f) && type::is_primitive(f->get_type())) {
      candidates.emplace_back(f);
    }
  }
  always_assert_log(candidates.size() == 1,
                    "Expected 1 final field of primitive type in class %s",
                    SHOW(cls));
  return candidates.at(0)->get_type();
}
} // namespace

void WrappedPrimitivesPass::bind_config() {
  std::vector<Json::Value> wrappers;
  std::vector<wp::Spec> wrapper_specs;
  bind("wrappers", {}, wrappers);
  for (auto it = wrappers.begin(); it != wrappers.end(); ++it) {
    const auto& value = *it;
    always_assert_log(value.isObject(),
                      "Wrong specification: spec in array not an object.");
    JsonWrapper json_obj = JsonWrapper(value);
    wp::Spec spec;
    std::string wrapper_desc;
    json_obj.get("wrapper", "", wrapper_desc);
    spec.wrapper = DexType::get_type(wrapper_desc);
    always_assert_log(spec.wrapper != nullptr, "Type %s does not exist",
                      wrapper_desc.c_str());
    // Ensure the wrapper type matches expectations by the pass.
    validate_wrapper_type(spec.wrapper);
    spec.primitive = get_wrapped_final_field_type(spec.wrapper);

    // Unpack an array of objects, each object is just a 1 key/value to map an
    // API using the wrapper type to the corresponding API of primitive type.
    Json::Value allowed_invokes_array;
    json_obj.get("allowed_invokes", Json::Value(), allowed_invokes_array);
    always_assert_log(
        allowed_invokes_array.isArray(),
        "Wrong specification: allowed_invokes must be an array of objects.");
    for (auto& obj : allowed_invokes_array) {
      always_assert_log(
          obj.isObject(),
          "Wrong specification: allowed_invokes must be an array of objects.");
      auto members = obj.getMemberNames();
      always_assert_log(
          members.size() == 1,
          "Wrong specification: allowed invoke object should be just 1 mapping "
          "of method ref string to method ref string.");
      auto api = members.at(0);
      TRACE(WP, 2, "Checking for API '%s'", api.c_str());
      auto wrapped_api = DexMethod::get_method(api);
      if (wrapped_api == nullptr) {
        continue;
      }
      std::string unwrapped_api_desc;
      JsonWrapper jobj = JsonWrapper(obj);
      jobj.get(api.c_str(), "", unwrapped_api_desc);
      always_assert_log(!unwrapped_api_desc.empty(), "empty!");
      TRACE(WP, 2, "Checking for unwrapped API '%s'",
            unwrapped_api_desc.c_str());
      auto unwrapped_api = DexMethod::get_method(unwrapped_api_desc);
      always_assert_log(unwrapped_api != nullptr, "Method %s does not exist",
                        unwrapped_api_desc.c_str());
      spec.allowed_invokes.emplace(wrapped_api, unwrapped_api);
      TRACE(WP, 2, "Allowed API call %s -> %s", SHOW(wrapped_api),
            SHOW(unwrapped_api));
    }
    wrapper_specs.emplace_back(spec);
  }
  wp::initialize(wrapper_specs);
  trait(Traits::Pass::unique, true);
}

void WrappedPrimitivesPass::eval_pass(DexStoresVector& stores,
                                      ConfigFiles& conf,
                                      PassManager&) {
  wp::get_instance()->mark_roots();
}

void WrappedPrimitivesPass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& /* unused */,
                                     PassManager& mgr) {
  auto wp_instance = wp::get_instance();
  wp_instance->unmark_roots();

  auto consts = wp_instance->consts_inserted();
  TRACE(WP, 1, "const instructions inserted: %zu", consts);
  mgr.set_metric(METRIC_CONSTS_INSERTED, consts);

  auto casts = wp_instance->casts_inserted();
  TRACE(WP, 1, "check-cast instructions inserted: %zu", casts);
  mgr.set_metric(METRIC_CASTS_INSERTED, casts);

  // Clear state so that no futher work gets done from multiple rounds of IPCP
  wp::initialize({});
}

static WrappedPrimitivesPass s_pass;
