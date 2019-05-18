/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include <boost/optional.hpp>

#include "Debug.h"
#include "JsonWrapper.h"

class DexMethod;
class DexType;

struct ConfigurableReflection {
  enum Type {
    // Primtives are types we support intrinsically, e.g. scalars or arrays of
    // scalars.
    // The primitives we support are defined by DEFINE_CONFIGURABLE_PRIMITIVE
    // macros
    // at the bottom of this file.
    PRIMITIVE,
    // Composites are types that are made up recursively of other Configurables,
    // e.g.
    // key/value pairs. Composite types are typically ones which derive from
    // Configurabl.
    COMPOSITE,
  };
  std::string name;
  std::string doc;
  // N.B. this tuple, which was supposed to be a variant, is now turning into a
  // struct, meh
  std::map<std::string,
           std::tuple<std::string, ConfigurableReflection, Type, std::string>>
      params;
};

/**
 * HOWTO Use Configurable
 *
 * // Derive from Configurable
 * class MyConfigurablePass : public Configurable {
 * public:
 *  // Override get_config_name to give your class a human readable name for
 * reflection std::string get_config_name() override { return
 * "MyConfigurablePass";
 *  }
 *  // Override get_config_doc to provide a docoumentation string explaining the
 *  // overall purpose of the Configurable (e.g. your pass)
 *  std::string get_config_doc() override {
 *    return "Shrink the app by doing xyz"
 *  };
 *  // Override bind_config to declare all the parameters on your Configurable
 *  void bind_config() override {
 *    // Bind the parameter named "param_name" to param_field, with a default
 *    // value of default_param_value, should the parameter be absent when
 * parsing
 *    // the config. The 4th parameter is a help string explaining the purpose
 * of the
 *    // Configurable parameter.
 *    //
 *    // bind() can bind any type that has intrinsic support (see
 *    // DEFINE_CONFIGURABLE_PRIMITIVE macros at the bottom of this file), or
 * any
 *    // type itself which derives from Configurable.
 *    bind("para_name", default_param_value, &param_field, "Help doc explaining
 * param");
 *  }
 * private:
 *  param_type_t param_field;
 * }
 */
class Configurable {

 public:
  virtual ~Configurable() {}

  /** 
   * Returns the human readable name of this Configurable, as used in
   * reflection. */
  virtual std::string get_config_name() = 0;

  /** Returns help text explaining this Configurable's purpose. */
  virtual std::string get_config_doc() { return default_doc(); };

  /** 
   * Configurables should override this function in order to declare their
   * bindings.
   *
   * bind_config is called in both reflection and configuration parsing
   * scenarios; implementations should NOT assume that the code is only called
   * in order to parse the configuration.
   *
   * Typically, you should only be calling the bind() function in bind_config().
   * If you wish to execute imperative code during the configuration parsing
   * scenario, but not the reflection scenario, then call "after_configuration"
   * in bind_config. The function supplied to after_configuration will be called
   * immediately after bind_config has been called.
   */
  virtual void bind_config() {}

  /** 
   * Returns a data structure containing the schema of this Configurable. This
   * schema itself may contain nested schemas, reflecting parameters which are
   * composite.
   */
  ConfigurableReflection reflect();

  /** 
   * Apply the declared bindings in order to consume json at configuration
   * time. */
  void parse_config(const JsonWrapper& json);

  // Type aliases for convience
  using MapOfVectorOfStrings =
      std::unordered_map<std::string, std::vector<std::string>>;

 protected:
  /**
   * The provided function will be called immediately after bind_config() is
   * called, in the case where we're consuming a configuration (e.g. it /won't
   * be called/ in the case where we are merely reflecting.) So, you should use
   * the function to perform any non-declarative work, such as registering
   * plugins with interdex, performing any complicated validations or
   * transformations, etc. Typically speaking, avoid needing to use this
   * function. bind_config() is intended to be as declarative as possible.
   */
  void after_configuration(std::function<void()> after_configuration_fn) {
    always_assert_log(!m_after_configuration,
                      "after_configuration may only be called once");
    m_after_configuration = after_configuration_fn;
  }

  /** 
   * Default behavior for all json -> data type coercions. this template
   * handles the case for composites (e.g. all Configurables). Primitives
   * will have specializations provided in Configurable.cpp
   */
  template <typename T>
  static T as(const Json::Value& value) {
    static_assert(
        std::is_base_of<Configurable, T>::value,
        "T must be a supported primitive or derive from Configurable");
    T t;
    // TODO: wrapping this in json wrapper is stupid, can we use raw
    // json::values here?)
    t.parse_config(JsonWrapper{value});
    return t;
  }

  /** 
   * Default behavior for all parameter reflections. this template
   * handles the case for composites (e.g. all Configurables). Primitives
   * will have specializations provided in Configurable.cpp
   */
  template <typename T>
  void reflect(std::function<void((const std::string& param_name,
                                   const std::string& param_doc,
                                   std::tuple<std::string,
                                              ConfigurableReflection,
                                              ConfigurableReflection::Type>
                                       param_type))>& reflector,
               const std::string& name,
               const std::string& doc,
               T& param) {
    static_assert(
        std::is_base_of<Configurable, T>::value,
        "T must be a supported primitive or derive from Configurable");
    reflector(name,
              doc,
              std::make_tuple("",
                              param.reflect(),
                              ConfigurableReflection::Type::COMPOSITE));
  }

  template <typename T>
  void bind(const std::string& name,
            T defaultValue,
            T& dest,
            const std::string& doc = default_doc()) {
    reflect(m_reflector, name, doc, dest);
    parse(name, defaultValue, dest);
  }

  void bind(const std::string& name,
            const char* defaultValue,
            std::string& dest,
            const std::string& doc = default_doc()) {
    bind(name, std::string(defaultValue), dest, doc);
  }

 private:
  template <typename T>
  void parse(const std::string& name, T defaultValue, T& dest) {
    boost::optional<const Json::Value&> value = m_parser(name);
    if (value) {
      dest = Configurable::as<T>(*value);
    } else {
      dest = defaultValue;
    }
  }

 private:
  std::function<void()> m_after_configuration;
  std::function<boost::optional<const Json::Value&>(const std::string& name)>
      m_parser;
  std::function<void((const std::string& param_name,
                      const std::string& param_doc,
                      std::tuple<std::string,
                                 ConfigurableReflection,
                                 ConfigurableReflection::Type> param_type))>
      m_reflector;

  static constexpr const char* default_doc() { return "TODO: Document this"; }
};

// Specializations for primitives

#define DEFINE_CONFIGURABLE_PRIMITIVE(type)                                   \
  template <>                                                                 \
  type Configurable::as<type>(const Json::Value& value);                      \
  template <>                                                                 \
  void Configurable::reflect(                                                 \
      std::function<void(                                                     \
          (const std::string& param_name,                                     \
           const std::string& param_doc,                                      \
           std::tuple<std::string,                                            \
                      ConfigurableReflection,                                 \
                      ConfigurableReflection::Type> param_type))>& reflector, \
      const std::string& name,                                                \
      const std::string& doc,                                                 \
      type&);

DEFINE_CONFIGURABLE_PRIMITIVE(int)
DEFINE_CONFIGURABLE_PRIMITIVE(float)
DEFINE_CONFIGURABLE_PRIMITIVE(bool)
DEFINE_CONFIGURABLE_PRIMITIVE(unsigned int)
DEFINE_CONFIGURABLE_PRIMITIVE(int64_t)
DEFINE_CONFIGURABLE_PRIMITIVE(uint64_t)
DEFINE_CONFIGURABLE_PRIMITIVE(std::string)
DEFINE_CONFIGURABLE_PRIMITIVE(Json::Value)
DEFINE_CONFIGURABLE_PRIMITIVE(std::vector<std::string>)
DEFINE_CONFIGURABLE_PRIMITIVE(std::unordered_set<std::string>)
DEFINE_CONFIGURABLE_PRIMITIVE(std::vector<DexType*>)
DEFINE_CONFIGURABLE_PRIMITIVE(std::unordered_set<DexType*>)
DEFINE_CONFIGURABLE_PRIMITIVE(std::vector<DexMethod*>)
DEFINE_CONFIGURABLE_PRIMITIVE(std::unordered_set<DexMethod*>)
DEFINE_CONFIGURABLE_PRIMITIVE(Configurable::MapOfVectorOfStrings)

#undef DEFINE_CONFIGURABLE_PRIMITIVE
