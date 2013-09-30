#!/bin/bash
#
# monitor_testcase_all.sh
#
# Run all testcases and stop on failure
#

run_max=4

for case in $(seq 1 14) ; do
    script=monitor_testcase_${case}.sh

    for run in $(seq 0 $run_max) ; do
	echo "Run $script, run $run from $run_max"
	if ! eval ./${script} ; then
	    failed=$script
	    break;
	fi
    done
    [ -n "$failed" ] && break
done

if [ -n "$failed" ] ; then
    echo "$failed failed"
    exit 1
fi
echo "All tests completed"
