/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// See https://docusaurus.io/docs/site-config for all the possible
// site configuration options.

const siteConfig = {
	title: "Redex", // Title for your website.
	tagline: "An Android Bytecode Optimizer",
	url: "https://fbredex.com",
	baseUrl: "/",

	// Used for publishing and more
	organizationName: "facebook",
	projectName: "redex",

	favicon: "img/favicon.png",

	/* Colors for website */
	// colors: {
	//   primaryColor: '#75c7a4',
	//   secondaryColor: '#f9f9f9',
	// },

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

	// highlight: {
	// 	// Highlight.js theme to use for syntax highlighting in code blocks.
	// 	theme: "default"
	// },

	// Add custom scripts here that would be placed in <script> tags.
	scripts: ["https://buttons.github.io/buttons.js"],

	// You may provide arbitrary config keys to be used as needed by your
	// template. For example, if you need your repo's URL...
	// repoUrl: "https://github.com/facebook/redex",
	presets: [
		[
			"@docusaurus/preset-classic",
			{
				docs: {
					// docs folder path relative to website dir.
					path: "../docs",
					// sidebars file relative to website dir.
					sidebarPath: require.resolve("./sidebars.json"),
					// Equivalent to `enableUpdateBy`.
					showLastUpdateAuthor: true,
					// Equivalent to `enableUpdateTime`.
					showLastUpdateTime: true
				},
				theme: {
					customCss: require.resolve("./src/css/custom.css")
				}
			}
		]
	],
	themeConfig: {
		navbar: {
			title: "Redex",
			logo: {
				alt: "Redex Logo",
				src: "img/redex.png"
			},
			links: [
				{ to: "docs/installation", label: "Docs", position: "right" },
				{ to: "docs/faq", label: "FAQ", position: "right" },
				// {to: 'blog', label: 'Blog'}, // put back when we create a blog folder and add our first blog
				{
					href:
						"https://code.facebook.com/posts/1480969635539475/optimizing-android-bytecode-with-redex",
					label: "Birth",
					position: "right"
				}
			]
		},
		footer: {
			logo: {
				alt: "Redex Logo",
				src: "img/redex.png"
			},
			// This copyright info is used in /core/Footer.js and blog RSS/Atom feeds.
			copyright: `Copyright © ${new Date().getFullYear()} Facebook Inc.`
		},
		image: "img/og_image.png"
	}
};

module.exports = siteConfig;
