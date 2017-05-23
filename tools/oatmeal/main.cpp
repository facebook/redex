/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */





#include "dump-oat.h"
#include "memory-accounter.h"
#include "util.h"

#include <getopt.h>
#include <wordexp.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>

#include <string>
#include <vector>

namespace {

enum class Action {
  DUMP,
  BUILD,
  NONE,
};

struct Arguments {
  Action action = Action::NONE;
  std::string oat_file;
  std::vector<std::string> dex_files;

  std::string oat_version;

  bool dump_classes = false;
  bool dump_tables = false;
  bool dump_memory_usage = false;

  std::string arch;
};

std::string expand(const std::string& path) {
  wordexp_t exp_result;
  std::string ret;
  if (wordexp(path.c_str(), &exp_result, 0) == 0) {
    ret = std::string(exp_result.we_wordv[0]);
  } else {
    ret = path;
  }
  wordfree(&exp_result);
  return ret;
}

Arguments parse_args(int argc, char* argv[]) {

  struct option options[] = {{"dump", no_argument, nullptr, 'd'},
                             {"build", no_argument, nullptr, 'b'},
                             {"dex", required_argument, nullptr, 'x'},
                             {"oat", required_argument, nullptr, 'o'},
                             {"oat-version", required_argument, nullptr, 'v'},
                             {"dump-classes", no_argument, nullptr, 'c'},
                             {"dump-tables", no_argument, nullptr, 't'},
                             {"dump-memory-usage", no_argument, nullptr, 'm'},
                             {"arch", required_argument, nullptr, 'a'},
                             {nullptr, 0, nullptr, 0}};

  Arguments ret;

  char c;
  while ((c = getopt_long(argc, argv, "ctmdbx:o:v:", &options[0], nullptr)) !=
         -1) {
    switch (c) {
    case 'd':
      if (ret.action != Action::DUMP && ret.action != Action::NONE) {
        fprintf(stderr, "Only one of --dump, --build may be set\n");
        exit(1);
      }
      ret.action = Action::DUMP;
      break;

    case 'b':
      if (ret.action != Action::BUILD && ret.action != Action::NONE) {
        fprintf(stderr, "Only one of --dump, --build may be set\n");
        exit(1);
      }
      ret.action = Action::BUILD;
      break;

    case 'a':
      ret.arch = optarg;
      break;

    case 'o':
      if (!ret.oat_file.empty()) {
        fprintf(stderr, "--oat may only be set once.");
        exit(1);
      }
      ret.oat_file = expand(optarg);
      break;

    case 'x':
      ret.dex_files.push_back(expand(optarg));
      break;

    case 'c':
      ret.dump_classes = true;
      break;

    case 't':
      ret.dump_tables = true;
      break;

    case 'm':
      ret.dump_memory_usage = true;
      break;

    case 'v':
      ret.oat_version = optarg;
      break;

    case ':':
      fprintf(stderr, "ERROR: %s requires an argument\n", argv[optind - 1]);
      exit(1);
      break;

    default:
      fprintf(stderr, "invalid arguments.\n");
      exit(1);
      break;
    }
  }
  return ret;
}

int dump(const Arguments& args) {
  if (args.oat_file.empty()) {
    fprintf(stderr, "-o/--oat required\n");
    return 1;
  }

  auto file = FileHandle(fopen(args.oat_file.c_str(), "r"));
  if (file.get() == nullptr) {
    fprintf(stderr,
            "failed to open file %s %s\n",
            args.oat_file.c_str(),
            std::strerror(errno));
    return 1;
  }

  auto file_size = get_filesize(file);

  // We don't run dumping during install on device, so it is allowed to consume
  // lots
  // of memory.
  std::unique_ptr<char[]> file_contents(new char[file_size]);
  auto bytesRead = fread(file_contents.get(), 1, file_size, file.get());
  if (bytesRead != file_size) {
    fprintf(stderr,
            "Failed to read file %s (%lu)\n",
            std::strerror(errno),
            bytesRead);
    return 1;
  }

  ConstBuffer buf{file_contents.get(), file_size};
  auto ma_scope = MemoryAccounter::NewScope(buf);

  auto oatfile = OatFile::parse(buf);
  oatfile->print(args.dump_classes, args.dump_tables);

  if (args.dump_memory_usage) {
    cur_ma()->print();
  }

  return oatfile->status() == OatFile::Status::PARSE_SUCCESS ? 0 : 1;
}

int build(const Arguments& args) {
  if (args.oat_file.empty()) {
    fprintf(stderr, "-o/--oat required\n");
    return 1;
  }

  if (args.dex_files.empty()) {
    fprintf(stderr, "one or more `-x dexfile` args required.\n");
    return 1;
  }

  if (args.oat_version.empty()) {
    fprintf(stderr, "-v is required. valid versions: 079\n");
    return 1;
  }

  OatFile::build(args.oat_file, args.dex_files, args.oat_version, args.arch);

  for (const auto& f : args.dex_files) {
    printf(" got dex %s\n", f.c_str());
  }
  return 0;
}

}

int main(int argc, char* argv[]) {

  auto args = parse_args(argc, argv);

  switch (args.action) {
  case Action::BUILD:
    return build(args);
  case Action::DUMP:
    return dump(args);
  case Action::NONE:
    fprintf(stderr, "Please specify --dump or --build\n");
    return 1;
  }
}
