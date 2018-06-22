load("//native/redex:redex_defs.bzl", "cxxflags", "cxxflags_dbg", "cxxflags_dbg_nowarn", "cxxflags_nowarn")
load("//build_defs:fb_xplat_cxx_binary.bzl", "fb_xplat_cxx_binary")
load("//build_defs:fb_xplat_cxx_library.bzl", "fb_xplat_cxx_library")
load("@xplat//configurations/buck:buckconfig.bzl", "read_bool")

# Core external dependencies of the Redex system.
default_library_deps = [
    ":redex_platform_deps",
    "//native/liblocator:liblocator",
    "xplat//third-party/boost:boost",
    "xplat//third-party/boost:boost_filesystem",
    "xplat//third-party/boost:boost_program_options",
    "xplat//third-party/boost:boost_regex",
    "xplat//third-party/boost:boost_system",
    "xplat//third-party/boost:boost_iostreams",
    "xplat//third-party/boost:boost_thread",
    "xplat//third-party/flatbuffers:flatbuffers",
    "xplat//third-party/jsoncpp:jsoncpp",
]

JEMALLOC = "xplat//third-party/jemalloc:jemalloc"

def build_deps(deps, xplat_deps, use_jemalloc, debug=False):
    jemalloc_available = read_bool("third-party", "jemalloc_available", False)
    return default_library_deps + \
           ([JEMALLOC] if jemalloc_available and use_jemalloc else []) + \
           [d + "-dbg" if debug else d for d in deps] + \
           xplat_deps

def redex_library(
        name,
        deps = [],
        exported_deps = [],
        xplat_deps = [],
        warn = True,
        use_jemalloc = False,
        **kwargs):
    fb_xplat_cxx_library(
        name = name,
        deps = build_deps(deps, xplat_deps, use_jemalloc),
        exported_deps = exported_deps,
        header_namespace = "",
        compiler_flags = cxxflags if warn else cxxflags_nowarn,
        defaults = {},
        visibility = ["PUBLIC"],
        # This is not dynamic linking, it just exposes symbol names so that the
        # backtrace handler can emit function names on a seg fault or abort
        exported_linker_flags = ["-rdynamic"],
        **kwargs
    )
    fb_xplat_cxx_library(
        name = name + "-dbg",
        deps = build_deps(deps, xplat_deps, use_jemalloc, debug=True),
        exported_deps = [d + "-dbg" for d in exported_deps],
        header_namespace = "",
        compiler_flags = cxxflags_dbg if warn else cxxflags_dbg_nowarn,
        defaults = {},
        visibility = ["PUBLIC"],
        # This is not dynamic linking, it just exposes symbol names so that the
        # backtrace handler can emit function names on a seg fault or abort
        exported_linker_flags = ["-rdynamic"],
        **kwargs
    )

def redex_binary(
        name,
        deps = [],
        xplat_deps = [],
        warn = True,
        use_jemalloc = False,
        **kwargs):
    fb_xplat_cxx_binary(
        name = name,
        deps = build_deps(deps, xplat_deps, use_jemalloc),
        header_namespace = "",
        compiler_flags = cxxflags if warn else cxxflags_nowarn,
        defaults = {},
        visibility = ["PUBLIC"],
        **kwargs
    )
    fb_xplat_cxx_binary(
        name = name + "-dbg",
        deps = build_deps(deps, xplat_deps, use_jemalloc, debug=True),
        header_namespace = "",
        compiler_flags = cxxflags_dbg if warn else cxxflags_dbg_nowarn,
        defaults = {},
        visibility = ["PUBLIC"],
        **kwargs
    )
