#!/bin/bash -e

# Regression test for oatmeal.
#
# To add new test data, run ./fetch-test-data.sh, then follow the
# instructions it prints.
#
# To update existing test data after making changes, run
# ./update-test-data.sh

# buck run leaves stdout/stderr alone, but it takes away our tty.
if [ -n "$ORIG_TTY" ]; then
  exec < $ORIG_TTY
fi

# <argument-parsing type="janky">

oatmeal_binary=$1

# Note: test data can be a tarball or a directory.
test_data=$2

# directory can be overridden when running
# buck run //native/redex/tools/oatmeal/test:regression-test -- <test dir>
if [ -n "$3" ]; then
  test_data=$3
fi

# </argument-parsing>

if [ -z "$test_data" ]; then
  echo "Usage: regression-test.sh <oatmeal binary> <test-data-location>"
  exit 1
fi

if [ -f "$test_data" ]; then
  test_data_dir=`mktemp -d`
  tar -C $test_data_dir -zxf $test_data
else
  test_data_dir=$test_data
fi

test_output_dir=`mktemp -d`

pushd $test_data_dir

files=`find test-data -type f -name '*.oat.rodata'`

for f in $files; do
  actual=$test_output_dir/$f.actual
  expected=$f.expected
  mkdir -p `dirname $actual`
  if [ -z "$UPDATE_OATMEAL_TESTDATA" ]; then
    $oatmeal_binary -m -t -d -o $f > $actual || \
      echo -e "==============\nExit status $?" >> $actual
    if ! diff $actual $f.expected > /dev/null; then
      echo "Output differed for expected for $f"
      echo "Delta:"
      diff $actual $expected
    fi
  else
    echo "Updating expected output for $f"
    new_output=`mktemp`
    $oatmeal_binary -m -t -d -o $f > $new_output || \
      echo -e "==============\nExit status $?" >> $new_output
    if [ -f $expected ] && ! diff $expected $new_output > /dev/null; then
      echo "\$ diff $expected $new_output"
      diff $expected $new_output | head -50
      echo "Accept these changes?"
      select yn in "Yes" "No"; do
          case $yn in
              Yes ) mv $new_output $expected; break;;
              No ) break;;
          esac
      done
    else
      mv $new_output $expected
    fi
  fi
done

if [ -n "$UPDATE_OATMEAL_TESTDATA" ]; then
  echo "New test output is in $test_data_dir. To commit this data run:"
  echo ""
  echo "  ./commit-test-data.sh $test_data_dir/test-data"
  echo ""
  echo "Be sure to check in the BUCK file changes, which contain the new commit id"
  echo ""
  echo "Test is now failing, because UPDATE_OATMEAL_TESTDATA is set"
  exit 1
fi

echo "PASSED"
exit 0
