---
id: usage
title: Usage
sidebar_position: 4
---

## Basic Usage

To use ReDex, first build your app and find the APK for it.  Then the simplest
invocation is:
```
[python3] redex.py path/to/your.apk -o path/to/output.apk
```
A more complete invocation with:
* the default configuration setting [config/default.config](https://github.com/facebook/redex/blob/master/config/default.config)
* access to the Android SDK tools
```
[python3] redex.py -c default.config \
  --android-sdk-path path/to/android/sdk \
  path/to/your.apk -o path/to/output.apk
```
The full set of options can be found with
```
[python3] redex.py --help
```
and at the time of writing is
```
usage: redex.py [-h] [-o [OUT]] [-j JARPATHS] [--redex-binary [REDEX_BINARY]]
                [-c CONFIG] [--sign] [-s [KEYSTORE]] [-a [KEYALIAS]]
                [-p [KEYPASS]] [-u] [--unpack-dest UNPACK_DEST] [-w [WARN]]
                [-d] [--dev] [-m [PROGUARD_MAP]] [--printseeds [PRINTSEEDS]]
                [--used-js-assets USED_JS_ASSETS] [-P PROGUARD_CONFIGS]
                [-k [KEEP]] [-A [ARCH]] [-S PASSTHRU] [-J PASSTHRU_JSON]
                [--lldb] [--gdb] [--ignore-zipalign] [--verify-none-mode]
                [--enable-instrument-pass] [--is-art-build] [--enable-pgi]
                [--post-lowering] [--disable-dex-hasher] [--page-align-libs]
                [--side-effect-summaries SIDE_EFFECT_SUMMARIES]
                [--escape-summaries ESCAPE_SUMMARIES] [--stop-pass STOP_PASS]
                [--output-ir OUTPUT_IR]
                [--debug-source-root [DEBUG_SOURCE_ROOT]] [--always-clean-up]
                [--cmd-prefix CMD_PREFIX] [--reset-zip-timestamps] [-q]
                [--android-sdk-path ANDROID_SDK_PATH]
                input_apk

Given an APK, produce a better APK!

positional arguments:
  input_apk             Input APK file

optional arguments:
  -h, --help            show this help message and exit
  -o [OUT], --out [OUT]
                        Output APK file name (defaults to redex-out.apk)
  -j JARPATHS, --jarpath JARPATHS
                        Path to dependent library jar file
  --redex-binary [REDEX_BINARY]
                        Path to redex binary
  -c CONFIG, --config CONFIG
                        Configuration file
  --sign, --no-sign     Sign the apk after optimizing it
  -s [KEYSTORE], --keystore [KEYSTORE]
  -a [KEYALIAS], --keyalias [KEYALIAS]
  -p [KEYPASS], --keypass [KEYPASS]
  -u, --unpack-only     Unpack the apk and print the unpacked directories,
                        don't run any redex passes or repack the apk
  --unpack-dest UNPACK_DEST
                        Specify the base name of the destination directories;
                        works with -u
  -w [WARN], --warn [WARN]
                        Control verbosity of warnings
  -d, --debug           Unpack the apk and print the redex command line to run
  --dev                 Optimize for development speed
  -m [PROGUARD_MAP], --proguard-map [PROGUARD_MAP]
                        Path to proguard mapping.txt for deobfuscating names
  --printseeds [PRINTSEEDS]
                        File to print seeds to
  --used-js-assets USED_JS_ASSETS
                        A JSON file (or files) containing a list of resources
                        used by JS
  -P PROGUARD_CONFIGS, --proguard-config PROGUARD_CONFIGS
                        Path to proguard config
  -k [KEEP], --keep [KEEP]
                        [deprecated] Path to file containing classes to keep
  -A [ARCH], --arch [ARCH]
                        Architecture; one of arm/armv7/arm64/x86_64/x86"
  -S PASSTHRU           Arguments passed through to redex
  -J PASSTHRU_JSON      JSON-formatted arguments passed through to redex
  --lldb                Run redex binary in lldb
  --gdb                 Run redex binary in gdb
  --ignore-zipalign     Ignore if zipalign is not found
  --verify-none-mode    Enable verify-none mode on redex
  --enable-instrument-pass
                        Enable InstrumentPass if any
  --is-art-build        States that this is an art only build
  --enable-pgi          If not passed, Profile Guided Inlining is disabled
  --post-lowering       Specifies whether post lowering steps should be run
  --disable-dex-hasher  Disable DexHasher
  --page-align-libs     Preserve 4k page alignment for uncompressed libs
  --side-effect-summaries SIDE_EFFECT_SUMMARIES
                        Side effect information for external methods
  --escape-summaries ESCAPE_SUMMARIES
                        Escape information for external methods
  --stop-pass STOP_PASS
                        Stop before a pass and dump intermediate dex and IR
                        meta data to a directory
  --output-ir OUTPUT_IR
                        Stop before stop_pass and dump intermediate dex and IR
                        meta data to output_ir folder
  --debug-source-root [DEBUG_SOURCE_ROOT]
                        Root directory that all references to source files in
                        debug information is given relative to.
  --always-clean-up     Clean up temporaries even under failure
  --cmd-prefix CMD_PREFIX
                        Prefix redex-all with
  --reset-zip-timestamps
                        Reset zip timestamps for deterministic output
  -q, --quiet           Do not be verbose, and override TRACE.
  --android-sdk-path ANDROID_SDK_PATH
                        Path to Android SDK
```

From here, you may want to read the [configuration guide](config.md) and details
about the [passes](passes.md).

The result `output.apk` should be smaller and faster than the
input.

## Tracing

If you want some statistics about each pass, you can turn on tracing:
```
export TRACE=1
```
More specifically, tracing has [categories](https://github.com/facebook/redex/blob/c5d5651b8b3ae9fda7b3305de9f55e1b82077a2d/libredex/Trace.h#L20)
and levels. For a `TRACE(X, Y, msg)` statement in the code to be logged,
category `X` must have level `Y` or higher:
```
export TRACE=X:1,Y:2,Z:3
```
The output of tracing can also be redirected to a file with the `TRACEFILE`
variable:
```
export TRACEFILE=/path/to/trace.txt
```
