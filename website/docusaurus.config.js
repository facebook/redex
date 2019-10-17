/**
* Copyright (c) 2017-present, Facebook, Inc.
*
* This source code is licensed under the MIT license found in the
* LICENSE file in the root directory of this source tree.
*/

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
				src: "img/favicon.png"
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
			style: "dark",
			logo: {
				alt: "Facebook Open Source Logo",
				src: "img/oss_logo.png"
			},
			links: [
				{
					title: "Docs",
					items: [
						{
							label: "Getting Started",
							to: "docs/installation"
						},
						{
							label: "Configuring",
							to: "docs/configuring"
						},
						{
							label: "Using",
							to: "docs/usage"
						},
						{
							label: "FAQ",
							to: "docs/faq"
						}
					]
				},
				{
					title: "Social",
					items: [
						{
							label: "Github",
							href: "https://github.com/facebook/redex"
						}
					]
				}
			],
			// This copyright info is used in /core/Footer.js and blog RSS/Atom feeds.
			copyright: `Copyright © ${new Date().getFullYear()} Facebook Inc.`
		},
		image: "img/og_image.png"
	}
};

module.exports = siteConfig;
