/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
#include <iostream>
#include <json/json.h>

#include "CommentFilter.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexPosition.h"
#include "DexUtil.h"
#include "IRMetaIO.h"
#include "InstructionLowering.h"
#include "JarLoader.h"
#include "Macros.h"
#include "Show.h"
#include "Timer.h"
#include "Walkers.h"

#if IS_WINDOWS
#include <io.h>
#endif

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
  ostrm << entry_data;
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
void write_intermediate_dex(const RedexOptions& redex_options,
                            ConfigFiles& conf,
                            const std::string& output_ir_dir,
                            DexStoresVector& stores,
                            Json::Value& dex_files) {
  Timer write_int_dex_timer("Write intermediate dex");
  {
    Timer t("Instruction lowering");
    instruction_lowering::run(stores);
  }
  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make(""));
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
      std::string filename =
          redex::get_dex_output_name(output_ir_dir, store, i);
      auto gtypes = std::make_shared<GatheredTypes>(&store.get_dexen()[i]);
      write_classes_to_dex(redex_options,
                           filename,
                           &store.get_dexen()[i],
                           std::move(gtypes),
                           /* locator_index= */ nullptr,
                           store_number,
                           &store.get_name(),
                           i,
                           conf,
                           pos_mapper.get(),
                           /* method_to_id= */ nullptr,
                           /* code_debug_lines= */ nullptr,
                           /* iodi_metadata= */ nullptr,
                           stores[0].get_dex_magic());
      auto basename = boost::filesystem::path(filename).filename().string();
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
    std::string store_name = store_files["name"].asString();
    DexStore store(store_name);
    stores.emplace_back(std::move(store));
    for (const Json::Value& file_name : store_files["list"]) {
      auto location = boost::filesystem::path(input_ir_dir);
      location /= file_name.asString();
      // `string().c_str()` to get guaranteed `const char*`.
      DexClasses classes = load_classes_from_dex(
          DexLocation::make_location(store_name, location.string()),
          &dex_stats);
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

static void assert_dex_magic_consistency(const std::string& source,
                                         const std::string& target) {
  always_assert_log(source.compare(target) == 0,
                    "APK contains dex file of different versions: %s vs %s\n",
                    source.c_str(), target.c_str());
}

bool is_zip(const std::string& filename) {
  char buffer[2];
  std::ifstream infile(filename);
  always_assert(infile);
  infile.read(buffer, 2);
  // the first two bytes of a ZIP file are usually "PK"
  return buffer[0] == 'P' && buffer[1] == 'K';
}
} // namespace

namespace redex {

bool dir_is_writable(const std::string& dir) {
  if (!boost::filesystem::is_directory(dir)) {
    return false;
  }
#if IS_WINDOWS
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
void write_all_intermediate(ConfigFiles& conf,
                            const std::string& output_ir_dir,
                            const RedexOptions& redex_options,
                            DexStoresVector& stores,
                            Json::Value& entry_data) {
  Timer t("Dumping all");
  redex_options.serialize(entry_data);
  entry_data["dex_list"] = Json::arrayValue;
  write_ir_meta(output_ir_dir, stores);
  write_intermediate_dex(redex_options, conf, output_ir_dir, stores,
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
  if (!(*entry_data).get("jars", Json::nullValue).empty()) {
    for (const Json::Value& item : (*entry_data)["jars"]) {
      const std::string jar_path = item.asString();
      always_assert(load_jar_file(DexLocation::make_location("", jar_path),
                                  &external_classes));
    }
  }

  init_ir_meta(stores);
  if (!load_ir_meta(input_ir_dir)) {
    std::string error =
        "Use default IR meta instead. The process result may be greatly "
        "different from the result of running whole optimization passes with "
        "redex-all\n";
    std::cerr << error;
    TRACE_NO_LINE(MAIN, 1, "%s", error.c_str());
  }
}

/**
 * Helper to load classes from a list of input dex files into a DexStoresVector.
 * Processes dex (.dex) files as well as DexMetadata files (.json)
 */
void load_classes_from_dexes_and_metadata(
    const std::vector<std::string>& dex_files,
    DexStoresVector& stores,
    dex_stats_t& input_totals,
    std::vector<dex_stats_t>& input_dexes_stats) {
  always_assert_log(!stores.empty(),
                    "Cannot load classes into empty DexStoresVector");
  for (const auto& filename : dex_files) {
    if (filename.size() >= 5 &&
        filename.compare(filename.size() - 4, 4, ".dex") == 0) {
      auto location = DexLocation::make_location("dex", filename);
      assert_dex_magic_consistency(stores[0].get_dex_magic(),
                                   load_dex_magic_from_dex(location));
      dex_stats_t dex_stats;
      DexClasses classes = load_classes_from_dex(location, &dex_stats);
      input_totals += dex_stats;
      input_dexes_stats.push_back(dex_stats);
      stores[0].add_classes(std::move(classes));
    } else if (is_zip(filename)) {
      std::cerr << "error: Input files are expected to be DEX (with filename "
                   "ending in "
                   ".dex), or a JSON metadata file. However, \""
                << filename
                << "\" is a ZIP. If this is an APK, please extract "
                   "the DEX files from it and pass those as the inputs.";
      exit(EXIT_FAILURE);
    } else {
      DexMetadata store_metadata;
      store_metadata.parse(filename);
      DexStore store(store_metadata);
      for (const auto& file_path : store_metadata.get_files()) {
        auto location = DexLocation::make_location(store.get_name(), file_path);
        assert_dex_magic_consistency(stores[0].get_dex_magic(),
                                     load_dex_magic_from_dex(location));
        dex_stats_t dex_stats;
        DexClasses classes = load_classes_from_dex(location, &dex_stats);

        input_totals += dex_stats;
        input_dexes_stats.push_back(dex_stats);
        store.add_classes(std::move(classes));
      }
      stores.emplace_back(std::move(store));
    }
  }
}

/**
 * Helper to get the output name of a specific dex file when a series of dex
 * files are being output by redex programs.
 * Index corresponds to the position in the order dex files are passed into
 * the redex programs: classes.dex -> 0, classes2.dex -> 1, classes3.dex -> 2...
 */
std::string get_dex_output_name(const std::string& output_dir,
                                const DexStore& store,
                                int index) {
  return output_dir + "/" + dex_name(store, index);
}
} // namespace redex
