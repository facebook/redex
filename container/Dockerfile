# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Dockerfile for dev, testing and running redex. See instructions in
# website/docs/technical_details/docker.md.

FROM docker.io/debian:13 as base

WORKDIR /redex

ENV TZ=UTC PATH="/redex:$PATH"
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone
RUN apt-get update && apt-get install -y curl wget zip unzip default-jdk-headless python3-pip
RUN wget https://dl.google.com/android/repository/commandlinetools-linux-6609375_latest.zip && \
    mkdir -p sdk/cmdline-tools && \
    unzip commandlinetools-linux-6609375_latest.zip -d sdk/cmdline-tools/ && \
    rm commandlinetools-linux-6609375_latest.zip && \
    cd sdk/cmdline-tools/tools/bin && \
    echo 'y' | ./sdkmanager --install 'build-tools;29.0.2' && \
    ./sdkmanager --install 'platforms;android-29' && \
    cd ../../../.. && \
    rm -rf sdk/emulator 2>/dev/null
COPY . .
RUN ./setup_oss_toolchain.sh
RUN autoreconf -ivf
RUN ./configure --disable-kotlin-tests "--with-android-sdk=$(pwd)/sdk"
RUN make -j$(nproc)

FROM base as test

RUN make -j$(nproc) check TESTS=
CMD make -j1 check

FROM base as redex

ENTRYPOINT ["python3", "redex.py", "--redex-binary", "/redex/redex-all"]
