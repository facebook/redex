# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Dockerfile for dev and testing.
#
# Build with:
#   docker build . -f container/Dockerfile -t redex
# Run tests with
#   docker run redex
# Or
#   docker run -it redex bash
# to explore the built results.

FROM docker.io/debian:12

ENV TZ=UTC
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone
RUN apt-get update && apt-get install -y curl wget zip unzip default-jdk-headless
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
RUN ./configure --disable-kotlin-tests --with-android-sdk=/sdk
RUN make -j$(nproc) check TESTS=
CMD make -j1 check
