/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ProguardPrintConfiguration.h"
#include "ProguardReporting.h"

#include <ostream>
#include <sstream>

namespace {
std::string show_keep_style(const keep_rules::KeepSpec& keep_rule) {
  if (keep_rule.mark_classes && !keep_rule.mark_conditionally &&
      !keep_rule.allowshrinking) {
    return "-keep";
  }
  if (!keep_rule.mark_classes && !keep_rule.mark_conditionally &&
      !keep_rule.allowshrinking) {
    return "-keepclassmembers";
  }
  if (!keep_rule.mark_classes && keep_rule.mark_conditionally &&
      !keep_rule.allowshrinking) {
    return "-keepclasseswithmembers";
  }
  if (keep_rule.mark_classes && !keep_rule.mark_conditionally &&
      keep_rule.allowshrinking) {
    return "-keepnames";
  }
  if (!keep_rule.mark_classes && !keep_rule.mark_conditionally &&
      keep_rule.allowshrinking) {
    return "-keepclassmembernames";
  }
  if (!keep_rule.mark_classes && keep_rule.mark_conditionally &&
      keep_rule.allowshrinking) {
    return "-keepclasseswithmembernames";
  }
  return "-invalidkeep";
}

std::string show_keep_modifiers(const keep_rules::KeepSpec& keep_rule) {
  std::string modifiers;
  if (keep_rule.allowoptimization) {
    modifiers += ",allowoptimization";
  }
  if (keep_rule.allowobfuscation) {
    modifiers += ",allowobfuscation";
  }
  if (keep_rule.includedescriptorclasses) {
    modifiers += ",includedescriptorclasses";
  }
  return modifiers;
}
} // namespace

std::string keep_rules::show_keep(const KeepSpec& keep_rule, bool show_source) {
  std::ostringstream text;
  text << show_keep_style(keep_rule) << show_keep_modifiers(keep_rule) << " "
       << keep_rule;
  if (show_source) {
    std::ostringstream source;
    source << keep_rule.source_filename << ":" << keep_rule.source_line;
    return '\'' + text.str() + "\' from " + source.str();
  }
  return text.str();
}

std::string keep_rules::show_assumenosideeffect(const KeepSpec& keep_rule,
                                                bool show_source) {
  std::ostringstream text;
  text << "-assumenosideeffects " << keep_rule;
  if (show_source) {
    std::ostringstream source;
    source << keep_rule.source_filename << ":" << keep_rule.source_line;
    return '\'' + text.str() + "\' from " + source.str();
  }
  return text.str();
}

void keep_rules::show_configuration(
    std::ostream& output,
    const Scope& classes,
    const keep_rules::ProguardConfiguration& config) {
  for (const auto& keep : config.keep_rules) {
    output << keep_rules::show_keep(*keep) << std::endl;
  }
}
