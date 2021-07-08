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

cd "$(dirname "$0")"

test_data_dir=$1
if [ -z "$test_data_dir" ]; then
  echo "Usage: commit-test-data.sh path/to/testdata"
  exit 1
fi

commit=$(cd "$test_data_dir" && find . -type f | sort | xargs sha1sum | sha1sum \
  | cut -f1 -d' ')

temp_dir=$(mktemp -d)
tar_file=$temp_dir/test-data.$commit.tar.xz

pushd "$(dirname "$test_data_dir")" > /dev/null
XZ_OPT="-9e -T16" tar -cfJ - "$(basename "$test_data_dir")" > $tar_file
popd > /dev/null

manifold put "$tar_file" "oatmeal/tree/test-data.$commit.tar.xz"

echo "Edit fetch-test-data.sh file with new commit id? (save changes first)"
select yn in "Yes" "No"; do
  case $yn in
      Yes )
        echo "Editing fetch file..."
        sed -i.bak -e "s/commit=.*/commit=$commit/" fetch-test-data.sh
        rm fetch-test-data.sh.bak
        break;;
      No )
        echo "Leaving fetch file alone. You probably want to update to"
        echo "commit $commit manually."
        break;;
  esac
done
