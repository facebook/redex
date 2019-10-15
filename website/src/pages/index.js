/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

import React from "react";
import Layout from "@theme/Layout";
import useDocusaurusContext from "@docusaurus/useDocusaurusContext";
import classnames from "classnames";
import withBaseUrl from "@docusaurus/withBaseUrl";

const features = [
	{
		title: "Optimizing",
		content:
			"Redex provides a framework for reading, writing, and analyzing .dex files, and a set of optimization passes that use this framework to improve the bytecode. An APK optimized by Redex should be smaller and faster."
	},
	{
		title: "Fast",
		content:
			"Fewer bytes also means faster download times, faster install times, and lower data usage for cell users. Lastly, less bytecode also typically translates into faster runtime performance."
	},
	{
		title: "Buck Integration",
		content:
			"Redex has deep integration with Buck where your Redex config is passed as a parameter to the Buck android_binary rule when generating the APK."
	}
];

class Button extends React.Component {
	render() {
		return (
			<div className="pluginWrapper buttonWrapper" style={{ padding: "0 2px" }}>
				<a className="button" href={this.props.href} target={this.props.target}>
					{this.props.children}
				</a>
			</div>
		);
	}
}

Button.defaultProps = {
	target: "_self"
};

const Logo = props => (
	<div className="projectLogo" style={{ padding: "0px 150px 70px 0px" }}>
		<img src={props.img_src} />
	</div>
);

function Index(props) {
	let language = props.language || "";
	const context = useDocusaurusContext();
	const { siteConfig = {} } = context;
	return (
		<Layout title={siteConfig.title} description={siteConfig.description}>
			<div>
				<header className={classnames("hero hero--primary")}>
					<div className="container padding-vert--lg">
						<div className="row">
							<div className="col text--center">
								<h1 className="hero__title">{siteConfig.title}</h1>
								<p className="hero__subtitle">{siteConfig.tagline}</p>
								<div className="buttons">
									<Button href={withBaseUrl("docs/installation")}>
										GETTING STARTED
									</Button>
									<Button href="https://github.com/facebook/redex">
										GITHUB
									</Button>
								</div>
							</div>
							<Logo img_src={withBaseUrl("img/redex-hero.png")} />
						</div>
					</div>
				</header>
				<div className="mainContainer">
					{features && features.length && (
						<section className="features">
							<div className="container">
								<div className="row padding-vert--xl">
									{features.map(({ imageUrl, title, content }, idx) => (
										<div key={idx} className="col col--4 text--center">
											{imageUrl && (
												<div className="text--center margin-bottom--lg">
													<img
														className="featureImage"
														src={withBaseUrl(imageUrl)}
														alt={title}
													/>
												</div>
											)}
											<h3>{title}</h3>
											<p>{content}</p>
										</div>
									))}
								</div>
							</div>
						</section>
					)}
				</div>
			</div>
		</Layout>
	);
}

export default Index;
