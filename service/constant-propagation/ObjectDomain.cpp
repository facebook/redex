/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ObjectDomain.h"

namespace escape_domain_impl {

Lattice lattice({EscapeState::BOTTOM, EscapeState::NOT_ESCAPED,
                 EscapeState::MAY_ESCAPE},
                {{EscapeState::BOTTOM, EscapeState::NOT_ESCAPED},
                 {EscapeState::NOT_ESCAPED, EscapeState::MAY_ESCAPE}});

} // namespace escape_domain_impl

std::ostream& operator<<(std::ostream& os, const EscapeDomain& dom) {
  auto elem = dom.element();
  switch (elem) {
  case EscapeState::MAY_ESCAPE:
    os << "ESCAPED";
    break;
  case EscapeState::NOT_ESCAPED:
    os << "NOT_ESCAPED";
    break;
  case EscapeState::BOTTOM:
    os << "_|_";
    break;
  }
  return os;
}
