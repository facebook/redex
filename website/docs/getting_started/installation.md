---
id: installation
title: Installation
sidebar_position: 1
---

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
For App Bundle support `brew install protobuf` is also required.

### Ubuntu/Debian (64-bit)
Base requirements are automake & libtool, GCC >= 7, Python >= 3.6 and Boost >= 1.71.0, as well as
development versions of `iberty`, `jemalloc`, `jsoncpp`, `lz4`, `lzma`, and `zlib`. `Protobuf` >= 3.0 is required if optimizing an App Bundle.
#### Ubuntu 18.04+, Debian 10(Buster)+
The minimum supported Ubuntu version is 18.04. The minimum supported Debian version is 10.

A [convenience script](https://github.com/facebook/redex/blob/master/setup_oss_toolchain.sh)
will set up the build environment. This may include downloading Python 3.6 and Boost 1.71.0
on older OS versions.
```
sudo ./setup_oss_toolchain.sh
```
After the script, please run `sudo ldconfig` if it throws an error about loading shared libraries for running protoc.

### Experimental: Windows (64-bit) with MSYS2

You need [MSYS2](https://www.msys2.org/#installation) to build `redex-all` (only MingW-w64 is supported) and [Python 3.6+](https://www.python.org/downloads/windows/) to run `redex.py`.

Install the build requirements in an MSYS or MingW64 shell:
```
pacman -Syuu && pacman -Sy make mingw-w64-x86_64-boost mingw-w64-x86_64-cmake mingw-w64-x86_64-gcc mingw-w64-x86_64-jsoncpp mingw-w64-x86_64-make
```

If you do not use Git on Windows directly, you may install and use it under MSYS2:
```
pacman -S git
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
autoreconf -ivf && ./configure && make
sudo make install
```

Alternatively, to enable protobuf to support App Bundles, please use:
```
autoreconf -ivf && ./configure --enable-protobuf
make
sudo make install
```

For protobuf installed in the default system (Homebrew) search path (e.g /usr/local/bin), `--enable-protobuf` is sufficient to trigger the build. Otherwise, specify the protobuf installation path for the autoconf:
```
autoreconf -ivf
./configure --with-protoc=/path/to/protoc --with-protolib=/path/to/protobuf_libs --with-protoheader=/path/to/protobuf_headers --enable-protobuf
make
sudo make install
```

*If your build machine has lots of RAM (on the order of 2-4GB per core), using
Make parallelism can speed up the build (e.g., `make -j4`). However, the C++
compilers are very memory hungry and this needs to be finely tuned on many
systems.*

### Experimental: Windows (64-bit) with MSYS2

The MSYS2 build relies on CMake. In a MingW64 shell:
```
# Assumes you want to use Git under MSYS. Else skip to below.
git clone https://github.com/facebook/redex.git
cd redex
# Assumes you are in the redex directory
mkdir build-cmake
cd build-cmake
cmake -G "MSYS Makefiles" ..
make
```
*If your build machine has lots of RAM (on the order of 2-4GB per core), using
Make parallelism can speed up the build (e.g., `make -j4`). However, the C++
compilers are very memory hungry and this needs to be finely tuned on many
systems.*

You may check whether the produced binary seems in a working condition:
```
# In the MingW64 shell:
./redex-all.exe --show-passes
# Or in a standard Windows command prompt in the same directory
redex-all.exe --show-passes
```
The output should show a large number of included passes, at the time of writing 81.

Bundling the `redex-all` binary with the python scripts is not supported on Windows. Manually copy the binary into the same directory as `redex.py` and use `redex.py` that way, or ensure that `redex.py` is called with the `--redex-binary` parameter:
```
python redex.py --redex-binary PATH_TO_BINARY [...]
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
Optionally, you can run our unit test suite.  We use gtest, which is automatically
downloaded when testing (or by invoking a setup script directly).

Note: Testing is currently not supported for CMake-based builds.

### Dependencies
Some ReDex tests require a Java environment and Android compiler tooling. If a JDK and the
Android SDK are available on the machine, ensure that `javac` and `dx` are available on
the `PATH`. Otherwise, install those dependencies.

For Ubuntu/Debian, this may for example be done with
```
sudo apt-get install -y --no-install-recommends dalvik-exchange openjdk-8-jdk-headless
sudo ln -s /usr/bin/dalvik-exchange /usr/local/bin/dx
```

### Execute
Run tests with
```
make -j check
```
*If your build machine has lots of RAM (on the order of 2-4GB per core), using
Make parallelism can speed up the build and testing (e.g., `make -j4`). However,
the C++ compilers are very memory hungry and this needs to be finely tuned on
many systems.*
