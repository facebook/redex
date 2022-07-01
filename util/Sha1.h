/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

/*
 * This implementation of SHA1 is taken from HHVM with minor style edits:
 *   https://github.com/facebook/hhvm
 */

/*
 * State used for SHA1 algorithm.
 */
struct Sha1Context {
  unsigned int state[5]; /* state (ABCD) */
  unsigned int count[2]; /* number of bits, modulo 2^64 */
  unsigned char buffer[64]; /* input buffer */
};

/*
 * SHA1 initialization. Begins an SHA1 operation, writing a new context.
 */
void sha1_init(Sha1Context* context);

/*
 * SHA1 block update operation. Continues an SHA1 message-digest operation,
 * processing another message block, and updating the context.
 */
void sha1_update(Sha1Context* context,
                 const unsigned char* input,
                 unsigned int inputLen);

/*
 * SHA1 finalization. Ends an SHA1 message-digest operation, writing the
 * message digest and zeroizing the context.
 */
void sha1_final(unsigned char* digest, Sha1Context* context);
