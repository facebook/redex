/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

import React from 'react';
import clsx from 'clsx';
import styles from './HomepageFeatures.module.css';

const FeatureList = [
  {
    title: 'Optimizing',
    description: (
      <>
        Redex provides a framework for reading, writing, and analyzing .dex
        files, and a set of optimization passes that use this framework to
        improve the bytecode. An APK optimized by Redex should be smaller and
        faster.
      </>
    ),
  },
  {
    title: 'Fast',
    description: (
      <>
        Fewer bytes also means faster download times, faster install times, and
        lower data usage for cell users. Lastly, less bytecode also typically
        translates into faster runtime performance.
      </>
    ),
  },
  {
    title: 'Buck Integration',
    description: (
      <>
        Redex has deep integration with Buck where your Redex config is passed
        as a parameter to the Buck android_binary rule when generating the APK.
      </>
    ),
  },
];

function Feature({Svg, title, description}) {
  return (
    <div className={clsx('col col--4')}>
      <div className="text--center padding-horiz--md">
        <h3>{title}</h3>
        <p>{description}</p>
      </div>
    </div>
  );
}

export default function HomepageFeatures() {
  return (
    <section className={styles.features}>
      <div className="container">
        <div className="row">
          {FeatureList.map((props, idx) => (
            <Feature key={idx} {...props} />
          ))}
        </div>
      </div>
    </section>
  );
}
