#!/bin/bash -e

# Runs the regtest, overwriting previous output, and prompts user with diffs
# showing the changes.
#
# Usage:
# ./update-test-data.sh

if tty > /dev/null; then
  cur_tty=`tty`
fi

ORIG_TTY=$cur_tty UPDATE_OATMEAL_TESTDATA=1 buck run //native/redex/tools/oatmeal/test:regression-test
