#!/bin/bash
#
# Testcase 16: Reboot during recovery  w/ root on MD
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MONITOR_TIMEOUT=30
logger "Monitor Testcase 16: Reboot during recovery w/ I/O on root on MD"

ulimit -c unlimited

function resume_dasd() {
    local dasd=$1

    echo "Resume $dasd"
    setdasd -q 0 -d ${dasd} || \
	error_exit "Cannot resume /dev/${dasd}"
}

MD_DEV=$(mount | grep ' / ' | cut -d ' ' -f 1)
MD_NAME=${MD_DEV#/dev/}
if [ "${MD_NAME##md}" = "${MD_NAME}" ] ; then
    echo "Testcase can only be run with root on MD"
    exit 0
fi
if [ "${MD_NAME##md/}" != "${MD_NAME}" ] ; then
    MD_NAME=${MD_NAME##md/}
fi

for dasd in $(mdadm --detail ${MD_DEV} | sed -n 's/.*set-A failfast *\/dev\/\(.*\)/\1/p') ; do
    dasd=${dasd%1}
    DASDS_LEFT+=("$dasd")
done
num=0
echo "$(date) Quiesce disks on first half ..."
for d in ${DASDS_LEFT[@]} ; do
    setdasd -q 1 -d /dev/${d} || \
	error_exit "Cannot quiesce /dev/${d}"
    push_recovery_fn "resume_dasd /dev/${d}"
done

wait_for_md_failed $MONITOR_TIMEOUT

echo "$(date) Resume disks on first half ..."
while true ; do
    if ! pop_recovery_fn ; then
	break;
    fi
done

wait_for_md_running_left $MONITOR_TIMEOUT

sleep 2
# Stop z/VM guest
vmcp cp cpu all stop
