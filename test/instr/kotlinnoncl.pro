# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

-keep class KtNonCapturingLambda {
private long sink               ;
private boolean ret             ;
public final void main()        ;
}

-allowaccessmodification
-dontobfuscaterintmapping
