/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

import React from 'react';
import classnames from 'classnames';

import Link from '@docusaurus/Link';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import withBaseUrl from '@docusaurus/withBaseUrl';

function SocialFooter(props) {
  const repoUrl = `https://github.com/${props.config.organizationName}/${
    props.config.projectName
  }`;
  return (
    <div className="footerSection" style={{flex:1}}>
      <h4 className="footer__title">Social</h4>
      <div className="social">
        <a
          className="github-button" // part of the https://buttons.github.io/buttons.js script in siteConfig.js
          href={repoUrl}
          data-count-href={`${repoUrl}/stargazers`}
          data-show-count="true"
          data-count-aria-label="# stargazers on GitHub"
          aria-label="Star this project on GitHub">
          {props.config.projectName}
        </a>
      </div>
    </div>
  );
}


function Footer() {
  const context = useDocusaurusContext();
  const {siteConfig = {}} = context;
  const {themeConfig = {}} = siteConfig;
  const {footer} = themeConfig;

  if (!footer) {
    return null;
  }

  const {copyright, links = [], logo} = footer;

  return (
    <footer
      className={classnames('footer', {
        'footer--dark': footer.style === 'dark',
      })}>
      <div className="container">
        {links && links.length > 0 && (
          <div className="row footer__links">
            <a href='#'>
              <img src={withBaseUrl('img/redex.png')} width="66" height="58"/>
            </a>
            {links.map((linkItem, i) => (
              <div key={i} className="col footer__col" style={{flex:1}}>
                {linkItem.title != null ? (
                  <h4 className="footer__title">{linkItem.title}</h4>
                ) : null}
                {linkItem.items != null &&
                Array.isArray(linkItem.items) &&
                linkItem.items.length > 0 ? (
                  <ul className="footer__items">
                    {linkItem.items.map(item => (
                      <li key={item.href || item.to} className="footer__item">
                        <Link
                          className="footer__link-item"
                          {...item}
                          {...(item.href
                            ? {
                                target: '_blank',
                                rel: 'noopener noreferrer',
                                href: item.href,
                              }
                            : {
                                to: withBaseUrl(item.to),
                              })}>
                          {item.label}
                        </Link>
                      </li>
                    ))}
                  </ul>
                ) : null}
              </div>
            ))}
            <SocialFooter config={siteConfig}/>
          </div>
        )}
        {(logo || copyright) && (
          <div className="text--center">
            {logo && logo.src && (
              <div className="margin-bottom--sm">
                <img className="footer__logo" alt={logo.alt} src={logo.src} />
              </div>
            )}
            {copyright}
          </div>
        )}
      </div>
    </footer>
  );
}

export default Footer;
