/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "CallSiteSummaries.h"
#include "DeterministicContainers.h"

namespace partial_application {

/*
 * Whether every constant argument bound by `reduced` is also bound, with the
 * same value, by `full`.
 *
 * This is the safety condition for the whole pass: a call-site may only be
 * rewritten to invoke a partial-application helper method that binds `reduced`
 * if the call-site's own constant arguments `full` actually agree with all of
 * those bindings. Otherwise the helper would pass constants the call-site never
 * passed.
 */
inline bool is_reduction_of(const CallSiteSummary* reduced,
                            const CallSiteSummary* full) {
  if (reduced->result_used != full->result_used) {
    return false;
  }
  if (reduced->arguments.is_top()) {
    return true;
  }
  if (full->arguments.is_top()) {
    return false;
  }
  for (const auto& p : reduced->arguments.bindings()) {
    if (!full->arguments.get(p.first).equals(p.second)) {
      return false;
    }
  }
  return true;
}

/*
 * Records how call-site summaries get weakened while searching for a
 * beneficial subset of constant arguments: `add(from, to)` states that `to` is
 * `from` with one constant argument dropped, and that any call-site whose
 * arguments match `from` may therefore be served by a helper method binding
 * `to` instead.
 *
 * Following the recorded chain from any summary only ever drops bindings, so
 * every summary reachable this way is a valid substitute for the one we
 * started at. A summary may be reduced more than once (it can re-enter the
 * priority queue after having already been reduced, once other summaries
 * transfer their costs onto it); the chain then reflects the most recent
 * reduction, which is still a valid substitute.
 *
 * Note that a *set*-based representation (such as union-find over the merged
 * summaries) is not sufficient here: it loses the direction of the reduction,
 * and can end up serving a call-site with a helper method that binds strictly
 * more constants than the call-site actually passes.
 */
class CallSiteSummaryReductions {
 public:
  void add(const CallSiteSummary* from, const CallSiteSummary* to) {
    m_reductions[from] = to;
  }

  /*
   * Returns the strongest summary reachable from `css` by dropping constant
   * arguments for which `is_selected` holds, or nullptr if there is none. The
   * result is always a reduction of `css` in the `is_reduction_of` sense.
   */
  template <typename IsSelectedFn>
  const CallSiteSummary* find_selected(const CallSiteSummary* css,
                                       IsSelectedFn is_selected) const {
    // Terminates because every hop strictly decreases the number of bindings.
    while (css != nullptr && !is_selected(css)) {
      auto it = m_reductions.find(css);
      css = it == m_reductions.end() ? nullptr : it->second;
    }
    return css;
  }

 private:
  UnorderedMap<const CallSiteSummary*, const CallSiteSummary*> m_reductions;
};

} // namespace partial_application
