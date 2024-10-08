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
            type: 'doc',
            label: 'Configuring Redex',
            id: 'fb/redex_configuration'
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
            link: { type: 'doc', id: 'fb/related_teams/index' },
            items: [
              {
                type: 'category',
                label: 'Pogo Stick',
                items: [
                  {
                    type: 'category',
                    label: 'Baseline Profiles',
                    items: [
                      {
                        type: 'doc',
                        id: 'fb/related_teams/pogo/baseline_profiles/manual_profile_generation',
                        label: 'Manual Profile Generation'
                      }
                    ]
                  },
                  {
                    type: 'category',
                    label: 'Pogo Stick Team Internal',
                    items: [
                      {
                        type: 'doc',
                        id: 'fb/related_teams/pogo/pogo_team_internal/overview',
                        label: 'Team Overview',
                      },
                      {
                        type: 'category',
                        label: 'Betamap',
                        items: [
                          {
                            type: 'doc',
                            id: 'fb/related_teams/pogo/pogo_team_internal/betamap/overview',
                            label: 'Overview'
                          },
                          {
                            type: 'doc',
                            id: 'fb/related_teams/pogo/pogo_team_internal/betamap/data_collection',
                            label: 'Data Collection'
                          },
                          {
                            type: 'doc',
                            id: 'fb/related_teams/pogo/pogo_team_internal/betamap/data_pipelines',
                            label: 'Data Pipelines'
                          },
                          {
                            type: 'doc',
                            id: 'fb/related_teams/pogo/pogo_team_internal/betamap/betamap_format',
                            label: 'Betamap Format'
                          },
                          {
                            type: 'doc',
                            id: 'fb/related_teams/pogo/pogo_team_internal/betamap/create_a_betamap',
                            label: 'Create a Betamap'
                          },
                          {
                            type: 'doc',
                            id: 'fb/related_teams/pogo/pogo_team_internal/betamap/additional_resources',
                            label: 'Additional Resources'
                          },
                        ],
                      },
                      {
                        type: 'category',
                        label: 'DeepData',
                        items: [
                          {
                            type: 'doc',
                            id: 'fb/related_teams/pogo/pogo_team_internal/deepdata/overview',
                            label: 'Overview'
                          },
                          {
                            type: 'doc',
                            id: 'fb/related_teams/pogo/pogo_team_internal/deepdata/source_blocks',
                            label: 'Source Blocks'
                          },
                          {
                            type: 'doc',
                            id: 'fb/related_teams/pogo/pogo_team_internal/deepdata/profiling',
                            label: 'Profiling'
                          },
                          {
                            type: 'doc',
                            id: 'fb/related_teams/pogo/pogo_team_internal/deepdata/optimizations',
                            label: 'Optimizations'
                          },
                        ],
                      },
                      {
                        type: 'category',
                        label: 'Baseline Profiles',
                        items: [
                          {
                            type: 'doc',
                            id: 'fb/related_teams/pogo/pogo_team_internal/baseline_profiles/overview',
                            label: 'Overview'
                          },
                          {
                            type: 'doc',
                            id: 'fb/related_teams/pogo/pogo_team_internal/baseline_profiles/generation',
                            label: 'Generation'
                          },
                          {
                            type: 'doc',
                            id: 'fb/related_teams/pogo/pogo_team_internal/baseline_profiles/experimentation',
                            label: 'Experimentation'
                          }
                        ]
                      }
                    ],
                  }
                ],
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
