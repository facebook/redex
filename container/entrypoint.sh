#!/usr/bin/env bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Ensure the toolchain has been set-up on entry into the container.  Intended
# for use with the "run" script, which correctly sets up the volume.

[[ -f "$HOME/.setup_complete" ]] || (
  /redex/setup_oss_toolchain.sh &&
  touch "$HOME/.setup_complete"
)

exec "$@"
