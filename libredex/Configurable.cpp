/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Configurable.h"

#include "DexClass.h"

void Configurable::parse_config(const JsonWrapper& json) {
  m_reflector = [](const std::string& param_name, const std::string& param_doc,
                   std::tuple<std::string, ConfigurableReflection,
                              ConfigurableReflection::Type> param_type) {};
  m_parser = [&json](const std::string& name) {
    // TODO: add std::string API for contains
    if (json.contains(name.c_str())) {
      return boost::optional<const Json::Value&>(json[name.c_str()]);
    } else {
      return boost::optional<const Json::Value&>{};
    }
  };
  bind_config();
  if (m_after_configuration) {
    m_after_configuration();
  }
}

ConfigurableReflection Configurable::reflect() {
  ConfigurableReflection cr;
  cr.name = get_config_name();
  cr.doc = get_config_doc();
  m_parser = [](const std::string&) {
    return boost::optional<const Json::Value&>{};
  };
  // N.B. using std::tuple here, meant to evolve to use of std::variant w/ c++17
  m_reflector = [&cr](const std::string& param_name,
                      const std::string& param_doc,
                      std::tuple<std::string, ConfigurableReflection,
                                 ConfigurableReflection::Type> param_type) {
    switch (std::get<2>(param_type)) {
    case ConfigurableReflection::Type::PRIMITIVE:
      cr.params[param_name] =
          std::make_tuple(std::get<0>(param_type), ConfigurableReflection(),
                          ConfigurableReflection::Type::PRIMITIVE, param_doc);
      break;
    case ConfigurableReflection::Type::COMPOSITE:
      cr.params[param_name] =
          std::make_tuple("", std::get<1>(param_type),
                          ConfigurableReflection::Type::COMPOSITE, param_doc);
      break;
    default:
      always_assert_log(false, "Invalid ConfigurableReflection::Type: %d",
                        std::get<2>(param_type));
      break;
    }
  };
  bind_config();
  return cr;
}

template <>
int Configurable::as<int>(const Json::Value& value) {
  return value.asInt();
}

template <>
unsigned int Configurable::as<unsigned int>(const Json::Value& value) {
  return value.asUInt();
}

template <>
int64_t Configurable::as<int64_t>(const Json::Value& value) {
  return value.asInt64();
}

template <>
uint64_t Configurable::as<uint64_t>(const Json::Value& value) {
  return value.asUInt64();
}

template <>
bool Configurable::as<bool>(const Json::Value& value) {
  return value.asBool();
}

template <>
std::string Configurable::as<std::string>(const Json::Value& value) {
  return value.asString();
}

template <>
std::vector<std::string> Configurable::as<std::vector<std::string>>(
    const Json::Value& value) {
  std::vector<std::string> result;
  for (auto& str : value) {
    result.emplace_back(str.asString());
  }
  return result;
}

template <>
std::unordered_set<std::string>
Configurable::as<std::unordered_set<std::string>>(const Json::Value& value) {
  std::unordered_set<std::string> result;
  for (auto& str : value) {
    result.emplace(str.asString());
  }
  return result;
}

template <>
std::unordered_set<DexMethod*> Configurable::as<std::unordered_set<DexMethod*>>(
    const Json::Value& value) {
  std::unordered_set<DexMethod*> result;
  for (auto& str : value) {
    auto meth = DexMethod::get_method(str.asString());
    if (meth == nullptr || !meth->is_def()) continue;
    result.emplace(static_cast<DexMethod*>(meth));
  }
  return result;
}

template <>
Configurable::MapOfVectorOfStrings
Configurable::as<Configurable::MapOfVectorOfStrings>(const Json::Value& value) {
  if (!value.isObject()) {
    throw std::runtime_error("expected object, got:" + value.asString());
  }
  Configurable::MapOfVectorOfStrings result;
  for (auto it = value.begin(); it != value.end(); ++it) {
    auto k = it.key();
    auto v = *it;
    if (!k.isString()) {
      throw std::runtime_error("expected string, got:" + k.asString());
    }
    if (!v.isArray()) {
      throw std::runtime_error("expected array, got:" + v.asString());
    }
    for (auto el : v) {
      if (!el.isString()) {
        throw std::runtime_error("expected string, got:" + el.asString());
      }
      result[k.asString()].emplace_back(el.asString());
    }
  }
  return result;
}

template <>
std::unordered_set<DexType*> Configurable::as<std::unordered_set<DexType*>>(
    const Json::Value& value) {
  std::unordered_set<DexType*> result;
  for (auto& str : value) {
    auto type = DexType::get_type(str.asString());
    if (type != nullptr) {
      result.emplace(type);
    } else {
      // TODO(T43491783) : logging?
      fprintf(stderr, "WARNING: Cannot resolve type %s while parsing config\n",
              str.asString().c_str());
    }
  }
  return result;
}

template <>
Json::Value Configurable::as<Json::Value>(const Json::Value& value) {
  return value;
}

#define IMPLEMENT_REFLECTOR(type)                                             \
  template <>                                                                 \
  void Configurable::reflect(                                                 \
      std::function<void(                                                     \
          (const std::string& param_name, const std::string& param_doc,       \
           std::tuple<std::string, ConfigurableReflection,                    \
                      ConfigurableReflection::Type> param_type))>& reflector, \
      const std::string& name, const std::string& doc, type&) {               \
    reflector(name, doc,                                                      \
              std::make_tuple(std::string{#type}, ConfigurableReflection(),   \
                              ConfigurableReflection::Type::PRIMITIVE));      \
  }

#define IMPLEMENT_REFLECTOR_EX(type, type_name)                               \
  template <>                                                                 \
  void Configurable::reflect(                                                 \
      std::function<void(                                                     \
          (const std::string& param_name, const std::string& param_doc,       \
           std::tuple<std::string, ConfigurableReflection,                    \
                      ConfigurableReflection::Type> param_type))>& reflector, \
      const std::string& name, const std::string& doc, type&) {               \
    reflector(name, doc,                                                      \
              std::make_tuple(std::string{type_name},                         \
                              ConfigurableReflection(),                       \
                              ConfigurableReflection::Type::PRIMITIVE));      \
  }

IMPLEMENT_REFLECTOR(int)
IMPLEMENT_REFLECTOR(float)
IMPLEMENT_REFLECTOR(bool)
IMPLEMENT_REFLECTOR_EX(unsigned int, "int")
IMPLEMENT_REFLECTOR_EX(int64_t, "long")
IMPLEMENT_REFLECTOR_EX(uint64_t, "long")
IMPLEMENT_REFLECTOR_EX(std::string, "string")
IMPLEMENT_REFLECTOR_EX(Json::Value, "json")
IMPLEMENT_REFLECTOR_EX(std::vector<std::string>, "list")
IMPLEMENT_REFLECTOR_EX(std::unordered_set<std::string>, "set")
IMPLEMENT_REFLECTOR_EX(std::vector<DexType*>, "list")
IMPLEMENT_REFLECTOR_EX(std::unordered_set<DexType*>, "set")
IMPLEMENT_REFLECTOR_EX(std::vector<DexMethod*>, "list")
IMPLEMENT_REFLECTOR_EX(std::unordered_set<DexMethod*>, "set")
IMPLEMENT_REFLECTOR_EX(Configurable::MapOfVectorOfStrings, "dict")
