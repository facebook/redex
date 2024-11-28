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
            label: 'Configuring Redex',
            link: { type: 'doc', id: 'fb/configuring_redex/redex_configuration' },
            items: [
              {
                type: 'doc',
                id: 'fb/configuring_redex/redex_configuration',
              },
              {
                type: 'doc',
                id: 'fb/configuring_redex/refig',
              }
            ]
          },
          {
            type: 'category',
            label: 'FAQ',
            link: { type: 'doc', id: 'fb/faq/index' },
            items: [
              {
                type: 'doc',
                id: 'fb/faq/CNF',
              },
              {
                type: 'doc',
                id: 'fb/faq/NSME',
              },
              {
                type: 'doc',
                id: 'fb/faq/donotstrip',
              },
              {
                type: 'doc',
                id: 'fb/faq/resources',
              },
              {
                type: 'doc',
                id: 'fb/faq/build',
              },
              {
                type: 'doc',
                id: 'fb/faq/symbolication',
              },
              {
                type: 'doc',
                id: 'fb/faq/betteroptimizing',
              },
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
            items: [
              {
                type: 'doc',
                id: 'fb/useful_tooling/symbolication',
              },
              {
                type: 'doc',
                id: 'fb/useful_tooling/bytecode_analysis_tool',
              },
              {
                type: 'doc',
                id: 'fb/useful_tooling/reachability_tool',
              },
              {
                type: 'doc',
                id: 'fb/useful_tooling/query_tool',
              },
              {
                type: 'doc',
                id: 'fb/useful_tooling/bisection_tool',
              },
              {
                type: 'doc',
                id: 'fb/useful_tooling/e2e_test_tool',
              },
              {
                type: 'doc',
                id: 'fb/useful_tooling/interactive_debugging_tool',
              },
              {
                type: 'doc',
                id: 'fb/useful_tooling/profiling_tool',
              },
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
            items: [
              {
                type: 'category',
                label: 'Redex Introduction',
                items: [
                  {
                    type: 'doc',
                    id: 'fb/redex_team_internal/redex_introduction/what_is_redex',
                    label: 'What is Redex?',
                  },
                  {
                    type: 'doc',
                    id: 'fb/redex_team_internal/redex_introduction/communication_channels',
                    label: 'Communication Channels',
                  },
                  {
                    type: 'doc',
                    id: 'fb/redex_team_internal/redex_introduction/developer_environment',
                    label: 'Developer Environment',
                  },
                  {
                    type: 'doc',
                    id: 'fb/redex_team_internal/redex_introduction/architecture',
                    label: 'Architecture',
                  },
                  {
                    type: 'doc',
                    id: 'fb/redex_team_internal/redex_introduction/running_redex',
                    label: 'Running Redex',
                  },
                  {
                    type: 'doc',
                    id: 'fb/redex_team_internal/redex_introduction/tracing',
                    label: 'Tracing',
                  },
                  {
                    type: 'doc',
                    id: 'fb/redex_team_internal/redex_introduction/debugging',
                    label: 'Debugging',
                  },
                  {
                    type: 'doc',
                    id: 'fb/redex_team_internal/redex_introduction/writing_tests',
                    label: 'Writing Tests',
                  },
                  {
                    type: 'doc',
                    id: 'fb/redex_team_internal/redex_introduction/shipping_a_diff',
                    label: 'Shipping a Diff',
                  },
                ]
              }
            ]
          },
          {
            type: 'category',
            label: 'Related Teams',
            items: [
              {
                type: 'link',
                label: 'Pogo Stick',
                href: 'https://www.internalfb.com/intern/staticdocs/pogo/',
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
