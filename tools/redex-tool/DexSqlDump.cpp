/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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

#include "ControlFlow.h"
#include "DexOutput.h"
#include "Resolver.h"
#include "Show.h"
#include "Tool.h"
#include "Transform.h"
#include "Walkers.h"

#include <boost/algorithm/string/replace.hpp>
#include <queue>
#include <vector>
#include <unordered_map>

namespace {

static std::unordered_map<DexClass*, int> class_ids;
static std::unordered_map<DexMethod*, int> method_ids;
static std::unordered_map<DexField*, int> field_ids;
static std::unordered_map<DexString*, int> string_ids;

void dump_method_refs(FILE* fdout, DexMethod* method, int method_id) {
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
        fprintf(
          fdout,
          "INSERT INTO method_string_refs VALUES (%d, %d, %d, %d);\n",
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
        fprintf(
          fdout,
          "INSERT INTO method_class_refs VALUES (%d, %d, %d, %d);\n",
          next_class_ref++,
          method_id,
          class_id,
          insn->opcode());
      }
    }
    if (insn->has_field()) {
      auto field = insn->get_field();
      if (!field->is_def()) {
        field = resolve_field(field);
        if (!field) {
          field = insn->get_field();
        }
      }
      if (field_ids.count(field)) {
        auto field_id = field_ids[insn->get_field()];
        fprintf(
          fdout,
          "INSERT INTO method_field_refs VALUES (%d, %d, %d, %d);\n",
          next_field_ref++,
          method_id,
          field_id,
          insn->opcode());
      }
    }
    if (insn->has_method()) {
      auto meth = insn->get_method();
      if (!meth->is_def()) {
        meth = resolve_method(meth, MethodSearch::Any);
        if (!meth) {
          meth = insn->get_method();
        }
      }
      if (method_ids.count(meth)) {
        auto method_ref_id = method_ids[insn->get_method()];
        fprintf(
          fdout,
          "INSERT INTO method_method_refs VALUES (%d, %d, %d, %d);\n",
          next_method_ref++,
          method_id,
          method_ref_id,
          insn->opcode());
      }
    }
  }
}

void dump_class(FILE* fdout, const char* dex_id, DexClass* cls, int class_id) {
  // TODO: annotations?
  // TODO: inheritance?
  // TODO: string usage
  // TODO: size estimate
  fprintf(
    fdout,
    "INSERT INTO classes VALUES (%d,'%s','%s','%s',%u);\n",
    class_id,
    dex_id,
    cls->get_deobfuscated_name().c_str(),
    cls->get_name()->c_str(),
    cls->get_access()
  );
}

void dump_field(FILE* fdout, int class_id, DexField* field, int field_id) {
  // TODO: more fixup here on this crapped up name/signature
  // TODO: break down signature
  // TODO: annotations?
  // TODO: string usage (encoded_value for static fields)
  auto field_name = strchr(field->get_deobfuscated_name().c_str(), ';');
  fprintf(
    fdout,
    "INSERT INTO fields VALUES(%d, %d, '%s', '%s', %u);\n",
    field_id,
    class_id,
    field_name,
    field->get_name()->c_str(),
    field->get_access()
  );
}

void dump_method(FILE* fdout, int class_id, DexMethod* method, int method_id) {
  // TODO: more fixup here on this crapped up name/signature
  // TODO: break down signature
  // TODO: throws?
  // TODO: annotations?
  // TODO: string usage
  // TODO: size estimate
  auto method_name = strchr(method->get_deobfuscated_name().c_str(), ';');
  fprintf(
    fdout,
    "INSERT INTO methods VALUES (%d,%d,'%s','%s',%d,%lu);\n",
    method_id,
    class_id,
    method_name,
    method->get_name()->c_str(),
    method->get_access(),
    method->get_code() ? method->get_code()->sum_opcode_sizes() : 0
  );
}

void dump_sql(FILE* fdout, DexStoresVector& stores, ProguardMap& pg_map) {
  fprintf(
    fdout,
R"___(
DROP TABLE IF EXISTS method_string_refs;
DROP TABLE IF EXISTS method_field_refs;
DROP TABLE IF EXISTS method_method_refs;
DROP TABLE IF EXISTS method_class_refs;
DROP TABLE IF EXISTS methods;
DROP TABLE IF EXISTS strings;
DROP TABLE IF EXISTS fields;
DROP TABLE IF EXISTS is_a;
DROP TABLE IF EXISTS methods;
DROP TABLE IF EXISTS classes;
CREATE TABLE classes (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  dex TEXT NOT NULL, -- dex identifiers look like "<store>/<dex_id>"
  name TEXT NOT NULL,
  obfuscated_name TEXT NOT NULL,
  access INTEGER NOT NULL
);
CREATE TABLE methods (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  class_id INTEGER, -- fk:classes.id
  name TEXT NOT NULL,
  obfuscated_name TEXT NOT NULL,
  access INTEGER NOT NULL,
  code_size INTEGER NOT NULL
);
CREATE TABLE is_a (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  class_id INTEGER, -- fk:classes.id
  is_a_class_id INTEGER -- fk:classes.id
);
CREATE TABLE strings (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  text TEXT NOT NULL
);
CREATE TABLE fields (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  class_id INTEGER, -- fk:classes.id
  name TEXT NOT NULL,
  obfuscated_name TEXT NOT NULL,
  access INTEGER NOT NULL
);
CREATE TABLE method_class_refs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  method_id INTEGER, -- fk:methods.id
  ref_class_id INTEGER NOT NULL, -- fk:classes.id
  opcode INTEGER NOT NULL
);
CREATE TABLE method_method_refs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  method_id INTEGER, -- fk:methods.id
  ref_method_id INTEGER NOT NULL, -- fk:methods.id
  opcode INTEGER NOT NULL
);
CREATE TABLE method_field_refs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  method_id INTEGER, -- fk:methods.id
  ref_field_id INTEGER NOT NULL, -- fk:fields.id
  opcode INTEGER NOT NULL
);
CREATE TABLE method_string_refs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  method_id INTEGER, -- fk:methods.id
  ref_string_id INTEGER NOT NULL, -- fk:strings.id
  opcode INTEGER NOT NULL
);
)___"
  );
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
    for (size_t dex_idx = 0 ; dex_idx < dexen.size() ; ++dex_idx) {
      auto& dex = dexen[dex_idx];
      GatheredTypes gtypes(&dex);
      auto strings = gtypes.get_cls_order_dexstring_emitlist();
      for (auto dexstr : strings) {
        int id = next_string_id++;
        string_ids[dexstr] = id;
        // Escape string before inserting. ' -> ''
        std::string esc(dexstr->c_str());
        boost::replace_all(esc, "'", "''");
        fprintf(fdout, "INSERT INTO strings VALUES(%d, '%s');\n", id, esc.c_str());
      }
      std::string dex_id_str(store_name + "/" + std::to_string(dex_idx));
      const char* dex_id = dex_id_str.c_str();
      for (const auto& cls : dex) {
        int class_id = next_class_id++;
        dump_class(fdout, dex_id, cls, class_id);
        class_ids[cls] = class_id;
        for (auto field : cls->get_ifields()) {
          int field_id = next_field_id++;
          field_ids[field] = field_id;
          dump_field(fdout, class_id, field, field_id);
        }
        for (auto field : cls->get_sfields()) {
          int field_id = next_field_id++;
          field_ids[field] = field_id;
          dump_field(fdout, class_id, field, field_id);
        }
        for (const auto& meth : cls->get_dmethods()) {
          int meth_id = next_method_id++;
          method_ids[meth] = meth_id;
          dump_method(fdout, class_id, meth, meth_id);
        }
        for (auto& meth : cls->get_vmethods()) {
          int meth_id = next_method_id++;
          method_ids[meth] = meth_id;
          dump_method(fdout, class_id, meth, meth_id);
        }
      }
    }
  }
  fprintf(fdout, "END TRANSACTION;\n");

  // Dump references
  fprintf(fdout, "BEGIN TRANSACTION;\n");
  for (auto& store : stores) {
    auto& dexen = store.get_dexen();
    for (size_t dex_idx = 0 ; dex_idx < dexen.size() ; ++dex_idx) {
      auto& dex = dexen[dex_idx];
      for (const auto& cls : dex) {
        for (const auto& meth : cls->get_dmethods()) {
          int meth_id = method_ids[meth];
          dump_method_refs(fdout, meth, meth_id);
        }
        for (auto& meth : cls->get_vmethods()) {
          int meth_id = method_ids[meth];
          dump_method_refs(fdout, meth, meth_id);
        }
      }
    }
  }
  fprintf(fdout, "END TRANSACTION;\n");

  // Dump hierarchy
  auto scope = build_class_scope(stores);
  int next_is_a_id = 0;
  fprintf(fdout, "BEGIN TRANSACTION;\n");
  for (auto& cls : scope) {
    std::unordered_set<const DexType*> results;
    get_all_children_and_implementors(scope, cls, &results);
    for(auto type : results) {
      auto type_cls = type_class(type);
      if (type_cls) {
        fprintf(
          fdout,
          "INSERT INTO is_a VALUES(%d, %d, %d);\n",
          next_is_a_id++,
          class_ids[type_cls],
          class_ids[cls]
        );
      }
    }
  }
  fprintf(fdout, "END TRANSACTION;\n");
}

class DexSqlDump : public Tool {
 public:
  DexSqlDump() : Tool("dex-sql-dump", "dump an apk to a sql insertion script") {}

  void add_options(po::options_description& options) const override {
    add_standard_options(options);
    options.add_options()
      ("proguard-map,p",
       po::value<std::string>()->value_name("redex-rename-map.txt"),
       "path to a rename map")
      ("output,o",
       po::value<std::string>()->value_name("dex.sql"),
       "path to output sql dump file (defaults to stdout)")
    ;
  }

  void run(const po::variables_map& options) override {
    auto stores = init(
      options["jars"].as<std::string>(),
      options["apkdir"].as<std::string>(),
      options["dexendir"].as<std::string>());
    ProguardMap pgmap(options.count("proguard-map") ?
      options["proguard-map"].as<std::string>() : "/dev/null");
    auto filename = options["output"].as<std::string>().c_str();
    FILE* fdout = options.count("output") ?
      fopen(filename, "w") : stdout;
    if (!fdout) {
      fprintf(stderr, "Could not open %s for writing; terminating\n", filename);
      exit(EXIT_FAILURE);
    }
    dump_sql(fdout, stores, pgmap);
    fclose(fdout);
  }
};

static DexSqlDump s_tool;

}
