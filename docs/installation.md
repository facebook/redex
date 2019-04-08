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
# if you're using gcc, please use gcc-5
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
