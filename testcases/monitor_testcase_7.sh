#!/bin/bash
#
# Testcase 7: Reshape RAID
#

. ./monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase7"
RESHAPE_TIMEOUT=60

logger "Monitor Testcase 7: expand RAID"

stop_md $MD_NUM

activate_dasds

clear_metadata

ulimit -c unlimited
start_md $MD_NUM 6

echo "Create filesystem ..."
if ! mkfs.ext3 /dev/${MD_NUM} ; then
    error_exit "Cannot create fs"
fi

echo "Mount filesystem ..."
if ! mount /dev/${MD_NUM} /mnt ; then
    error_exit "Cannot mount MD array."
fi

echo "Add ${DEVICES_LEFT[3]} ${DEVICES_RIGHT[3]}"
mdadm --add /dev/${MD_NUM} --failfast ${DEVICES_LEFT[3]} ${DEVICES_RIGHT[3]}
if [ $? != 0 ] ; then
    error_exit "Cannot add devices"
fi

echo "Expand array"
mdadm --grow /dev/${MD_NUM} --raid-devices=8
if [ $? != 0 ] ; then
    error_exit "Cannot expand array"
fi

echo "Waiting for reshape to finish"
wait_for_sync ${MD_NUM}

# Wait for lazy bitmap update to finish
sleeptime=0
while [ $sleeptime -lt $RESHAPE_TIMEOUT ] ; do
    dirty=$(sed -n 's/.*bitmap: \([0-9]*\)\/[0-9]* pages.*/\1/p' /proc/mdstat)
    [ $dirty -eq 0 ] && break;
    sleep 1
    (( sleeptime ++))
done
if [ $sleeptime -ge $RESHAPE_TIMEOUT ] ; then
    error_exit "Bitmap not cleared after $sleeptime seconds"
fi

mdadm --grow /dev/${MD_NUM} --bitmap=none
if [ $? != 0 ] ; then
    error_exit "Cannot remove bitmap"
fi
mdadm --grow /dev/${MD_NUM} --bitmap=internal --bitmap-chunk=512K
if [ $? != 0 ] ; then
    error_exit "Cannot update bitmap size"
fi
cat /proc/mdstat
sleep 5

echo "Resize array"
raid_size=$(sed -n 's/ *\([0-9]*\) blocks .*/\1/p' /proc/mdstat)
raid_size=$(( raid_size / 8 ))
raid_size=$(( raid_size * 6 ))
mdadm --grow /dev/${MD_NUM} --array-size=$raid_size
if [ $? != 0 ] ; then
    error_exit "Cannot resize array"
fi

mdadm --grow /dev/${MD_NUM} --raid-devices=6
if [ $? != 0 ] ; then
    error_exit "Cannot reshape array"
fi

wait_for_sync ${MD_NUM}

sleep 5

# Bug#763212
echo "Removing spare devices"
for dev in $(mdadm --detail /dev/${MD_NUM} | sed -n 's/.*spare *\(\/dev\/dasd[a-z]*[0-9]*\)/\1/p') ; do
    mdadm --manage /dev/${MD_NUM} --remove ${dev}
    if [ $? != 0 ] ; then
	error_exit "Cannot remove spare device ${dev}"
    fi
done

mdadm --detail /dev/${MD_NUM}

echo "Umount filesystem ..."
umount /mnt

stop_md $MD_NUM
