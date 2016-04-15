ReDex: An Android Bytecode Optimizer
------------------------------------

ReDex is an Android bytecode (dex) optimizer originally developed at
Facebook. It provides a framework for reading, writing, and analyzing .dex
files, and a set of optimization passes that use this framework to improve the
bytecode.  An APK optimized by ReDex should be smaller and faster than its
source.

# Quick Start Guide

## Dependencies

ReDex depends on folly, glog, double-conversion, boost and zlib, and uses
autoconf/automake for building.  Getting these dependences is easiest using a
package manager.

### Mac OS X

You'll need Xcode with command line tools installed.  To get the command line
tools, use:
```
xcode-select --install
```

Install dependences using homebrew:
```
brew install autoconf automake libtool python3
brew install boost double-conversion gflags glog libevent openssl
brew link openssl --force
```

### Ubuntu 14.04 LTS
```
sudo apt-get install \
    g++ \
    automake \
    autoconf \
    autoconf-archive \
    libtool \
    libboost-all-dev \
    libevent-dev \
    libdouble-conversion-dev \
    libgoogle-glog-dev \
    libgflags-dev \
    liblz4-dev \
    liblzma-dev \
    libsnappy-dev \
    make \
    zlib1g-dev \
    binutils-dev \
    libjemalloc-dev \
    libssl-dev \
    libiberty-dev
```

## Download, Build and Install

Get ReDex and its submodules from GitHub:
```
git clone https://github.com/facebook/redex.git
cd redex
git submodule update --init
```

Now, build ReDex using autoconf and make.
```
autoreconf -ivf && ./configure && make
sudo make install
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
With any luck, the result `output.apk` should be smaller and faster than the
input.  Enjoy!

# FAQ

## How does this compare to ProGuard?

ReDex is conceptually similar to ProGuard, in that both optimize bytecode.
ReDex, however, optimizes .dex bytecode, while ProGuard optimizes .class
bytecode before it is lowered to .dex.  Operating on .dex is sometimes an
advantage: you can consider the number of virtual registers used by a method
that is an inlining candidate, and you can control the layout of classes within
a dex file.  But ProGuard has many capabilities that ReDex does not (for
example, ReDex will not remove unused method parameters, which ProGuard does).

In our opinion, comparing ReDex and ProGuard is a bit apples-and-oranges, since
we have focused on optimizations that add value on top of ProGuard.  We use both
tools to optimize the Facebook app.  Our reported performance and size
improvements (about 25% on both dex size and cold start time) are based on using
ReDex on an app already optimized with ProGuard.  We have no plans to measure
performance without ProGuard.

## How about DexGuard?

DexGuard operates on dex, but we haven't evaluated it at all since it's closed
source.  We don't use it at Facebook and we have no plans to start.

# More Information

The blog [Optimizing Android bytecode with ReDex](https://code.facebook.com/posts/1480969635539475/optimizing-android-bytecode-with-redex) provides an overview of the Redex project.

# License

ReDex is BSD-licensed.  We also provide an additional patent grant.
