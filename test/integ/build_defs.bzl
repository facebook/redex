load("@xplat//build_defs:fb_core_android_library.bzl", "fb_core_android_library")
load("//native/redex:redex_defs.bzl", "REDEX_ROOT")
load("//native/redex:redex_test_defs.bzl", "REDEX_LIB", "REDEX_OPT")

def def_integ_test(name, java_files, cpp_files, env = {}, **kwargs):
    """
    Helper function to define an integration test.
    Compile java_files into a dex, then run the gtests in cpp_files against that dex.
    """
    fb_core_android_library(
        name = name + "-class",
        srcs = java_files,
    )
    native.genrule(
        name = name + "-dex",
        out = name + ".dex",
        bash = "$DX --dex --output=$OUT $(location :%s)" % (name + "-class"),
    )
    env = dict(env)
    env["dexfile"] = "$(location :%s)" % (name + "-dex")
    env["android_target"] = native.read_config("android", "target", "NotFound")

    native.cxx_test(
        name = name + "-test",
        labels = ["no_lsan"],
        srcs = cpp_files,
        compiler_flags = ["-std=gnu++14"],
        env = env,
        deps = [
            ":%s" % (name + "-dex"),
            REDEX_LIB,
            REDEX_OPT,
            REDEX_ROOT + "/test/common:common",
            "xplat//third-party/gmock:gmock",
        ],
        **kwargs
    )
