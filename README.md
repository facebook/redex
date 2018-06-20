ReDex: An Android Bytecode Optimizer
====================================

ReDex is an Android bytecode (dex) optimizer originally developed at
Facebook. It provides a framework for reading, writing, and analyzing .dex
files, and a set of optimization passes that use this framework to improve the
bytecode.  An APK optimized by ReDex should be smaller and faster than its
source.

# Quick Start Guide

## Dependencies

We use package managers to resolve third-party library dependencies.

### macOS

You will need Xcode with command line tools installed.  To get the command line tools, use:
```
xcode-select --install
```

Install dependencies using homebrew:
```
brew install autoconf automake libtool python3
brew install boost jsoncpp
```

### Ubuntu (64-bit)
```
sudo apt-get install \
    g++ \
    automake \
    autoconf \
    autoconf-archive \
    libtool \
    liblz4-dev \
    liblzma-dev \
    make \
    zlib1g-dev \
    binutils-dev \
    libjemalloc-dev \
    libiberty-dev \
    libjsoncpp-dev
```

Redex requires boost version >= 1.58. The versions in the Ubuntu 14.04 and
14.10 repositories are too old. This script will install boost for you instead:
```
sudo ./get_boost.sh
```

If you're on ubuntu 16.04 or newer, the version in the repository is fine:
```
sudo apt-get install libboost-all-dev
```

### Experimental: Windows 10 (64-bit)

You need Visual Studio 2017. Visual Studio 2015 is also possible, but a couple of C++ compile errors need to be fixed. We use [vcpkg](https://github.com/Microsoft/vcpkg) for dependencies. Install vcpkg from their [document](https://github.com/Microsoft/vcpkg):

```
cd c:\tools
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg integrate install
```
Install necessary libraries with `x64-windows-static`:
```
.\vcpkg install boost --triplet x64-windows-static
.\vcpkg install zlib --triplet x64-windows-static
.\vcpkg install jsoncpp --triplet x64-windows-static
.\vcpkg install mman --triplet x64-windows-static
```

## Download, Build and Install

Get ReDex from GitHub:
```
git clone https://github.com/facebook/redex.git
cd redex
```

Now, build ReDex using autoconf and make.
```
# if you're using gcc, please use gcc-4.9
autoreconf -ivf && ./configure && make -j4
sudo make install
```

### Experimental: CMake for Mac, Linux, and Windows

Alternatively, build using CMake. Note that the current `CMakeLists.txt` only implements a rule for `redex-all` binary. We will support installation and testing soon.

Generate build files. By default, it uses Makefile:
```
# Assume you are in redex directory
mkdir build-cmake
cd build-cmake
# .. is the root source directory of Redex
cmake ..
```

If you prefer the ninja build system:
```
cmake .. -G Ninja
```

On Windows, first, get `CMAKE_TOOLCHAIN_FILE` from the output of `"vcpkg integrate install"`, and then:
```
cmake .. -G "Visual Studio 15 2017 Win64"
 -DVCPKG_TARGET_TRIPLET=x64-windows-static
 -DCMAKE_TOOLCHAIN_FILE="C:/tools/vcpkg/scripts/buildsystems/vcpkg.cmake"
```

Build `redex-all`:

```
cmake --build .
```

On Windows, you may build from Visual Studio. `Redex.sln` has been generated.

You should see a `redex-all` executable, and the executable should show about 45 passes.

```
./redex-all --show-passes
```

## Test

Optionally, you can run our unit test suite.  We use gtest, which is downloaded
via a setup script.
```
./test/setup.sh
cd test
make check
```

## Usage

To use ReDex, first build your app and find the APK for it.  Then run:
```
redex path/to/your.apk -o path/to/output.apk
```

If you want some statistics about each pass, you can turn on tracing:
```
export TRACE=1
```

The result `output.apk` should be smaller and faster than the
input.  Enjoy!

# Documentation
Right now we have a limited amount of [documentation](docs/README.md) which describes a few
example Redex optimization passes as well as deployments of Redex (including Docker).

# More Information

The blog [Optimizing Android bytecode with ReDex](https://code.facebook.com/posts/1480969635539475/optimizing-android-bytecode-with-redex) provides an overview of the Redex project.

# Issues
Issues on GitHub are assigned priorities which reflect their urgency and how soon they are
likely to be addressed.
* P0: Unbreak now! A serious issue which should have someone working on it right now.
* P1: High Priority. An important issue that someone should be actively working on.
* P2: Mid Priority. An important issue which is in the queue to be processed soon.
* P3: Low Priority. An important issue which may get dealt with at a later date.
* P4: Wishlist: An issue with merit but low priority which is up for grabs but likely to be pruned if not addressed after a reasonable period.

# License

ReDex is BSD-licensed.  We also provide an additional patent grant.

---

# FAQ

## I'm getting "Couldn't find zipalign. See README.md to resolve this"

`zipalign` is an optimization step that is bundled with the Android SDK.  You
need to tell redex where to find it.  For example, if you installed the SDK at
`/path/to/android/sdk`, try:
```
ANDROID_SDK=/path/to/android/sdk redex [... arguments ...]
```
You can alternatively add `zipalign` to your PATH, for example:
```
PATH=/path/to/android/sdk/build-tools/xx.y.zz:$PATH redex [... arguments ...]
```

## My app fails to install with `Failure [INSTALL_PARSE_FAILED_NO_CERTIFICATES]`

After you run redex, you'll need to re-sign your app.  You can re-sign manually
using these instructions:
https://developer.android.com/tools/publishing/app-signing.html#signing-manually.

You can also tell redex to sign for you.  If you want to sign with the debug
key, you can simply do:

```
redex --sign [ ... arguments ...]
```

If you want to sign with your release key, you'll need to provide the
appropriate args:

```
--sign Sign the apk after optimizing it
-s [KEYSTORE], --keystore [KEYSTORE]
-a [KEYALIAS], --keyalias [KEYALIAS]
-p [KEYPASS], --keypass [KEYPASS]
```

## My App crashes with `MethodNotFoundException`, `ClassNotFoundException`, `NoSuchFieldException`, or something similar. How do I fix this?

Redex probably deleted or renamed it. Redex is quite aggressive about deleting
things it deems are unreachable. But, often Redex doesn't know about reflection
or other complex ways an entity could be reached.

Here's how you ensure Redex will not delete or rename something:

Annotate any class, method, or field you want to keep with `@DoNotStrip`.

Add this to your redex config (at the uppermost level of the json) to
prevent deletion:
```
"keep_annotations": [
  "Lcom/path/to/your/DoNotStrip;"
]
```

and add this to your config to prevent renaming:
```
"RenameClassesPassV2" : {
  "dont_rename_annotated": [
    "Lcom/path/to/your/DoNotStrip;"
  ]
}
```

and define `DoNotStrip`:

```
package com.path.to.your;
public @interface DoNotStrip {}
```

## How does this compare to ProGuard?

ReDex is conceptually similar to ProGuard, in that both optimize bytecode.
ReDex, however, optimizes .dex bytecode, while ProGuard optimizes .class
bytecode before it is lowered to .dex.  Operating on .dex is sometimes an
advantage: you can consider the number of virtual registers used by a method
that is an inlining candidate, and you can control the layout of classes within
a dex file.  But ProGuard has some capabilities that ReDex does not (for
example, ReDex will not remove unused method parameters, which ProGuard does).

## How about DexGuard?

DexGuard operates on dex, but we haven't evaluated it at all since it's closed
source.  We don't use it at Facebook and we have no plans to start.
