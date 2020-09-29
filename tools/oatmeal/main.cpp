/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OatmealUtil.h"
#include "dump-oat.h"
#include "memory-accounter.h"
#include "vdex.h"

#include <getopt.h>

#ifndef ANDROID
#include <wordexp.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstdlib>
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

  // if true, write elf file, else write bare oat file.
  bool write_elf = false;
  bool one_oat_per_dex = false;
  std::vector<std::string> oat_files;
  std::vector<DexInput> dex_files;

  std::string oat_version;

  bool dump_classes = false;
  bool dump_code = false;
  bool dump_tables = false;
  bool dump_memory_usage = false;

  bool print_unverified_classes = false;

  std::string arch;

  std::string art_image_location;

  std::string quick_data_location;

  bool test_is_oatmeal = false;

  // generate samsung compatible oat file.
  bool samsung_mode = false;
};

#ifndef ANDROID
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
#else
// We don't expand ~ in paths on android
std::string expand(const std::string& path) { return path; }
#endif

Arguments parse_args(int argc, char* argv[]) {

  struct option options[] = {
      {"dump", no_argument, nullptr, 'd'},
      {"build", no_argument, nullptr, 'b'},
      {"write-elf", no_argument, nullptr, 'e'},
      {"dex", required_argument, nullptr, 'x'},
      {"dex-location", required_argument, nullptr, 'l'},
      {"oat", required_argument, nullptr, 'o'},
      {"oat-version", required_argument, nullptr, 'v'},
      {"dump-classes", no_argument, nullptr, 'c'},
      {"dump-code", no_argument, nullptr, 'w'},
      {"dump-tables", no_argument, nullptr, 't'},
      {"dump-memory-usage", no_argument, nullptr, 'm'},
      {"print-unverified-classes", no_argument, nullptr, 'p'},
      {"arch", required_argument, nullptr, 'a'},
      {"art-image-location", required_argument, nullptr, 0},
      {"test-is-oatmeal", no_argument, nullptr, 1},
      {"samsung-oatformat", no_argument, nullptr, 2},
      {"one-oat-per-dex", no_argument, nullptr, 3},
      {"quickening-data", required_argument, nullptr, 'q'},
      {nullptr, 0, nullptr, 0}};

  Arguments ret;
  std::vector<std::string> dex_files;
  std::vector<std::string> dex_locations;

  int c;
  while ((c = getopt_long(
              argc, argv, "cetmpdbx:l:o:v:a:", &options[0], nullptr)) != -1) {
    switch (c) {
    case 'd':
      if (ret.action != Action::DUMP && ret.action != Action::NONE) {
        fprintf(stderr, "Only one of --dump, --build may be set\n");
        exit(1);
      }
      ret.action = Action::DUMP;
      break;

    case 'e':
      ret.write_elf = true;
      break;

    case 'p':
      ret.print_unverified_classes = true;
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
      ret.oat_files.push_back(expand(optarg));
      break;

    case 'x':
      dex_files.push_back(expand(optarg));
      break;

    case 'l':
      dex_locations.push_back(optarg);
      break;

    case 'c':
      ret.dump_classes = true;
      break;

    case 'w':
      ret.dump_code = true;
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

    case 0:
      ret.art_image_location = optarg;
      break;

    case 1:
      ret.test_is_oatmeal = true;
      break;

    case 2:
      ret.samsung_mode = true;
      break;

    case 3:
      ret.one_oat_per_dex = true;
      break;

    case 'q':
      ret.quick_data_location = expand(optarg);
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

  if (ret.action != Action::DUMP && ret.print_unverified_classes) {
    fprintf(stderr,
            "-p/--print-unverified-classes can only be used with -d/--dump\n");
    exit(1);
  }

  if (!dex_locations.empty()) {
    if (dex_locations.size() != dex_files.size()) {
      fprintf(
          stderr,
          "ERROR: number of -l arguments must match number of -x arguments.\n");
      exit(1);
    }

    foreach_pair(dex_files,
                 dex_locations,
                 [&](const std::string& file, const std::string& loc) {
                   ret.dex_files.push_back(DexInput{file, loc});
                 });
  } else {
    for (const auto& f : dex_files) {
      ret.dex_files.push_back(DexInput{f, f});
    }
  }

  return ret;
}

int dump(const Arguments& args) {
  if (args.oat_files.size() != 1) {
    fprintf(stderr, "-o/--oat required (exactly once)\n");
    return 1;
  }

  auto const& oat_file_name = args.oat_files[0];
  auto oat_file = FileHandle(fopen(oat_file_name.c_str(), "r"));
  if (oat_file.get() == nullptr) {
    fprintf(stderr,
            "failed to open file %s %s\n",
            oat_file_name.c_str(),
            std::strerror(errno));
    return 1;
  }

  auto oat_file_size = get_filesize(oat_file);

  // We don't run dumping during install on device, so it is allowed to consume
  // lots of memory.
  auto oat_file_contents = std::make_unique<char[]>(oat_file_size);
  auto oatFileBytesRead =
      fread(oat_file_contents.get(), 1, oat_file_size, oat_file.get());
  if (oatFileBytesRead != oat_file_size) {
    fprintf(stderr,
            "Failed to read file %s (%zd)\n",
            std::strerror(errno),
            oatFileBytesRead);
    return 1;
  }

  ConstBuffer oatfile_buffer{oat_file_contents.get(), oat_file_size};
  auto ma_scope = MemoryAccounter::NewScope(oatfile_buffer);

  CHECK(oatfile_buffer.len > 4);
  if (*(reinterpret_cast<const uint32_t*>(oatfile_buffer.ptr)) ==
      kVdexMagicNum) {
    auto vdexfile = VdexFile::parse(oatfile_buffer);
    vdexfile->print();
    return 0;
  }
  auto oatfile =
      OatFile::parse(oatfile_buffer, args.dex_files, args.test_is_oatmeal);

  if (!oatfile) {
    fprintf(stderr, "Cannot open .oat file %s\n", oat_file_name.c_str());
    return 1;
  }

  if (args.test_is_oatmeal) {
    return oatfile->created_by_oatmeal();
  }

  oatfile->print(
      args.dump_classes, args.dump_tables, args.print_unverified_classes);

  if (args.dump_memory_usage) {
    cur_ma()->print();
  }

  return oatfile->status() == OatFile::Status::PARSE_SUCCESS ? 0 : 1;
}

int build(const Arguments& args) {

  if (args.dex_files.empty()) {
    fprintf(stderr, "one or more `-x dexfile` args required.\n");
    return 1;
  }

  if (args.one_oat_per_dex) {
    if (args.oat_files.size() != args.dex_files.size()) {
      fprintf(stderr,
              "--one-oat-per-dex was set, so number of -o args (oat files) "
              "must match number of -x args (dex files).\n");
      return 1;
    }
  } else {
    if (args.oat_files.size() != 1) {
      fprintf(stderr, "-o/--oat required (exactly once)\n");
      return 1;
    }
  }

  if (args.oat_version.empty()) {
    fprintf(stderr, "-v is required. valid versions: 079\n");
    return 1;
  }

  OatFile::build(args.oat_files,
                 args.dex_files,
                 args.oat_version,
                 args.arch,
                 args.write_elf,
                 args.art_image_location,
                 args.samsung_mode,
                 args.quick_data_location);

  return 0;
}

} // namespace

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
