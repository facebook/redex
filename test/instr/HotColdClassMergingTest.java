/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import static org.assertj.core.api.Assertions.assertThat;

import org.junit.Test;

interface HotColdWorker {
  int work();
}

/**
 * Eight anonymous HotColdWorker implementations of the same shape. The method
 * profile marks work() of $1..$4 hot and leaves $5..$8 without data, so hot/cold
 * grouping must merge them into two separate classes.
 */
public class HotColdClassMergingTest {
  @Test
  public void testHotAndColdWorkers() {
    HotColdWorker[] workers = {
      new HotColdWorker() {
        @Override
        public int work() {
          return 101;
        }
      },
      new HotColdWorker() {
        @Override
        public int work() {
          return 102;
        }
      },
      new HotColdWorker() {
        @Override
        public int work() {
          return 103;
        }
      },
      new HotColdWorker() {
        @Override
        public int work() {
          return 104;
        }
      },
      new HotColdWorker() {
        @Override
        public int work() {
          return 201;
        }
      },
      new HotColdWorker() {
        @Override
        public int work() {
          return 202;
        }
      },
      new HotColdWorker() {
        @Override
        public int work() {
          return 203;
        }
      },
      new HotColdWorker() {
        @Override
        public int work() {
          return 204;
        }
      },
    };
    int[] expected = {101, 102, 103, 104, 201, 202, 203, 204};
    for (int i = 0; i < workers.length; i++) {
      assertThat(workers[i].work()).isEqualTo(expected[i]);
    }
  }
}
