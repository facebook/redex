/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <stdio.h>
#include <sys/stat.h> // mkdir

#include <boost/program_options.hpp> // arg parsing

#include "InjectDebug.h"
#include "ToolsCommon.h" // redex::dir_is_writable

/* Main program file for injecting debug information that will be used for
 * creating a bytecode-level debugger for android.
 *
 * Parses command line args (parse_args is copied as a subset of the args used
 * in the redex-all program).
 * Calls upon the InjectDebug class to perform the dex modifications.
 */

namespace {

const std::string USAGE_HEADER = "usage: inject_debug [-o out-dir] -d dexes";

void print_usage() {
  std::cout << USAGE_HEADER << std::endl;
  std::cout << "Try 'inject_debug -h' for more information." << std::endl;
}

struct Arguments {
  std::string out_dir;
  std::vector<std::string> dex_files;
};

Arguments parse_args(int argc, char* argv[]) {
  Arguments args;
  args.out_dir = ".";

  namespace po = boost::program_options;
  po::options_description od(USAGE_HEADER);
  od.add_options()("help,h", "print this help message");
  od.add_options()("outdir,o", po::value<std::string>(),
                   "output directory for processed dex file");
  od.add_options()("dex-files,d", po::value<std::vector<std::string>>(),
                   "paths to dex files or dex metadata files");
  po::positional_options_description pod;
  pod.add("dex-files", -1);
  po::variables_map vm;

  try {
    po::store(
        po::command_line_parser(argc, argv).options(od).positional(pod).run(),
        vm);
    po::notify(vm);
  } catch (std::exception& e) {
    std::cerr << e.what() << std::endl << std::endl;
    print_usage();
    exit(EXIT_FAILURE);
  }

  if (vm.count("help")) {
    od.print(std::cout);
    exit(EXIT_SUCCESS);
  }

  if (vm.count("dex-files")) {
    args.dex_files = vm["dex-files"].as<std::vector<std::string>>();
  } else {
    std::cerr << "error: no input dex files" << std::endl << std::endl;
    print_usage();
    exit(EXIT_SUCCESS);
  }

  if (vm.count("outdir")) {
    args.out_dir = vm["outdir"].as<std::string>();
    if (!redex::dir_is_writable(args.out_dir)) {
      std::cerr << "error: outdir is not a writable directory: " << args.out_dir
                << std::endl;
      exit(EXIT_FAILURE);
    }
  }

  std::string metafiles = args.out_dir + "/meta/";
  int status = mkdir(metafiles.c_str(), 0755);
  if (status != 0 && errno != EEXIST) {
    // Attention: errno may get changed by syscalls or lib functions.
    // Saving before printing is a conventional way of using errno.
    int errsv = errno;
    std::cerr << "error: cannot mkdir meta in outdir. errno = " << errsv
              << std::endl;
    exit(EXIT_FAILURE);
  }

  return args;
}
} // namespace

int main(int argc, char* argv[]) {
  Arguments args = parse_args(argc, argv);

  InjectDebug inject_debug(args.out_dir, args.dex_files);
  inject_debug.run();

  return 0;
}
