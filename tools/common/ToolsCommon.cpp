/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "ToolsCommon.h"

#include <boost/filesystem.hpp>
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <boost/iostreams/filtering_stream.hpp> // uses deprecated auto_ptr
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
#include <json/json.h>

#include "CommentFilter.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "IRMetaIO.h"
#include "InstructionLowering.h"
#include "JarLoader.h"
#include "Timer.h"
#include "Walkers.h"

namespace {
/**
 * Entry file contains the list of dex files, config file and original command
 * line arguments.
 */
const std::string ENTRY_FILE = "/entry.json";
void load_entry_file(const std::string& input_ir_dir, Json::Value* entry_data) {
  std::ifstream istrm(input_ir_dir + ENTRY_FILE);
  istrm >> *entry_data;
}

void write_entry_file(const std::string& output_ir_dir,
                      const Json::Value& entry_data) {
  std::ofstream ostrm(output_ir_dir + ENTRY_FILE);
  Json::StyledStreamWriter writer;
  writer.write(ostrm, entry_data);
}

/**
 * Init the IR meta to default values.
 */
void init_ir_meta(DexStoresVector& stores) {
  Timer t("Init default meta");
  Scope classes = build_class_scope(stores);
  walk::parallel::classes(classes, [](DexClass* cls) {
    cls->set_deobfuscated_name(show(cls));
    for (DexField* field : cls->get_sfields()) {
      field->set_deobfuscated_name(show(field));
    }
    for (DexField* field : cls->get_ifields()) {
      field->set_deobfuscated_name(show(field));
    }
    for (DexMethod* method : cls->get_dmethods()) {
      method->set_deobfuscated_name(show(method));
    }
    for (DexMethod* method : cls->get_vmethods()) {
      method->set_deobfuscated_name(show(method));
    }
  });
}

/**
 * Write meta data to file.
 * Development usage only
 */
void write_ir_meta(const std::string& output_ir_dir, DexStoresVector& stores) {
  Timer t("Dumping IR meta");
  Scope classes = build_class_scope(stores);
  ir_meta_io::dump(classes, output_ir_dir);
}

/**
 * Write intermediate dex to files.
 * Development usage only
 */
void write_intermediate_dex(const ConfigFiles& cfg,
                            const std::string& output_ir_dir,
                            DexStoresVector& stores,
                            Json::Value& dex_files) {
  Timer write_int_dex_timer("Write intermediate dex");
  {
    Timer t("Instruction lowering");
    instruction_lowering::run(stores);
  }
  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make("", ""));
  for (size_t store_number = 0; store_number < stores.size(); ++store_number) {
    auto& store = stores[store_number];
    Timer t("Writing intermediate dexes");

    dex_files.append(Json::nullValue);
    Json::Value& store_files = dex_files[dex_files.size() - 1];
    store_files["name"] = store.get_name();
    store_files["list"] = Json::arrayValue;

    for (size_t i = 0; i < store.get_dexen().size(); i++) {
      if (store.get_dexen()[i].empty()) {
        continue;
      }
      std::ostringstream ss;
      ss << output_ir_dir << "/" << store.get_name();
      if (store.get_name().compare("classes") == 0) {
        // primary/secondary dex store, primary has no numeral and secondaries
        // start at 2
        if (i > 0) {
          ss << (i + 1);
        }
      } else {
        // other dex stores do not have a primary,
        // so it makes sense to start at 2
        ss << (i + 2);
      }
      ss << ".dex";

      write_classes_to_dex(ss.str(),
                           &store.get_dexen()[i],
                           nullptr /* locator_index */,
                           false /* name-based locators */,
                           store_number,
                           i,
                           cfg,
                           pos_mapper.get(),
                           nullptr,
                           nullptr,
                           nullptr /* IODIMetadata* */,
                           stores[0].get_dex_magic());
      auto basename = boost::filesystem::path(ss.str()).filename().string();
      store_files["list"].append(basename);
    }
  }
}

/**
 * Load intermediate dex
 */
void load_intermediate_dex(const std::string& input_ir_dir,
                           const Json::Value& dex_files,
                           DexStoresVector& stores) {
  Timer t("Load intermediate dex");
  dex_stats_t dex_stats;
  for (const Json::Value& store_files : dex_files) {
    DexStore store(store_files["name"].asString());
    stores.emplace_back(std::move(store));
    for (const Json::Value& file_name : store_files["list"]) {
      auto location = boost::filesystem::path(input_ir_dir);
      location /= file_name.asString();
      DexClasses classes = load_classes_from_dex(location.c_str(), &dex_stats);
      stores.back().add_classes(std::move(classes));
    }
  }
}

/**
 * Load IR meta data
 */
bool load_ir_meta(const std::string& input_ir_dir) {
  Timer t("Loading IR meta");
  return ir_meta_io::load(input_ir_dir);
}
} // namespace

namespace redex {

bool dir_is_writable(const std::string& dir) {
  if (!boost::filesystem::is_directory(dir)) {
    return false;
  }
#ifdef _MSC_VER
  return _access(dir.c_str(), 2) == 0;
#else
  return access(dir.c_str(), W_OK) == 0;
#endif
}

Json::Value parse_config(const std::string& config_file) {
  std::ifstream config_stream(config_file);
  if (!config_stream) {
    std::cerr << "error: cannot find config file: " << config_file << std::endl;
    exit(EXIT_FAILURE);
  }

  boost::iostreams::filtering_istream inbuf;
  inbuf.push(CommentFilter());
  inbuf.push(config_stream);
  Json::Value ret;
  inbuf >> ret; // parse JSON
  return ret;
}

/**
 * Dumping dex, IR meta data and entry file
 */
void write_all_intermediate(const ConfigFiles& cfg,
                            const std::string& output_ir_dir,
                            const RedexOptions& redex_options,
                            DexStoresVector& stores,
                            Json::Value& entry_data) {
  Timer t("Dumping all");
  redex_options.serialize(entry_data);
  entry_data["dex_list"] = Json::arrayValue;
  write_ir_meta(output_ir_dir, stores);
  write_intermediate_dex(ConfigFiles(Json::nullValue), output_ir_dir, stores,
                         entry_data["dex_list"]);
  write_entry_file(output_ir_dir, entry_data);
}

/**
 * Loading entry file, dex files and IR meta data
 */
void load_all_intermediate(const std::string& input_ir_dir,
                           DexStoresVector& stores,
                           Json::Value* entry_data) {
  Timer t("Loading all");
  load_entry_file(input_ir_dir, entry_data);
  load_intermediate_dex(input_ir_dir, (*entry_data)["dex_list"], stores);

  // load external classes
  Scope external_classes;
  if ((*entry_data).get("jars", Json::nullValue).size()) {
    for (const Json::Value& item : (*entry_data)["jars"]) {
      const std::string jar_path = item.asString();
      always_assert(load_jar_file(jar_path.c_str(), &external_classes));
    }
  }

  init_ir_meta(stores);
  if (!load_ir_meta(input_ir_dir)) {
    std::cerr << "Load IR meta failed\n";
  }
}
} // namespace redex
