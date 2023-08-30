/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

class DexClass;
class DexLocation;
class DexField;
class DexMethod;

#include "boost/variant.hpp"

#include "ConfigFiles.h"

#include <functional>
#include <string_view>

namespace JarLoaderUtil {
uint32_t read32(uint8_t*& buffer, uint8_t* buffer_end);
uint32_t read16(uint8_t*& buffer, uint8_t* buffer_end);
}; // namespace JarLoaderUtil

using attribute_hook_t =
    std::function<void(boost::variant<DexField*, DexMethod*> field_or_method,
                       const std::string_view& attribute_name,
                       uint8_t* attribute_pointer,
                       uint8_t* attribute_pointer_end)>;

namespace jar_loader {

using duplicate_allowed_hook_t =
    std::function<bool(const DexClass*, const std::string&)>;

// Legacy implementation for other clients.
bool default_duplicate_allow_fn(const DexClass* c, const std::string& jar_name);

} // namespace jar_loader

bool load_jar_file(const DexLocation* location,
                   Scope* classes = nullptr,
                   const attribute_hook_t& attr_hook = nullptr,
                   const jar_loader::duplicate_allowed_hook_t& is_allowed =
                       jar_loader::default_duplicate_allow_fn);

bool load_class_file(const std::string& filename, Scope* classes = nullptr);

void init_basic_types();
bool process_jar(const DexLocation* location,
                 const uint8_t* mapping,
                 size_t size,
                 Scope* classes,
                 const attribute_hook_t& attr_hook,
                 const jar_loader::duplicate_allowed_hook_t& is_allowed);

bool parse_class(uint8_t* buffer,
                 size_t buffer_size,
                 Scope* classes,
                 attribute_hook_t attr_hook,
                 const jar_loader::duplicate_allowed_hook_t& is_allowed,
                 const DexLocation* jar_location);
