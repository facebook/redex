/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Configurable.h"

#include "DexClass.h"

template <typename T>
using optional = std::optional<T>;

#define error_or_warn(error, warn, msg, ...)         \
  always_assert_log(!(error), msg, ##__VA_ARGS__);   \
  if (warn) {                                        \
    fprintf(stderr, "WARNING: " msg, ##__VA_ARGS__); \
  }

#define ASSERT_NO_BINDFLAGS(type) \
  always_assert_log(!bindflags, "No bindflags may be specified for a " #type);

namespace {

using OptJsonVal = std::optional<std::reference_wrapper<const Json::Value>>;

// NOTE: "Leaf" parse functions return an `optional` return type to allow
//       unified checking of the value in container parsing, without having
//       to do tricks like SFINAE to have special handling for pointers
//       vs empty strings vs empty containers etc.

template <bool kCheckType>
optional<std::string> parse_str(const Json::Value& str,
                                Configurable::bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(std::string);
  if (kCheckType && !str.isString()) {
    throw std::runtime_error("Expected string, got:" + str.asString());
  }
  return str.asString();
}

optional<DexType*> parse_type(const Json::Value& str,
                              Configurable::bindflags_t bindflags) {
  assert_log(!(bindflags & ~Configurable::bindflags::types::mask),
             "Only type bindflags may be specified for a DexType*");
  if (!str.isString()) {
    throw std::runtime_error("Expected string, got:" + str.asString());
  }
  auto type = DexType::get_type(str.asString());
  if (type == nullptr) {
    error_or_warn(
        bindflags & Configurable::bindflags::types::error_if_unresolvable,
        bindflags & Configurable::bindflags::types::warn_if_unresolvable,
        "\"%s\" failed to resolve to a known type\n", str.asString().c_str());
    return std::nullopt;
  }
  return type;
}

optional<DexClass*> parse_class(const Json::Value& value,
                                Configurable::bindflags_t bindflags) {
  auto type_res = parse_type(value, bindflags);
  auto cls = type_class(type_res ? *type_res : nullptr);
  if (cls == nullptr) {
    error_or_warn(
        bindflags & Configurable::bindflags::classes::error_if_unresolvable,
        bindflags & Configurable::bindflags::classes::warn_if_unresolvable,
        "\"%s\" failed to resolve to a known class\n",
        value.asString().c_str());
    return std::nullopt;
  }
  return cls;
}

optional<DexMethodRef*> parse_method_ref(const Json::Value& str,
                                         Configurable::bindflags_t bindflags) {
  always_assert_log(!(bindflags & ~Configurable::bindflags::methods::mask),
                    "Only method bindflags may be specified for a "
                    "UnorderedMap<DexMethod*, DexMethod*>");
  if (!str.isString()) {
    throw std::runtime_error("Expected string, got:" + str.asString());
  }
  auto meth = DexMethod::get_method(str.asString());
  if (meth == nullptr) {
    error_or_warn(
        bindflags & Configurable::bindflags::methods::error_if_unresolvable,
        bindflags & Configurable::bindflags::methods::warn_if_unresolvable,
        "\"%s\" failed to resolve to a known method\n",
        str.asString().c_str());
    return std::nullopt;
  }
  return meth;
}

optional<DexMethod*> parse_method(const Json::Value& str,
                                  Configurable::bindflags_t bindflags) {
  auto meth_ref_opt = parse_method_ref(str, bindflags);
  if (!meth_ref_opt) {
    return std::nullopt;
  }
  auto meth_ref = *meth_ref_opt;
  if (!meth_ref->is_def()) {
    error_or_warn(
        bindflags & Configurable::bindflags::methods::error_if_not_def,
        bindflags & Configurable::bindflags::methods::warn_if_not_def,
        "\"%s\" resolved to a method reference\n", str.asString().c_str());
    return std::nullopt;
  }
  return meth_ref->as_def();
}

// NOTE: For parsing into containers, `std::nullopt` values of the parsing
//       function will be skipped.

// Infer the result of the parsing function.
template <typename ParseFn>
using parse_result =
    typename std::invoke_result<ParseFn,
                                const Json::Value&,
                                Configurable::bindflags_t>::type::value_type;

template <typename ParseFn>
optional<std::vector<parse_result<ParseFn>>> parse_vec(
    const Json::Value& value,
    ParseFn parse_fn,
    Configurable::bindflags_t bindflags) {
  std::vector<parse_result<ParseFn>> result;
  for (auto& v : value) {
    if (auto parsed = parse_fn(v, bindflags)) {
      result.emplace_back(std::move(*parsed));
    }
  }
  return result;
}

template <typename ParseFn>
optional<UnorderedSet<parse_result<ParseFn>>> parse_set(
    const Json::Value& value,
    ParseFn parse_fn,
    Configurable::bindflags_t bindflags) {
  UnorderedSet<parse_result<ParseFn>> result;
  for (auto& v : value) {
    if (auto parsed = parse_fn(v, bindflags)) {
      result.emplace(std::move(*parsed));
    }
  }
  return result;
}

template <typename KFn, typename VFn>
UnorderedMap<parse_result<KFn>, parse_result<VFn>> parse_map(
    const Json::Value& value,
    KFn k_parse,
    Configurable::bindflags_t k_bindflags,
    VFn v_parse,
    Configurable::bindflags_t v_bindflags) {
  if (!value.isObject()) {
    throw std::runtime_error("Expected object, got:" + value.asString());
  }
  UnorderedMap<parse_result<KFn>, parse_result<VFn>> result;
  for (auto it = value.begin(); it != value.end(); ++it) {
    auto k = k_parse(it.key(), k_bindflags);
    auto v = v_parse(*it, v_bindflags);
    if (k && v) {
      result.emplace(std::move(*k), std::move(*v));
    }
  }
  return result;
}

template <bool kCheckType>
optional<std::vector<std::string>> parse_str_vec(
    const Json::Value& value, Configurable::bindflags_t bindflags) {
  return parse_vec(value, parse_str<kCheckType>, bindflags);
}

} // namespace

std::string Configurable::trim(std::string s) {
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](auto ch) {
            return !std::isspace(ch) && ch != '\n';
          }));
  s.erase(std::find_if(s.rbegin(), s.rend(),
                       [](auto ch) { return !std::isspace(ch) && ch != '\n'; })
              .base(),
          s.end());
  return s;
}

void Configurable::parse_config(const JsonWrapper& json) {
  m_after_configuration = {};
  m_reflecting = false;
  m_param_reflector =
      [](const std::string& param_name, const std::string& param_doc,
         const bool param_is_required, const bindflags_t param_bindflags,
         const Configurable::ReflectionParam::Type param_type_tag,
         const std::tuple<std::string, Configurable::Reflection>& param_type,
         const Json::Value& default_value) {};
  m_trait_reflector = [](const std::string&, const Json::Value&) {};
  m_parser = [&json](const std::string& name) {
    // TODO: add std::string API for contains
    if (json.contains(name.c_str())) {
      return OptJsonVal(json[name.c_str()]);
    } else {
      return OptJsonVal{};
    }
  };
  bind_config();
  // m_after_configuration may have been set in bind_config()
  if (m_after_configuration) {
    m_after_configuration();
  }
}

Configurable::Reflection Configurable::reflect() {
  Configurable::Reflection cr;
  cr.name = get_config_name();
  cr.doc = get_config_doc();
  m_after_configuration = {};
  m_parser = [](const std::string&) { return OptJsonVal{}; };
  // N.B. using std::tuple here, meant to evolve to use of std::variant w/ c++17
  m_reflecting = true;
  m_param_reflector =
      [&cr](const std::string& param_name,
            const std::string& param_doc,
            const bool param_is_required,
            const bindflags_t param_bindflags,
            const Configurable::ReflectionParam::Type param_type_tag,
            const std::tuple<std::string, Configurable::Reflection>& param_type,
            const Json::Value& default_value) {
        switch (param_type_tag) {
        case Configurable::ReflectionParam::Type::PRIMITIVE:
          cr.params[param_name] = Configurable::ReflectionParam(
              param_name, param_doc, param_is_required, param_bindflags,
              std::get<Configurable::ReflectionParam::Type::PRIMITIVE>(
                  param_type),
              default_value);
          break;
        case Configurable::ReflectionParam::Type::COMPOSITE:
          cr.params[param_name] = Configurable::ReflectionParam(
              param_name, param_doc, param_is_required, param_bindflags,
              std::get<Configurable::ReflectionParam::Type::COMPOSITE>(
                  param_type));
          break;
        default:
          not_reached_log("Invalid Configurable::ReflectionParam::Type: %d",
                          param_type_tag);
          break;
        }
      };
  m_trait_reflector = [&cr](const std::string& name, const Json::Value& value) {
    cr.traits[name] = Configurable::ReflectionTrait(name, value);
  };

  bind_config();
  return cr;
}

template <>
float Configurable::as<float>(const Json::Value& value, bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(float);
  return value.asFloat();
}

template <>
int Configurable::as<int>(const Json::Value& value, bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(int);
  return value.asInt();
}

template <>
unsigned int Configurable::as<unsigned int>(const Json::Value& value,
                                            bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(unsigned int);
  return value.asUInt();
}

template <>
std::optional<int> Configurable::as<std::optional<int>>(
    const Json::Value& value, bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(unsigned int);
  return value.asInt();
}

template <>
std::optional<unsigned int> Configurable::as<std::optional<unsigned int>>(
    const Json::Value& value, bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(unsigned int);
  return value.asUInt();
}

template <>
long Configurable::as<long>(const Json::Value& value, bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(long);
  return value.asInt64();
}

template <>
unsigned long Configurable::as<unsigned long>(const Json::Value& value,
                                              bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(unsigned long);
  return value.asUInt64();
}

template <>
std::optional<long> Configurable::as<std::optional<long>>(
    const Json::Value& value, bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(unsigned long);
  return value.asInt64();
}

template <>
std::optional<unsigned long> Configurable::as<std::optional<unsigned long>>(
    const Json::Value& value, bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(unsigned long);
  return value.asUInt64();
}

template <>
long long Configurable::as<long long>(const Json::Value& value,
                                      bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(long long);
  return value.asInt64();
}

template <>
unsigned long long Configurable::as<unsigned long long>(
    const Json::Value& value, bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(unsigned long long);
  return value.asUInt64();
}

template <>
bool Configurable::as<bool>(const Json::Value& value, bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(bool);
  return value.asBool();
}
template <>
std::optional<bool> Configurable::as<std::optional<bool>>(
    const Json::Value& value, bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(unsigned int);
  return value.asBool();
}

template <>
std::string Configurable::as<std::string>(const Json::Value& value,
                                          bindflags_t bindflags) {
  return std::move(*parse_str<false>(value, bindflags));
}

template <>
std::optional<std::string> Configurable::as<std::optional<std::string>>(
    const Json::Value& value, bindflags_t bindflags) {
  always_assert_log(
      !(bindflags & ~Configurable::bindflags::optionals::skip_empty_string),
      "Only bindflags::optionals::skip_empty_string may be specified for a "
      "std::optional<std::string>");
  std::string str = value.asString();
  if (str.empty() &&
      (bindflags & Configurable::bindflags::optionals::skip_empty_string)) {
    return std::nullopt;
  } else {
    return str;
  }
}

template <>
std::vector<Json::Value> Configurable::as<std::vector<Json::Value>>(
    const Json::Value& value, bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(std::vector<Json::Value>);
  std::vector<Json::Value> result;
  for (auto& str : value) {
    result.emplace_back(str);
  }
  return result;
}

template <>
std::vector<std::string> Configurable::as<std::vector<std::string>>(
    const Json::Value& value, bindflags_t bindflags) {
  return std::move(*parse_str_vec<false>(value, bindflags));
}

template <>
std::vector<unsigned int> Configurable::as<std::vector<unsigned int>>(
    const Json::Value& value, bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(std::vector<unsigned int>);
  std::vector<unsigned int> result;
  for (const auto& str : value) {
    result.push_back(str.asUInt());
  }
  return result;
}

template <>
UnorderedSet<std::string> Configurable::as<UnorderedSet<std::string>>(
    const Json::Value& value, bindflags_t bindflags) {
  return std::move(*parse_set(value, parse_str<false>, bindflags));
}

template <>
DexType* Configurable::as<DexType*>(const Json::Value& value,
                                    bindflags_t bindflags) {
  if (auto type = parse_type(value, bindflags)) {
    return *type;
  }
  return nullptr;
}

template <>
std::vector<DexType*> Configurable::as<std::vector<DexType*>>(
    const Json::Value& value, bindflags_t bindflags) {
  return std::move(*parse_vec(value, parse_type, bindflags));
}

template <>
std::vector<DexMethod*> Configurable::as<std::vector<DexMethod*>>(
    const Json::Value& value, bindflags_t bindflags) {
  return std::move(*parse_vec(value, parse_method, bindflags));
}

template <>
UnorderedSet<DexType*> Configurable::as<UnorderedSet<DexType*>>(
    const Json::Value& value, bindflags_t bindflags) {
  return std::move(*parse_set(value, parse_type, bindflags));
}

template <>
UnorderedSet<const DexType*> Configurable::as<UnorderedSet<const DexType*>>(
    const Json::Value& value, bindflags_t bindflags) {
  return std::move(*parse_set(
      value,
      [](const Json::Value& v, bindflags_t b) {
        return std::optional<const DexType*>(parse_type(v, b));
      },
      bindflags));
}

using TypeMap = UnorderedMap<DexType*, DexType*>;

template <>
TypeMap Configurable::as<TypeMap>(const Json::Value& value,
                                  bindflags_t bindflags) {
  return parse_map(value, parse_type, bindflags, parse_type, bindflags);
}

template <>
UnorderedSet<DexClass*> Configurable::as<UnorderedSet<DexClass*>>(
    const Json::Value& value, bindflags_t bindflags) {
  return std::move(*parse_set(value, parse_class, bindflags));
}

template <>
UnorderedSet<DexMethod*> Configurable::as<UnorderedSet<DexMethod*>>(
    const Json::Value& value, bindflags_t bindflags) {
  return std::move(*parse_set(value, parse_method, bindflags));
}

using MethRefMap = UnorderedMap<DexMethodRef*, DexMethodRef*>;

template <>
MethRefMap Configurable::as<MethRefMap>(const Json::Value& value,
                                        bindflags_t bindflags) {
  return parse_map(value, parse_method_ref, bindflags, parse_method_ref,
                   bindflags);
}

template <>
Configurable::MapOfMethods Configurable::as<Configurable::MapOfMethods>(
    const Json::Value& value, bindflags_t bindflags) {
  return parse_map(value, parse_method, bindflags, parse_method, bindflags);
}

template <>
Configurable::MapOfVectorOfStrings
Configurable::as<Configurable::MapOfVectorOfStrings>(const Json::Value& value,
                                                     bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(Configurable::MapOfVectorOfStrings);
  return parse_map(value, parse_str<true>, bindflags, parse_str_vec<true>,
                   bindflags);
}

template <>
Configurable::MapOfStrings Configurable::as<Configurable::MapOfStrings>(
    const Json::Value& value, bindflags_t bindflags) {
  return parse_map(value, parse_str<true>, bindflags, parse_str<true>,
                   bindflags);
}

template <>
Json::Value Configurable::as<Json::Value>(const Json::Value& value,
                                          bindflags_t bindflags) {
  ASSERT_NO_BINDFLAGS(Json::Value);
  return value;
}

// NOLINTBEGIN(bugprone-macro-parentheses)
#define IMPLEMENT_REFLECTOR(type)                                              \
  template <>                                                                  \
  void Configurable::reflect(                                                  \
      ReflectorParamFunc& reflector, const std::string& param_name,            \
      const std::string& param_doc, const bool param_is_required,              \
      const Configurable::bindflags_t param_bindflags, type& param,            \
      type default_value) {                                                    \
    param = default_value;                                                     \
    reflector(param_name, param_doc, param_is_required, param_bindflags,       \
              Configurable::ReflectionParam::PRIMITIVE,                        \
              std::make_tuple(std::string{#type}, Configurable::Reflection()), \
              Json::nullValue);                                                \
  }

#define IMPLEMENT_REFLECTOR_EX(T, type_name)                                 \
  template <>                                                                \
  void Configurable::reflect(                                                \
      ReflectorParamFunc& reflector, const std::string& param_name,          \
      const std::string& param_doc, const bool param_is_required,            \
      const Configurable::bindflags_t param_bindflags, T& param,             \
      typename DefaultValueType<T>::type default_value) {                    \
    param = default_value;                                                   \
    reflector(                                                               \
        param_name, param_doc, param_is_required, param_bindflags,           \
        Configurable::ReflectionParam::PRIMITIVE,                            \
        std::make_tuple(std::string{type_name}, Configurable::Reflection()), \
        Json::nullValue);                                                    \
  }

#define IMPLEMENT_REFLECTOR_WITH_DFLT_VALUE(T)                              \
  template <>                                                               \
  void Configurable::reflect(                                               \
      ReflectorParamFunc& reflector, const std::string& param_name,         \
      const std::string& param_doc, const bool param_is_required,           \
      const Configurable::bindflags_t param_bindflags, T& param,            \
      typename DefaultValueType<T>::type default_value) {                   \
    param = default_value;                                                  \
    reflector(param_name, param_doc, param_is_required, param_bindflags,    \
              Configurable::ReflectionParam::PRIMITIVE,                     \
              std::make_tuple(std::string{#T}, Configurable::Reflection()), \
              Json::Value(default_value));                                  \
  }

#define IMPLEMENT_TRAIT_REFLECTOR(type)                                   \
  template <>                                                             \
  void Configurable::reflect_trait(ReflectorTraitFunc& reflector_trait,   \
                                   const std::string& name, type value) { \
    reflector_trait(name, Json::Value(value));                            \
  }
// NOLINTEND(bugprone-macro-parentheses)

IMPLEMENT_REFLECTOR(float)
IMPLEMENT_REFLECTOR_WITH_DFLT_VALUE(bool)
IMPLEMENT_REFLECTOR_EX(std::optional<bool>, "bool")
IMPLEMENT_REFLECTOR_EX(int, "int")
IMPLEMENT_REFLECTOR_EX(unsigned int, "int")
IMPLEMENT_REFLECTOR_EX(std::optional<int>, "int")
IMPLEMENT_REFLECTOR_EX(std::optional<unsigned int>, "int")
IMPLEMENT_REFLECTOR_EX(long, "long")
IMPLEMENT_REFLECTOR_EX(unsigned long, "long")
IMPLEMENT_REFLECTOR_EX(long long, "long")
IMPLEMENT_REFLECTOR_EX(unsigned long long, "long")
IMPLEMENT_REFLECTOR_EX(DexType*, "string")
IMPLEMENT_REFLECTOR_EX(std::string, "string")
IMPLEMENT_REFLECTOR_EX(Json::Value, "json")
IMPLEMENT_REFLECTOR_EX(std::vector<Json::Value>, "list")
IMPLEMENT_REFLECTOR_EX(std::optional<std::string>, "string")
IMPLEMENT_REFLECTOR_EX(std::vector<std::string>, "list")
IMPLEMENT_REFLECTOR_EX(std::vector<unsigned int>, "list")
IMPLEMENT_REFLECTOR_EX(UnorderedSet<std::string>, "set")
IMPLEMENT_REFLECTOR_EX(std::vector<DexType*>, "list")
IMPLEMENT_REFLECTOR_EX(std::vector<DexMethod*>, "list")
IMPLEMENT_REFLECTOR_EX(UnorderedSet<const DexType*>, "set")
IMPLEMENT_REFLECTOR_EX(UnorderedSet<DexType*>, "set")
IMPLEMENT_REFLECTOR_EX(UnorderedSet<DexClass*>, "set")
IMPLEMENT_REFLECTOR_EX(UnorderedSet<DexMethod*>, "set")
IMPLEMENT_REFLECTOR_EX(Configurable::MapOfVectorOfStrings, "dict")
IMPLEMENT_REFLECTOR_EX(Configurable::MapOfMethods, "dict")
IMPLEMENT_REFLECTOR_EX(Configurable::MapOfStrings, "dict")
IMPLEMENT_REFLECTOR_EX(MethRefMap, "dict")
IMPLEMENT_REFLECTOR_EX(TypeMap, "dict")

IMPLEMENT_TRAIT_REFLECTOR(bool)
IMPLEMENT_TRAIT_REFLECTOR(int)
IMPLEMENT_TRAIT_REFLECTOR(const std::string&)

#undef error_or_warn
