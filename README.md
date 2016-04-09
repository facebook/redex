ReDex: An Android Bytecode Optimizer
------------------------------------

ReDex is an Android bytecode (dex) optimizer originally developed at
Facebook. It provides a framework for reading, writing, and analyzing .dex
files, and a set of optimization passes that use this framework to improve the
bytecode.  An APK optimized by ReDex should be smaller and faster than its
source.

# Quick Start Guide

### Dependencies

ReDex depends on folly, glog, double-conversion, boost and zlib, and uses
autoconf/automake for building.  Getting these dependences is easiest using a
package manager.

Mac OS X:
```
brew install autoconf automake libtool
brew install boost double-conversion gflags glog libevent
```

Ubuntu 14.04 LTS:
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

### Download, Build and Install

ReDex includes [folly](https://github.com/facebook/folly) as a git submodule.
Initialize it using:
```
git submodule update --init
```

Now, build ReDex using autoconf and make.
```
autoreconf -ivf && ./configure && make && make install
```

### Usage
To use ReDex, first build your app and find the APK for it.  Then run:
```
redex path/to/your.apk -o path/to/output.apk
```
With any luck, the result `output.apk` should be smaller and faster than the
input.  Enjoy!

# More Information
The blog [Optimizing Android bytecode with ReDex](https://code.facebook.com/posts/1480969635539475/optimizing-android-bytecode-with-redex) provides an overview of the Redex project.

# License

ReDex is BSD-licensed.  We also provide an additional patent grant.
