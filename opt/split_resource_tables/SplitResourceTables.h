/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

using SwitchIndices = std::set<int>;

/**
 * This pass is meant to reduce the amount of disk space that resources.arsc
 * file takes up in an application with many different configurations /
 * languages / supported API levels. Conceptually, has a similar motivation to
 * what aapt2 can do with sparse encoding of resource type data, except that
 * this strategy does not rely on binary search and has no known restrictions on
 * old API levels (it has been tested as far back as Android 4.0 / API 14, and
 * probably works even further back).
 *
 * WHAT DOES IT ACTUALLY DO?
 * For a given set of allowed resource type names (i.e. "dimen", "layout",
 * "style" etc) this pass will edit the resources.arsc file (the "resource
 * table") to create additional types that are more space efficient than the
 * original.
 *
 * Example: consider an application with 500 layout files, two of them with an
 * override for landscape mode. By default, this results in 4 bytes of overhead
 * for every layout file that does not have a value in landscape config. To
 * lessen the overhead, this pass will create a new type in the resource table
 * called "layout2" and relocate the 498 layout entries/values that do not have
 * landscape override, so that there will be no wasted space in entry offsets.
 * The original "layout" type will still be kept, but compacted to only contain
 * two entries (thus making specific / seldom used configs not pay a high
 * penalty).
 *
 * HOW TO USE IT SAFELY:
 * The strategy employed by the pass tries to be as transparent as it can, but,
 * it can very well break application logic. Normal usage from .xml files and
 * Java code (via R.string.foo) will not be impacted by this pass, but other
 * APIs, paticularly ones that are doing dynamic access require interventions.
 *
 * The pass offers the ability to wrap certain APIs with a compat method, namely
 * android.content.res.Resources.getIdentifier(). If your application is making
 * use of this method, and you still want to split the type, you can provide a
 * static method that will loop over the names of the split types created by the
 * pass (see "getidentifier_compat_method") to check one by one (not recommended
 * for performance sensitive situations).
 *
 * Other things to watch out for:
 * 1) android.content.res.Resources.getResourceTypeName() is tricky. If you are
 * using this method to format a URI (as in
 * https://developer.android.com/reference/android/content/ContentResolver#the-android.resource-scheme_android_resource-scheme_1),
 * you still want to split the type, you must use the split type name; i.e.
 * android.resource://package_name/drawable2/my_image
 *
 * BUT, if application logic is using getResourceTypeName() for equality
 * checks/comparisons against the normal set of resource type names, you must
 * refactor your code accordingly!
 *
 * 2) For resource URIs, be aware that certain Android APIs might persist a URI
 * to a resource (with its name and type). Thus, moving an image from drawable
 * type to drawable2, or a sound from raw type to raw2 type might break things
 * across app updates. Audit the APIs the application uses and configure
 * "allowed_types" accordingly.
 *
 * CONSTRAINTS:
 * As specified in OptimizeResources.h, this pass requires that resource
 * identifier values have not been inlined throughout the dex code in the
 * application. So, this pass must happen before FinalInlinePass /
 * FinalInlinePassV2.
 *
 * OTHER NOTES:
 * This pass's name is a misnomer; it is actually creating resource types (i.e.
 * ResTable_typeSpec and ResTable_type structures) not a new table.
 *
 * This pass works against .aab input files, but requires a version of
 * bundletool 1.10.0 or newer to actually produce an .apk file.
 */
class SplitResourceTablesPass : public Pass {
 public:
  SplitResourceTablesPass() : Pass("SplitResourceTablesPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
    };
  }

  void bind_config() override {
    bind("allowed_types", {}, m_allowed_types);
    bind("static_ids", "", m_static_ids_file_path);
    bind("getidentifier_compat_method", "", m_getidentifier_compat_method);
    bind("typename_compat_method", "", m_typename_compat_method);
    // TODO(T44504426) coercer should assert the non-negativity of the parsed
    // values
    bind("split_threshold", 50, m_split_threshold);
    bind("max_splits_per_type", 5, m_max_splits_per_type);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool is_type_allowed(const std::string& type_name);
  std::unordered_set<std::string> m_allowed_types;
  std::string m_static_ids_file_path;
  std::string m_getidentifier_compat_method;
  std::string m_typename_compat_method;
  size_t m_split_threshold;
  size_t m_max_splits_per_type;
};
