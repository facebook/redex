#!/bin/bash -e
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Fetches the current test-data tarball from dewey.
#
# Usage:
#
# $ ./fetch-test-data.sh

cd $(dirname "$0")

commit=`grep 'TESTDATA_COMMIT =' BUCK | awk '{print $3}'|sed 's/"//g'`
temp_dir=`mktemp -d`
tar_file=$temp_dir/test-data.tar.gz

echo "Fetching $commit to $temp_dir"

dewey cat --project fbsource --commit $commit --tag oatmeal/test-data \
      --dest $tar_file --path test-data.tar.gz

tar -C $temp_dir -zxf $tar_file

rm $tar_file

echo "Test data extracted to $temp_dir"
echo ""
echo "To generate new test data, copy new input to $temp_dir and run:"
echo ""
echo "  ORIG_TTY=\`tty\` UPDATE_OATMEAL_TESTDATA=1 buck run //native/redex/tools/oatmeal/test:regression-test -- $temp_dir"
echo ""
echo "This simply writes whatever output the current oatmeal binary happens"
echo "to generate to the test dir. Make sure the output is correct before continuing!"
