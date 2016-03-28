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
brew install boost autoconf automake glog double-conversion
```
You will also need to build and install
[folly](https://github.com/facebook/folly) from source.

Ubuntu 14.04 LTS:
```
apt-get install \
  autoconf \
  build-essential \
  libboost-all-dev \
  zlib1g-dev \
  libdouble-conversion-dev \
  libgoogle-glog-dev
```
You will also need to build and install
[folly](https://github.com/facebook/folly) from source.

### Build and Install

Build ReDex using autoconf and make:
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
