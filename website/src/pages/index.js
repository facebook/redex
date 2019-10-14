/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

import React from 'react';
import classnames from 'classnames';
import Layout from '@theme/Layout';
// const CompLibrary = require('../../core/CompLibrary.js');
// const GridBlock = CompLibrary.GridBlock;
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import styles from './index.module.css';

function imgUrl(img,siteConfig) {
  return siteConfig.baseUrl + 'img/' + img;
}

function docUrl(doc, language,siteConfig) {
  return siteConfig.baseUrl + 'docs/' + (language ? language + '/' : '') + doc;
}

function pageUrl(page, language,siteConfig) {
  return siteConfig.baseUrl + (language ? language + '/' : '') + page;
}

class Button extends React.Component {
  render() {
    return (
      <div className="pluginWrapper buttonWrapper">
        <a className="button" href={this.props.href} target={this.props.target}>
          {this.props.children}
        </a>
      </div>
    );
  }
}

Button.defaultProps = {
  target: '_self',
};

const SplashContainer = props => (
  <div className="homeContainer">
    <div className="homeSplashFade">
      <div className="wrapper homeWrapper">{props.children}</div>
    </div>
  </div>
);

const Logo = props => (
  <div className={classnames("projectLogo")}>
    <img src={props.img_src} />
  </div>
);

const ProjectTitle = props => (
  <h2 className={classnames("projectTitle")}>
    {props.siteConfig.title}
    <small>{props.siteConfig.tagline}</small>
  </h2>
);

const PromoSection = props => (
  <div className="section promoSection">
    <div className="promoRow">
      <div className="pluginRowBlock">{props.children}</div>
    </div>
  </div>
);

function HomeSplash(props){
  
    let language = props.language || '';
    const context = useDocusaurusContext();
    const { siteConfig = {} } = context;
    console.log('Homesplash',context,siteConfig,language)
    return (
      <SplashContainer>
        <Logo img_src={imgUrl('redex-hero.png',siteConfig)} />
        <div className={classnames("inner")}>
          <ProjectTitle siteConfig={siteConfig}/>
          <PromoSection>
            <Button href={docUrl('installation', language,siteConfig)}>Getting Started</Button>
            <Button href='https://github.com/facebook/redex'>GitHub</Button>
          </PromoSection>
        </div>
      </SplashContainer>
    );

}

const Block = props => (
  <Container
    padding={['bottom', 'top']}
    id={props.id}
    background={props.background}>
    <GridBlock align="left" contents={props.children} layout={props.layout} />
  </Container>
);

const Container=(props)=>(
  <div style={{backgroundColor:"pink"}}>
    {props.children}
  </div>
)

const GridBlock=props=>(
  <div>
    {
      props.contents.map(contentItem=>{
        <div>
          <h3>
            {contentItem.title}
          </h3>
          {contentItem.content}
          </div>
      })
    }
  </div>
)

function Index(props) {
    let language = props.language || '';
    console.log('Index',props)
    const context = useDocusaurusContext();
    const { siteConfig = {} } = context;
    return (
      <Layout
        title={siteConfig.title}
        description={siteConfig.description}  
      >
        <div>
          <HomeSplash language={language} />
          <div className={classnames("mainContainer")}>
            <Container padding={['bottom', 'top']}>
              <GridBlock align="center" layout="fourColumn" contents={[
                  {
                    title: 'Optimizing',
                    content: 'Redex provides a framework for reading, writing, and analyzing .dex files, and a set of optimization passes that use this framework to improve the bytecode. An APK optimized by Redex should be smaller and faster.',
                  },
                  {
                    title: 'Fast',
                    content: 'Fewer bytes also means faster download times, faster install times, and lower data usage for cell users. Lastly, less bytecode also typically translates into faster runtime performance.',
                  },
                  {
                    title: 'Buck Integration',
                    content: 'Redex has deep integration with Buck where your Redex config is passed as a parameter to the Buck android_binary rule when generating the APK.',
                  }
                ]} />
            </Container>
          </div>
        </div>
      </Layout>
    );
}

export default Index;