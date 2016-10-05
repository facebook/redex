ReDex: An Android Bytecode Optimizer
------------------------------------

ReDex is an Android bytecode (dex) optimizer originally developed at
Facebook. It provides a framework for reading, writing, and analyzing .dex
files, and a set of optimization passes that use this framework to improve the
bytecode.  An APK optimized by ReDex should be smaller and faster than its
source.

# Quick Start Guide

## Dependencies

Getting these dependences is easiest using a package manager.

### Mac OS X

You'll need Xcode with command line tools installed.  To get the command line
tools, use:
```
xcode-select --install
```

Install dependences using homebrew:
```
brew install autoconf automake libtool python3
brew install boost double-conversion gflags glog libevent jsoncpp
```

### Ubuntu 14.04 LTS (64-bit)
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
    libiberty-dev \
    libjsoncpp-dev
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

# Documentation
Right now we have a limited amount of [documentation](docs/README.md) which describes a few
example Redex optimization passes as well as deployments of Redex (including Docker).

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
http://developer.android.com/tools/publishing/app-signing.html#signing-manually.

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
