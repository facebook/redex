# Usage: docker run -it -v $ANDROID_SDK:/opt/android-sdk-linux -v $(pwd):/redex redex redex path/to/your.apk -o path/to/output.apk
# Build: docker build --rm -t redex -f docker/Dockerfile .
FROM ubuntu:14.04

ADD . /redex

WORKDIR /redex
ENV LD_LIBRARY_PATH /usr/local/lib
ENV ANDROID_HOME /opt/android-sdk-linux
ENV ANDROID_SDK $ANDROID_HOME

RUN apt-get update && apt-get install -y git \
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
    python3 \
    libiberty-dev && \
    git submodule update --init && \
    autoreconf -ivf && ./configure && make && make install && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

CMD ["redex"]
