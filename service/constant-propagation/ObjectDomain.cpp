/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
    os << "MAY_ESCAPE";
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
