/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

import React from "react";
import Layout from "@theme/Layout";
import withBaseUrl from "@docusaurus/withBaseUrl";

function Help(props) {

	const supportLinks = [
		{
			content: `Learn more using the [documentation on this site.](${withBaseUrl(
				"docs/doc1"
			)})`,
			title: "Browse Docs"
		},
		{
			content: "Ask questions about the documentation and project",
			title: "Join the community"
		},
		{
			content: "Find out what's new with this project",
			title: "Stay up to date"
		}
	];

	return (
		<Layout>
			<div className="docMainWrapper wrapper">
				<div className="container padding-vert--lg padding-top--xl">
					<div className="post">
						<header className="postHeader">
							<h1>Need help?</h1>
						</header>
						<p>This project is maintained by a dedicated group of people.</p>
					</div>
					<main>
						{supportLinks && supportLinks.length && (
							<section>
                  <div className="row padding-top--lg">
                    {supportLinks.map((link, index) => (
                      <div key={index} className="col col--4 text--break">
                        <h3>{link.title}</h3>
                        <p>{link.content}</p>
                      </div>
                    ))}
                  </div>
							</section>
						)}
					</main>
				</div>
			</div>
		</Layout>
	);
}

export default Help;
