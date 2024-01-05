/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "Pass.h"
#include "PassManager.h"
#include "Trace.h"

/*
 * Current D8 rewrites API 19 Objects.requireNonNull method into Method
 * java/lang/Object.getClass:()Ljava/lang/Class;. However, during this
 * conversion, D8 just replaces the former invoke-statck to later invoke-virtual
 * and ingores the return value. In Redex,
 * "java/lang/Object.getClass:()Ljava/lang/Class;" is viewed as a purity method.
 * Therefore, with this D8 conversion, Redex will opt out
 * java/lang/Object.getClass:()Ljava/lang/Class; and lose the null check
 * semantic.  Therefore, in this Pass,  a D8 desugred getClass() (i.e no
 * move-object followed by) will be converted into a explict redex null_check
 * method "Lredex/$NullCheck;.null_check:(Ljava/lang/Object;)V" to keep null
 * checking feature. Redex null-check analysis will remove some redandent redex
 * null check. Then at the end  of  redex optimizaiton,
 * MaterializeNullChecksPass will convert the rest redex null_check method back
 * to getClass().
 */
class IntrinsifyNullChecksPass : public Pass {
 public:
  struct Stats {
    size_t num_of_obj_getClass{0};
    size_t num_of_convt_getClass{0};
    size_t num_of_null_check{0};
    Stats& operator+=(const Stats& that) {
      num_of_obj_getClass += that.num_of_obj_getClass;
      num_of_convt_getClass += that.num_of_convt_getClass;
      num_of_null_check += that.num_of_null_check;
      return *this;
    }
    /* Updates metrics tracked by \p mgr corresponding to these statistics. */
    void report(PassManager& mgr) const {
      mgr.incr_metric("num_of_obj_getClass", num_of_obj_getClass);
      mgr.incr_metric("num_of_convt_getClass", num_of_convt_getClass);
      mgr.incr_metric("num_of_null_check", num_of_null_check);
      TRACE(NCI, 1, "Number of object getClass = %zu\n", num_of_obj_getClass);
      TRACE(NCI, 1, "Number of converted getClass = %zu\n",
            num_of_convt_getClass);
    }
  };

  explicit IntrinsifyNullChecksPass() : Pass("IntrinsifyNullChecksPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {HasSourceBlocks, Preserves},
        {NoSpuriousGetClassCalls, Establishes},
        {UltralightCodePatterns, Preserves},
    };
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  Stats convert_getClass(DexMethod* method);

 private:
  /* Create a helper class for null check.*/
  void create_null_check_class(DexStoresVector* stores);

  Stats m_stats;
  DexMethodRef* m_getClass_ref;
  DexMethodRef* m_NPE_ref;
  DexMethodRef* m_null_check_ref;
  DexType* m_NPE_type;
};
