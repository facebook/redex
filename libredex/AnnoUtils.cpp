/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AnnoUtils.h"

namespace {

const DexEncodedValue* parse_anno_value_helper(const DexAnnotationSet* anno_set,
                                               const DexType* target_anno,
                                               DexEncodedValueTypes type,
                                               std::string elem_str) {
  auto& annos = anno_set->get_annotations();
  for (auto& anno : annos) {
    if (anno->type() != target_anno) {
      continue;
    }
    TRACE(ANNO, 9, "   anno %s\n", SHOW(anno));
    auto& elems = anno->anno_elems();
    if (elem_str.empty()) {
      always_assert(elems.size() == 1);
      auto& elem = elems[0];
      always_assert(elem.encoded_value->evtype() == type);
      auto val = elem.encoded_value->value();
      TRACE(ANNO, 9, " parsed annotation value: %d\n", val);
      return elem.encoded_value;
    }

    for (auto& elem : elems) {
      if (strcmp(elem.string->c_str(), elem_str.c_str()) != 0) {
        continue;
      }
      always_assert(elem.encoded_value->evtype() == type);
      TRACE(ANNO, 9, " parsed annotation elem: %d\n", SHOW(elem.encoded_value));
      return elem.encoded_value;
    }
  }

  always_assert_log(false,
                    " Unable to parse annotation value of %s\non %s\n",
                    elem_str.c_str(),
                    SHOW(anno_set));
}

template <class DexMember>
uint32_t parse_anno_value(const DexMember* member,
                          const DexType* target_anno,
                          DexEncodedValueTypes type,
                          std::string elem_str = "") {
  auto anno_set = member->get_anno_set();
  always_assert(anno_set != nullptr);
  TRACE(ANNO,
        9,
        " Parsing annotations elem %s on %s: %s\n",
        elem_str.c_str(),
        SHOW(member),
        SHOW(anno_set));
  auto val = parse_anno_value_helper(anno_set, target_anno, type, elem_str);
  return val->value();
}

template <class DexMember>
std::string parse_str_anno_value(const DexMember* member,
                                 const DexType* target_anno,
                                 DexEncodedValueTypes type,
                                 std::string elem_str = "") {
  always_assert(target_anno);
  always_assert(type == DEVT_STRING);
  auto anno_set = member->get_anno_set();
  always_assert(anno_set != nullptr);
  TRACE(ANNO,
        9,
        " Parsing annotations elem %s on %s: %s\n",
        elem_str.c_str(),
        SHOW(member),
        SHOW(anno_set));
  auto val = parse_anno_value_helper(anno_set, target_anno, type, elem_str);
  return std::string(
      static_cast<const DexEncodedValueString*>(val)->string()->c_str());
}

} // namespace

bool parse_bool_anno_value(const DexMethod* method,
                           const DexType* target_anno,
                           std::string name) {
  auto val = parse_anno_value(method, target_anno, DEVT_BOOLEAN, name);
  return static_cast<bool>(val);
}

uint32_t parse_int_anno_value(const DexMethod* method,
                              const DexType* target_anno,
                              std::string name) {
  auto val = parse_anno_value(method, target_anno, DEVT_INT, name);
  return static_cast<uint32_t>(val);
}

uint32_t parse_int_anno_value(const DexClass* cls,
                              const DexType* target_anno,
                              std::string name) {
  auto val = parse_anno_value(cls, target_anno, DEVT_INT, name);
  return static_cast<uint32_t>(val);
}

std::string parse_str_anno_value(const DexMethod* method,
                                 const DexType* target_anno,
                                 std::string name) {
  return parse_str_anno_value(method, target_anno, DEVT_STRING, name);
}
