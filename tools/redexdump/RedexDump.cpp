/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RedexDump.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>

#include "PrintUtil.h"
#include "Formatters.h"

static const char ddump_usage_string[] =
    "ReDex, DEX Dump tool\n"
    "\nredexdump pretty prints content of a dexfile. "
    "By default only prints the header\n"
    "\n"
    "Usage:\n"
    "\tredump [-h | --all | [[-string] [-type] [-proto] [-field] [-meth] "
    "[-clsdef] [-clsdata] [-code] [-enarr] [-anno]] [-clean]"
    " <classes.dex>...\n"
    "\n<classes.dex>: path to a dex file (not an APK!)\n"
    "\noptions:\n"
    "--h: help summary\n"
    "\nsections to print:\n"
    "-a, --all: print all items in all sections\n"
    "-s, --string: print items in the string id section\n"
    "-S, --stringdata: print string section (pointee of string ids)\n"
    "-t, --type: print items in the type id section\n"
    "-p, --proto: print items in the proto id section\n"
    "-f, --field: print items in the field id section\n"
    "-m, --meth: print items in the method id section\n"
    "-c, --clsdef: print items in the class def id section\n"
    "-C, --clsdata: print items in the class data section\n"
    "-x, --code: print items in the code data section\n"
    "-e, --enarr: print items in the encoded array section\n"
    "-A, --anno: print items in the annotation section\n"
    "-d, --debug: print debug info items in the data section\n"
    "-D, --ddebug=<addr>: disassemble debug info item at <addr>\n"
    "\nprinting options:\n"
    "--clean: does not print indexes or offsets making the output "
    "--raw: print all bytes, even control characters "
    "usable for a diff\n";

int main(int argc, char* argv[]) {

  bool all = false;
  bool string = false;
  bool stringdata = false;
  bool type = false;
  bool proto = false;
  bool field = false;
  bool meth = false;
  bool clsdef = false;
  bool clsdata = false;
  bool code = false;
  bool enarr = false;
  bool anno = false;
  bool debug = false;
  uint32_t ddebug_offset = 0;
  int no_dump_map = 0;

  char c;
  static const struct option options[] = {
    { "all", no_argument, nullptr, 'a' },
    { "string", no_argument, nullptr, 's' },
    { "stringdata", no_argument, nullptr, 'S' },
    { "type", no_argument, nullptr, 't' },
    { "proto", no_argument, nullptr, 'p' },
    { "field", no_argument, nullptr, 'f' },
    { "meth", no_argument, nullptr, 'm' },
    { "clsdef", no_argument, nullptr, 'c' },
    { "clsdata", no_argument, nullptr, 'C' },
    { "code", no_argument, nullptr, 'x' },
    { "enarr", no_argument, nullptr, 'e' },
    { "anno", no_argument, nullptr, 'A' },
    { "debug", no_argument, nullptr, 'd' },
    { "ddebug", required_argument, nullptr, 'D' },
    { "clean", no_argument, (int*)&clean, 1 },
    { "raw", no_argument, (int*)&raw, 1 },
    { "no-dump-map", no_argument, &no_dump_map, 1 },
    { "help", no_argument, nullptr, 'h' },
    { nullptr, 0, nullptr, 0 },
  };

  while ((c = getopt_long(
            argc,
            argv,
            "asStpfmcCxeAdDh",
            &options[0],
            nullptr)) != -1) {
    switch (c) {
      case 'a':
        all = true;
        break;
      case 's':
        string = true;
        break;
      case 'S':
        stringdata = true;
        break;
      case 't':
        type = true;
        break;
      case 'p':
        proto = true;
        break;
      case 'f':
        field = true;
        break;
      case 'm':
        meth = true;
        break;
      case 'c':
        clsdef = true;
        break;
      case 'C':
        clsdata = true;
        break;
      case 'x':
        code = true;
        break;
      case 'e':
        enarr = true;
        break;
      case 'A':
        anno = true;
        break;
      case 'd':
        debug = true;
        break;
      case 'D':
        sscanf(optarg, "%x", &ddebug_offset);
        break;
      case 'h':
        puts(ddump_usage_string);
        return 0;
      case '?':
        return 1; // getopt_long has printed an error
      case 0:
        // we're handling a long-only option
        break;
      default:
        abort();
    }
  }

  if (optind == argc) {
    fprintf(stderr, "%s: no dex files given; use -h for help\n", argv[0]);
    return 1;
  }

  while (optind < argc) {
    const char* dexfile = argv[optind++];
    ddump_data rd;
    open_dex_file(dexfile, &rd);
    if (!no_dump_map) {
      redump(format_map(&rd).c_str());
    }
    if (string || all) {
      dump_strings(&rd);
    }
    if (stringdata || all) {
      dump_stringdata(&rd);
    }
    if (type || all) {
      dump_types(&rd);
    }
    if (proto || all) {
      dump_protos(&rd);
    }
    if (field || all) {
      dump_fields(&rd);
    }
    if (meth || all) {
      dump_methods(&rd);
    }
    if (clsdef || all) {
      dump_clsdefs(&rd);
    }
    if (clsdata || all) {
      dump_clsdata(&rd);
    }
    if (code || all) {
      dump_code(&rd);
    }
    if (enarr || all) {
      dump_enarr(&rd);
    }
    if (anno || all) {
      dump_anno(&rd);
    }
    if (debug || all) {
      dump_debug(&rd);
    }
    if (ddebug_offset != 0) {
      disassemble_debug(&rd, ddebug_offset);
    }
    fprintf(stdout, "\n");
    fflush(stdout);
  }

  return 0;
}
