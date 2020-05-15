/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*

Workflow:

$ ./native/redex/redex.py -u <APK>
$ buck run  //native/redex:redex-tool -- dex-sql-dump  \
     --apkdir <APKDIR> --dexendir <DEXEN_DIR> \
     --jars <ANDROID_JAR> --proguard-map <RENAME_MAP> \
     --output dex.sql
$ sqlite3 dex.db < dex.sql
$ sqlite3 dex.db "SELECT COUNT(*) FROM dex;"   # verify sane-looking value
$ ./native/redex/tools/redex-tool/DexSqlQuery.py dex.db
<..enter queries..>

*/

#include <boost/algorithm/string/replace.hpp>
#include <queue>
#include <unordered_map>
#include <vector>

#include "ClassHierarchy.h"
#include "ControlFlow.h"
#include "DexOutput.h"
#include "IRCode.h"
#include "Resolver.h"
#include "Show.h"
#include "Tool.h"
#include "Walkers.h"

namespace {

static std::unordered_map<DexClass*, int> class_ids;
static std::unordered_map<DexMethod*, int> method_ids;
static std::unordered_map<DexField*, int> field_ids;
static std::unordered_map<DexString*, int> string_ids;

void dump_field_refs(FILE* fdout,
                     const char* prefix,
                     DexField* field,
                     int field_id) {
  static int next_string_ref = 0;
  auto* static_value = field->get_static_value();
  if (!static_value || (static_value->evtype() != DEVT_STRING)) return;
  auto* static_string_value = static_cast<DexEncodedValueString*>(static_value);
  auto string_id = string_ids[static_string_value->string()];
  fprintf(fdout,
          "INSERT INTO %sfield_string_refs VALUES (%d, %d, %d);\n",
          prefix,
          next_string_ref++,
          field_id,
          string_id);
}

void dump_method_refs(FILE* fdout,
                      const char* prefix,
                      DexMethod* method,
                      int method_id) {
  auto code = method->get_code();
  if (!code) return;

  static int next_string_ref = 0;
  static int next_class_ref = 0;
  static int next_field_ref = 0;
  static int next_method_ref = 0;

  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn->has_string()) {
      if (string_ids.count(insn->get_string())) {
        auto string_id = string_ids[insn->get_string()];
        fprintf(fdout,
                "INSERT INTO %smethod_string_refs VALUES (%d, %d, %d, %d);\n",
                prefix,
                next_string_ref++,
                method_id,
                string_id,
                insn->opcode());
      }
    }
    if (insn->has_type()) {
      auto cls = type_class(insn->get_type());
      if (cls && class_ids.count(cls)) {
        auto class_id = class_ids[cls];
        fprintf(fdout,
                "INSERT INTO %smethod_class_refs VALUES (%d, %d, %d, %d);\n",
                prefix,
                next_class_ref++,
                method_id,
                class_id,
                insn->opcode());
      }
    }
    if (insn->has_field()) {
      auto field = resolve_field(insn->get_field());
      if (field != nullptr && field_ids.count(field)) {
        auto field_id = field_ids[field];
        fprintf(fdout,
                "INSERT INTO %smethod_field_refs VALUES (%d, %d, %d, %d);\n",
                prefix,
                next_field_ref++,
                method_id,
                field_id,
                insn->opcode());
      }
    }
    if (insn->has_method()) {
      auto meth =
          resolve_method(insn->get_method(), opcode_to_search(insn), method);
      if (meth != nullptr && method_ids.count(meth)) {
        auto method_ref_id = method_ids[meth];
        fprintf(fdout,
                "INSERT INTO %smethod_method_refs VALUES (%d, %d, %d, %d);\n",
                prefix,
                next_method_ref++,
                method_id,
                method_ref_id,
                insn->opcode());
      }
    }
  }
}

void dump_class(FILE* fdout,
                const char* prefix,
                const char* dex_id,
                DexClass* cls,
                int class_id) {
  // TODO: annotations?
  // TODO: inheritance?
  // TODO: string usage
  // TODO: size estimate
  const auto& deobfuscated_name = cls->get_deobfuscated_name();
  fprintf(fdout,
          "INSERT INTO %sclasses VALUES (%d,'%s','%s','%s',%u);\n",
          prefix,
          class_id,
          dex_id,
          deobfuscated_name.c_str(),
          cls->get_name()->c_str(),
          cls->get_access());
}

void dump_field(FILE* fdout,
                const char* prefix,
                int class_id,
                DexField* field,
                int field_id) {
  // TODO: more fixup here on this crapped up name/signature
  // TODO: break down signature
  // TODO: annotations?
  // TODO: string usage (encoded_value for static fields)
  const auto& deobfuscated_name = field->get_deobfuscated_name();
  auto field_name = strchr(deobfuscated_name.c_str(), ';');
  fprintf(fdout,
          "INSERT INTO %sfields VALUES(%d, %d, '%s', '%s', %u);\n",
          prefix,
          field_id,
          class_id,
          field_name,
          field->get_name()->c_str(),
          field->get_access());
}

void dump_method(FILE* fdout,
                 const char* prefix,
                 int class_id,
                 DexMethod* method,
                 int method_id) {
  // TODO: more fixup here on this crapped up name/signature
  // TODO: break down signature
  // TODO: throws?
  // TODO: annotations?
  // TODO: string usage
  // TODO: size estimate
  auto deobfuscated_name = method->get_deobfuscated_name();
  auto method_name = strchr(deobfuscated_name.c_str(), ';');
  fprintf(fdout,
          "INSERT INTO %smethods VALUES (%d,%d,'%s','%s',%d,%lu);\n",
          prefix,
          method_id,
          class_id,
          method_name,
          method->get_name()->c_str(),
          method->get_access(),
          method->get_code() ? method->get_code()->sum_opcode_sizes() : 0);
}

void dump_sql(FILE* fdout,
              DexStoresVector& stores,
              ProguardMap& pg_map,
              const char* prefix) {
  fprintf(fdout,
          R"___(
DROP TABLE IF EXISTS %1$sfield_string_refs;
DROP TABLE IF EXISTS %1$smethod_string_refs;
DROP TABLE IF EXISTS %1$smethod_field_refs;
DROP TABLE IF EXISTS %1$smethod_method_refs;
DROP TABLE IF EXISTS %1$smethod_class_refs;
DROP TABLE IF EXISTS %1$sstrings;
DROP TABLE IF EXISTS %1$sfields;
DROP TABLE IF EXISTS %1$sis_a;
DROP TABLE IF EXISTS %1$smethods;
DROP TABLE IF EXISTS %1$sclasses;
CREATE TABLE %1$sclasses (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  dex TEXT NOT NULL, -- dex identifiers look like "<store>/<dex_id>"
  name TEXT NOT NULL,
  obfuscated_name TEXT NOT NULL,
  access INTEGER NOT NULL
);
CREATE TABLE %1$smethods (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  class_id INTEGER, -- fk:classes.id
  name TEXT NOT NULL,
  obfuscated_name TEXT NOT NULL,
  access INTEGER NOT NULL,
  code_size INTEGER NOT NULL
);
CREATE TABLE %1$sis_a (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  class_id INTEGER, -- fk:classes.id
  is_a_class_id INTEGER -- fk:classes.id
);
CREATE TABLE %1$sstrings (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  text TEXT NOT NULL
);
CREATE TABLE %1$sfields (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  class_id INTEGER, -- fk:classes.id
  name TEXT NOT NULL,
  obfuscated_name TEXT NOT NULL,
  access INTEGER NOT NULL
);
CREATE TABLE %1$sfield_string_refs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  field_id INTEGER NOT NULL, -- fk:fields.id
  ref_string_id INTEGER NOT NULL -- fk:strings.id
);
CREATE TABLE %1$smethod_class_refs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  method_id INTEGER, -- fk:methods.id
  ref_class_id INTEGER NOT NULL, -- fk:classes.id
  opcode INTEGER NOT NULL
);
CREATE TABLE %1$smethod_method_refs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  method_id INTEGER, -- fk:methods.id
  ref_method_id INTEGER NOT NULL, -- fk:methods.id
  opcode INTEGER NOT NULL
);
CREATE TABLE %1$smethod_field_refs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  method_id INTEGER, -- fk:methods.id
  ref_field_id INTEGER NOT NULL, -- fk:fields.id
  opcode INTEGER NOT NULL
);
CREATE TABLE %1$smethod_string_refs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  method_id INTEGER, -- fk:methods.id
  ref_string_id INTEGER NOT NULL, -- fk:strings.id
  opcode INTEGER NOT NULL
);
)___",
          prefix);
  int next_class_id = 0;
  int next_method_id = 0;
  int next_field_id = 0;
  int next_string_id = 0;

  // Dump all dex items
  fprintf(fdout, "BEGIN TRANSACTION;\n");
  for (auto& store : stores) {
    auto store_name = store.get_name();
    auto& dexen = store.get_dexen();
    apply_deobfuscated_names(dexen, pg_map);
    for (size_t dex_idx = 0; dex_idx < dexen.size(); ++dex_idx) {
      auto& dex = dexen[dex_idx];
      GatheredTypes gtypes(&dex);
      auto strings = gtypes.get_cls_order_dexstring_emitlist();
      for (auto dexstr : strings) {
        int id = next_string_id++;
        string_ids[dexstr] = id;
        // Escape string before inserting. ' -> ''
        std::string esc(dexstr->c_str());
        boost::replace_all(esc, "'", "''");
        fprintf(fdout,
                "INSERT INTO %sstrings VALUES(%d, '%s');\n",
                prefix,
                id,
                esc.c_str());
      }
      std::string dex_id_str(store_name + "/" + std::to_string(dex_idx));
      const char* dex_id = dex_id_str.c_str();
      for (const auto& cls : dex) {
        int class_id = next_class_id++;
        dump_class(fdout, prefix, dex_id, cls, class_id);
        class_ids[cls] = class_id;
        for (auto field : cls->get_ifields()) {
          int field_id = next_field_id++;
          field_ids[field] = field_id;
          dump_field(fdout, prefix, class_id, field, field_id);
        }
        for (auto field : cls->get_sfields()) {
          int field_id = next_field_id++;
          field_ids[field] = field_id;
          dump_field(fdout, prefix, class_id, field, field_id);
        }
        for (const auto& meth : cls->get_dmethods()) {
          int meth_id = next_method_id++;
          method_ids[meth] = meth_id;
          dump_method(fdout, prefix, class_id, meth, meth_id);
        }
        for (auto& meth : cls->get_vmethods()) {
          int meth_id = next_method_id++;
          method_ids[meth] = meth_id;
          dump_method(fdout, prefix, class_id, meth, meth_id);
        }
      }
    }
  }
  fprintf(fdout, "END TRANSACTION;\n");

  // Dump references
  fprintf(fdout, "BEGIN TRANSACTION;\n");
  for (auto& store : stores) {
    auto& dexen = store.get_dexen();
    for (size_t dex_idx = 0; dex_idx < dexen.size(); ++dex_idx) {
      auto& dex = dexen[dex_idx];
      for (const auto& cls : dex) {
        for (const auto& meth : cls->get_dmethods()) {
          int meth_id = method_ids[meth];
          dump_method_refs(fdout, prefix, meth, meth_id);
        }
        for (auto& meth : cls->get_vmethods()) {
          int meth_id = method_ids[meth];
          dump_method_refs(fdout, prefix, meth, meth_id);
        }
        for (const auto& field : cls->get_sfields()) {
          int field_id = field_ids[field];
          dump_field_refs(fdout, prefix, field, field_id);
        }
        for (const auto& field : cls->get_ifields()) {
          int field_id = field_ids[field];
          dump_field_refs(fdout, prefix, field, field_id);
        }
      }
    }
  }
  fprintf(fdout, "END TRANSACTION;\n");

  // Dump hierarchy
  auto scope = build_class_scope(stores);
  ClassHierarchy ch = build_type_hierarchy(scope);
  int next_is_a_id = 0;
  fprintf(fdout, "BEGIN TRANSACTION;\n");
  for (auto& cls : scope) {
    TypeSet results;
    get_all_children_or_implementors(ch, scope, cls, results);
    for (auto type : results) {
      auto type_cls = type_class(type);
      if (type_cls) {
        fprintf(fdout,
                "INSERT INTO %sis_a VALUES(%d, %d, %d);\n",
                prefix,
                next_is_a_id++,
                class_ids[type_cls],
                class_ids[cls]);
      }
    }
  }
  fprintf(fdout, "END TRANSACTION;\n");
}

class DexSqlDump : public Tool {
 public:
  DexSqlDump()
      : Tool("dex-sql-dump", "dump an apk to a sql insertion script") {}

  void add_options(po::options_description& options) const override {
    add_standard_options(options);
    options.add_options()(
        "proguard-map,p",
        po::value<std::string>()->value_name("redex-rename-map.txt"),
        "path to a rename map")(
        "output,o",
        po::value<std::string>()->value_name("dex.sql"),
        "path to output sql dump file (defaults to stdout)")(
        "table-prefix,t",
        po::value<std::string>()->value_name("pre_"),
        "prefix to use on all table names");
  }

  void run(const po::variables_map& options) override {
    auto stores = init(options["jars"].as<std::string>(),
                       options["apkdir"].as<std::string>(),
                       options["dexendir"].as<std::string>());
    ProguardMap pgmap(options.count("proguard-map")
                          ? options["proguard-map"].as<std::string>()
                          : "/dev/null");
    const std::string& filename = options["output"].as<std::string>();
    FILE* fdout =
        options.count("output") ? fopen(filename.c_str(), "w") : stdout;
    std::string prefix = options.count("table-prefix")
                             ? options["table-prefix"].as<std::string>()
                             : "";
    if (!fdout) {
      fprintf(stderr,
              "Could not open %s for writing; terminating\n",
              filename.c_str());
      exit(EXIT_FAILURE);
    }
    auto* pfx_cstr = prefix.c_str();
    dump_sql(fdout, stores, pgmap, pfx_cstr);
    fclose(fdout);
  }
};

static DexSqlDump s_tool;

} // namespace
