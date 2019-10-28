#!/bin/bash -e
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Tar and upload a test-data directory to dewey.
#
# Usage:
#
# $ ./commit-test-data.sh /path/to/test/dir
#
# You generally don't want to run this manually. Start by
# running ./fetch-test-data.sh and follow the instructions.

cd $(dirname "$0")

test_data_dir=$1
if [ -z "$test_data_dir" ]; then
  echo "Usage: commit-test-data.sh path/to/testdata"
  exit 1
fi

temp_dir=`mktemp -d`
tar_file=$temp_dir/test-data.tar.gz

pushd `dirname $test_data_dir` > /dev/null
tar -czf - `basename $test_data_dir` > $tar_file
popd > /dev/null

commit=$(cat $tar_file | shasum | awk '{print $1}')

dewey publish --verbose --create-tag \
  --commit $commit \
  --tag oatmeal/test-data --location $temp_dir

echo "Edit BUCK file with new commit id? (save changes first)"
select yn in "Yes" "No"; do
  case $yn in
      Yes )
        echo "Editing BUCK file..."
        tmp_buck=`mktemp`
        sed "s/TESTDATA_COMMIT =.*/TESTDATA_COMMIT = \"$commit\"/" < BUCK > $tmp_buck
        mv $tmp_buck BUCK
        break;;
      No )
        echo "Leaving BUCK file alone. You probably want to update to"
        echo "commit $commit manually."
        break;;
  esac
done
