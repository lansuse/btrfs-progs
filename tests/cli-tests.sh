#!/bin/bash
#
# command line interface coverage tests

LANG=C
SCRIPT_DIR=$(dirname $(readlink -f "$0"))
if [ -z "$TOP" ]; then
	TOP=$(readlink -f "$SCRIPT_DIR/../")
	if [ -f "$TOP/configure.ac" ]; then
		# inside git
		TEST_TOP="$TOP/tests"
		INTERNAL_BIN="$TOP"
	else
		# external, defaults to system binaries
		TOP=$(dirname `type -p btrfs`)
		TEST_TOP="$SCRIPT_DIR"
		INTERNAL_BIN="$TEST_TOP"
	fi
else
	# assume external, TOP set from commandline
	TEST_TOP="$SCRIPT_DIR"
	INTERNAL_BIN="$TEST_TOP"
fi
if ! [ -x "$TOP/btrfs" ]; then
	echo "ERROR: cannot execute btrfs from TOP=$TOP"
	exit 1
fi
TEST_DEV=${TEST_DEV:-}
RESULTS="$TEST_TOP/cli-tests-results.txt"
IMAGE="$TEST_TOP/test.img"

source "$TEST_TOP/common"

export INTERNAL_BIN
export TEST_TOP
export TOP
export RESULTS
export LANG
export IMAGE
export TEST_DEV

rm -f "$RESULTS"

check_prereq btrfs
check_kernel_support

# The tests are driven by their custom script called 'test.sh'

test_found=0

for i in $(find "$TEST_TOP/cli-tests" -maxdepth 1 -mindepth 1 -type d	\
	${TEST:+-name "$TEST"} | sort)
do
	name=$(basename "$i")
	if ! [ -z "$TEST_FROM" ]; then
		if [ "$test_found" == 0 ]; then
			case "$name" in
				$TEST_FROM) test_found=1;;
			esac
		fi
		if [ "$test_found" == 0 ]; then
			printf "    [TEST/cli]   %-32s (SKIPPED)\n" "$name"
			continue
		fi
	fi
	cd "$i"
	if [ -x test.sh ]; then
		echo "=== START TEST $i" >> "$RESULTS"
		echo "    [TEST/cli]   $name"
		./test.sh
		if [ $? -ne 0 ]; then
			if [[ $TEST_LOG =~ dump ]]; then
				cat "$RESULTS"
			fi
			_fail "test failed for case $name"
		fi
	else
		_fail "custom test script not found or lacks execution permission"
	fi
	cd "$TEST_TOP"
done
