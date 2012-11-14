#!/bin/bash
#
# Testcase 8: Accidental disk overwrite
#

. ./monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase8"
DEVNOS_LEFT="0.0.0200 0.0.0201 0.0.0202 0.0.0203"
DEVNOS_RIGHT="0.0.0210 0.0.0211 0.0.0212 0.0.0213"
SLEEPTIME=30

logger "Monitor Testcase 8: Accidental DASD overwrite"

stop_md $MD_NUM

activate_dasds

clear_metadata

modprobe vmcp

ulimit -c unlimited
start_md $MD_NUM $MD_NAME

echo "Create filesystem ..."
if ! mkfs.ext3 /dev/${MD_NUM} ; then
    error_exit "Cannot create fs"
fi

echo "Mount filesystem ..."
if ! mount /dev/${MD_NUM} /mnt ; then
    error_exit "Cannot mount MD array."
fi

echo "Write test file ..."
dd if=/dev/zero of=/mnt/testfile1 bs=4096 count=1024

echo "Invoke flashcopy"
vmcp flashcopy 5000 0 16 to 5001 0 16

echo "Waiting for MD to pick up changes ..."
# Wait for md_monitor to pick up changes
sleeptime=0
num=${#DASDS_LEFT[@]}
while [ $num -gt 0  ] ; do
    for d in ${DASDS_LEFT[@]} ; do
	device=$(sed -n "s/${MD_NUM}.* \(${d}1\[[0-9]\](F)\).*/\1/p" /proc/mdstat)
	if [ "$device" ] ; then
	    (( num -- ))
	fi
    done
    [ $num -eq 0 ] && break
    num=${#DASDS_LEFT[@]}
    sleep 1
    (( sleeptime ++ ))
done
echo "MD monitor picked up changes after $sleeptime seconds"

echo "MD status"
mdadm --detail /dev/${MD_NUM}
echo "Write second test file ..."
dd if=/dev/zero of=/mnt/testfile2 bs=4096 count=1024

echo "Umount filesystem ..."
umount /mnt

stop_md $MD_NUM
