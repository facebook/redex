/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// See https://docusaurus.io/docs/site-config for all the possible
// site configuration options.

const siteConfig = {
  algolia: {
    apiKey: '7ef33486bb9b0c17ed9a5dedb0da8e36',
    indexName: 'fbredex',
  },
  title: 'Redex', // Title for your website.
  tagline: 'An Android Bytecode Optimizer',
  url: 'https://fbredex.com',
  baseUrl: '/',
  cname: 'fbredex.com',

  // Used for publishing and more
  organizationName: 'facebook',
  projectName: 'redex',


  // For no header links in the top nav bar -> headerLinks: [],
  headerLinks: [
    {doc: 'installation', label: 'Docs'},
    {doc: 'faq', label: 'FAQ'},
    // {blog: true, label: 'Blog'}, // put back when we create a blog folder and add our first blog
    {
      href: 'https://code.facebook.com/posts/1480969635539475/optimizing-android-bytecode-with-redex',
      label: 'Birth',
    }
  ],

  /* path to images for header/footer */
  headerIcon: 'img/redex.png',
  footerIcon: 'img/redex.png',
  favicon: 'img/favicon.png',

  /* Colors for website */
  colors: {
    primaryColor: '#75c7a4',
    secondaryColor: '#f9f9f9',
  },

  /* Custom fonts for website */
  /*
  fonts: {
    myFont: [
      "Times New Roman",
      "Serif"
    ],
    myOtherFont: [
      "-apple-system",
      "system-ui"
    ]
  },
  */

  // This copyright info is used in /core/Footer.js and blog RSS/Atom feeds.
  copyright: `Copyright © ${new Date().getFullYear()} Meta Platforms Inc.`,

  highlight: {
    // Highlight.js theme to use for syntax highlighting in code blocks.
    theme: 'default',
  },

  // Add custom scripts here that would be placed in <script> tags.
  scripts: ['https://buttons.github.io/buttons.js'],

  // On page navigation for the current documentation page.
  onPageNav: 'separate',
  // No .html extensions for paths.
  cleanUrl: true,

  // Open Graph and Twitter card images.
  ogImage: 'img/og_image.png',

  // Show documentation's last contributor's name.
  enableUpdateBy: true,

  // Show documentation's last update time.
  enableUpdateTime: true,

  // You may provide arbitrary config keys to be used as needed by your
  // template. For example, if you need your repo's URL...
  repoUrl: 'https://github.com/facebook/redex',
};

module.exports = siteConfig;
