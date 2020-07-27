---
id: installation
title: Installation
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

### Ubuntu/Debian (64-bit)
Base requirements are automake & libtool, GCC >= 5, Python >= 3.6 and Boost >= 1.71.0, as well as
development versions of `iberty`, `jemalloc`, `jsoncpp`, `lz4`, `lzma`, and `zlib`.
#### Ubuntu 16.04+, Debian 10(Buster)+
The minimum supported Ubntu version is 16.04. The minimum supported Debian version is 10.

A [convenience script](https://github.com/facebook/redex/blob/master/setup_oss_toolchain.sh)
will set up the build environment. This may include downloading Python 3.6 and Boost 1.71.0
on older OS versions.
```
sudo ./setup_oss_toolchain.sh
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
autoreconf -ivf && ./configure && make -j
sudo make install
```
If you experience out-of-memory errors, reduce Make parallelism, e.g., to `-j4`.

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
If you experience out-of-memory errors, reduce Make parallelism, e.g., to `-j4`.
