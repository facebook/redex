/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * Creating a sidebar enables you to:
 - create an ordered group of docs
 - render a sidebar for each doc of that group
 - provide next/previous navigation

 The sidebars can be generated from the filesystem, or explicitly defined here.

 Create as many sidebars as you want.
 */

const {fbContent} = require('docusaurus-plugin-internaldocs-fb/internal');

module.exports = {
  // By default, Docusaurus generates a sidebar from the docs folder structure
  mySidebar: fbContent({
    external: [
      {
        type: 'category',
        label: 'Getting Started',
        link: { type: 'doc', id: 'getting_started/getting_started' },
        items: [
          'getting_started/installation',
          'getting_started/configuring',
          'getting_started/passes',
          'getting_started/usage',
        ]
      },
      {
        type: 'category',
        label: 'Examples',
        items: [
          'examples/proguard',
          'examples/synth',
        ]
      },
      {
        type: 'category',
        label: 'Technical Details',
        items: [
          'technical_details/docker',
          'technical_details/interdex',
        ]
      },
      {
        type: 'category',
        label: 'Help',
        items: [
          'help/faq',
        ]
      }
    ],
    internal: [
      {
        type: 'category',
        label: 'Redex Documentation',
        link: {
          type: 'generated-index',
          title: 'Redex Documentation (Meta-Internal)',
          slug: '/',
        },
        items: [
          {
            type: 'category',
            label: 'Redex Optimizations',
            items: [
              'getting_started/passes',
            ]
          },
          {
            type: 'category',
            label: 'FAQ',
            link: { type: 'doc', id: 'fb/faq/index' },
            items: [
            ]
          },
          {
            type: 'category',
            label: 'Android APK Size Topics',
            link: { type: 'doc', id: 'fb/apk_size_topics/index' },
            items: [
            ]
          },
          {
            type: 'category',
            label: 'Useful Tooling',
            link: { type: 'doc', id: 'fb/useful_tooling/index' },
            items: [
            ]
          },
          {
            type: 'category',
            label: 'Contributing to Redex',
            link: { type: 'doc', id: 'fb/contributing/index' },
            items: [
            ]
          },
          {
            type: 'category',
            label: 'Redex Team Internal',
            link: { type: 'doc', id: 'fb/redex_team_internal/index' },
            items: [
            ]
          },
          {
            type: 'category',
            label: 'Related Teams',
            link: { type: 'doc', id: 'fb/related_teams/index' },
            items: [
              {
                type: 'category',
                label: 'Pogo',
                link: { type: 'doc', id: 'fb/related_teams/pogo/index' },
                items: [],
              },
            ]
          },
          {
            type: 'category',
            label: 'External Docs',
            items: [
              {
                type: 'category',
                label: 'Getting Started',
                link: { type: 'doc', id: 'getting_started/getting_started' },
                items: [
                  'getting_started/installation',
                  'getting_started/configuring',
                  { type: 'ref', id: 'getting_started/passes' },
                  'getting_started/usage',
                ]
              },
              {
                type: 'category',
                label: 'Examples',
                items: [
                  'examples/proguard',
                  'examples/synth',
                ]
              },
              {
                type: 'category',
                label: 'Technical Details',
                items: [
                  'technical_details/docker',
                  'technical_details/interdex',
                ]
              },
              {
                type: 'category',
                label: 'Help',
                items: [
                  'help/faq',
                ]
              }
            ]
          }
        ]
      }
    ]
  }),
};
