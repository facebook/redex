/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

const lightCodeTheme = require('prism-react-renderer').themes.github;
const darkCodeTheme = require('prism-react-renderer').themes.dracula;

// With JSDoc @type annotations, IDEs can provide config autocompletion
/** @type {import('@docusaurus/types').DocusaurusConfig} */
(module.exports = {
  title: 'Redex',
  tagline: 'An Android Bytecode Optimizer',
  url: 'https://fbredex.com',
  baseUrl: '/',
  onBrokenLinks: 'throw',
  onBrokenMarkdownLinks: 'throw',
  trailingSlash: true,
  favicon: 'img/favicon.png',
  organizationName: 'facebook',
  projectName: 'Redex',
  customFields: {
    fbRepoName: 'fbsource',
    ossRepoPath: '',
  },

  presets: [
    [
      'docusaurus-plugin-internaldocs-fb/docusaurus-preset',
      /** @type {import('docusaurus-plugin-internaldocs-fb').PresetOptions} */
      ({
        docs: {
          sidebarPath: require.resolve('./sidebars.js'),
        },
        experimentalXRepoSnippets: {
          baseDir: '.',
        },
        staticDocsProject: 'Redex',
        trackingFile: '',
        blog: {
          showReadingTime: true,
          onUntruncatedBlogPosts: 'ignore',
        },
        theme: {
          customCss: require.resolve('./src/css/custom.css'),
        },
      }),
    ],
  ],

  themeConfig:
    /** @type {import('@docusaurus/preset-classic').ThemeConfig} */
    ({
      announcementBar: {
        id: 'support_ukraine',
        content:
          'Support Ukraine 🇺🇦 <a target="_blank" rel="noopener noreferrer" href="https://opensource.facebook.com/support-ukraine"> Help Provide Humanitarian Aid to Ukraine</a>.',
        backgroundColor: '#20232a',
        textColor: '#fff',
        isCloseable: false,
      },
      navbar: {
        title: 'Redex',
        logo: {
          alt: 'Redex Logo',
          src: 'img/favicon.png',
        },
        items: [
          {
            type: 'doc',
            docId: 'getting_started/installation',
            position: 'left',
            label: 'Docs',
          },
          {to: '/blog', label: 'Blog', position: 'left'},
          {
            href: 'https://github.com/facebook/redex',
            label: 'GitHub',
            position: 'right',
          },
        ],
      },
      footer: {
        style: 'dark',
        links: [
          {
            title: 'Docs',
            items: [
              {
                label: 'Getting Started',
                to: '/docs/getting_started/installation',
              },
              {
                label: 'Configuring',
                to: '/docs/getting_started/configuring',
              },
              {
                label: 'Using',
                to: '/docs/getting_started/usage',
              },
              {
                label: 'FAQ',
                to: '/docs/help/faq',
              },
            ],
          },
          {
            title: 'Legal',
            items: [
              {
                label: 'Privacy',
                href: 'https://opensource.fb.com/legal/privacy/',
                target: '_blank',
                rel: 'noreferrer noopener',
              },
              {
                label: 'Terms',
                href: 'https://opensource.fb.com/legal/terms/',
                target: '_blank',
                rel: 'noreferrer noopener',
              },
            ],
          },
          {
            title: 'Community',
            items: [
              {
                label: 'Github',
                href: 'https://github.com/facebook/redex/issues',
              }
            ],
          },
        ],
        logo: {
          alt: 'Meta Open Source Logo',
          src: 'https://docusaurus.io/img/meta_opensource_logo_negative.svg',
          href: 'https://opensource.fb.com',
        },
        copyright: `Copyright © ${new Date().getFullYear()} Meta Platforms, Inc. and affiliates.`,
      },
      prism: {
        theme: lightCodeTheme,
        darkTheme: darkCodeTheme,
      },
    }),
});
