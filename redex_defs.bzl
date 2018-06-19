REDEX_ROOT = "//native/redex"

REDEX_BINARY = "%s:redex-all" % REDEX_ROOT

REDEX_TOOL = "%s:redex-tool" % REDEX_ROOT

REDEX_DEBUG_BINARY = "%s:redex-all-dbg" % REDEX_ROOT

REDEX_SCRIPT = "%s:fb-redex" % REDEX_ROOT

REDEX_LIB = "%s:libredex" % REDEX_ROOT

REDEX_SERVICE = "%s:service" % REDEX_ROOT

REDEX_OPT = "%s:opt" % REDEX_ROOT

REDEX_ABSTRACT_INTERPRETATION = "%s:abstract-interpretation" % REDEX_ROOT

# Always issue warning, compile a release build with optimization
# and pthreads, make a debug build with no optimization and no inlining.
warnings = [
    "-Wall",
    "-Wsign-compare",
    "-Wnon-virtual-dtor",
    "-Wmissing-field-initializers",
    "-Wshadow",
]

cxxflags_shared = [
    "-std=gnu++14",
    "-g",
]

cxxflags_nowarn = cxxflags_shared + ["-O3"]

cxxflags = cxxflags_nowarn + warnings

cxxflags_dbg_nowarn = cxxflags_shared + [
    "-O0",
    "-fno-inline",
]

cxxflags_dbg = cxxflags_dbg_nowarn + warnings
