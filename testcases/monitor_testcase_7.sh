#!/bin/bash
#
# Testcase 7: Reshape RAID
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase7"
RESHAPE_TIMEOUT=60

stop_md $MD_NUM

activate_dasds

clear_metadata

ulimit -c unlimited
start_md $MD_NUM 6

logger "${MD_NAME}: expand RAID"

echo "Create filesystem ..."
if ! mkfs.ext3 /dev/${MD_NUM} ; then
    error_exit "Cannot create fs"
fi

echo "Mount filesystem ..."
if ! mount /dev/${MD_NUM} /mnt ; then
    error_exit "Cannot mount MD array."
fi

echo "Add ${DEVICES_LEFT[3]} ${DEVICES_RIGHT[3]}"
mdadm --add /dev/${MD_NUM} \
    --failfast ${DEVICES_LEFT[3]} ${DEVICES_RIGHT[3]} \
    || error_exit "Cannot add devices"

echo "Expand array"
mdadm --grow /dev/${MD_NUM} --raid-devices=8 \
    || error_exit "Cannot expand array"

echo "Waiting for reshape to finish"
wait_for_sync ${MD_NUM} || \
    error_exit "Failed to synchronize array"


# Wait for lazy bitmap update to finish
echo "Wait for bitmap to clear"
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

echo "Resize bitmap"
mdadm --grow /dev/${MD_NUM} --bitmap=none \
    || error_exit "Cannot remove bitmap"

mdadm --grow /dev/${MD_NUM} --bitmap=internal --bitmap-chunk=512K \
    || error_exit "Cannot update bitmap size"

cat /proc/mdstat
sleep 5

echo "Resize array"
raid_size=$(sed -n 's/ *\([0-9]*\) blocks .*/\1/p' /proc/mdstat)
raid_size=$(( raid_size / 8 ))
raid_size=$(( raid_size * 6 ))
mdadm --grow /dev/${MD_NUM} --array-size=$raid_size \
    || error_exit "Cannot resize array"

mdadm --grow /dev/${MD_NUM} --raid-devices=6 \
    || error_exit "Cannot reshape array"

wait_for_sync ${MD_NUM} \
    || error_exit "Failed to synchronize array"

sleep 5

# Bug#763212
echo "Removing spare devices"
for dev in $(mdadm --detail /dev/${MD_NUM} | sed -n 's/.*spare *\(\/dev\/dasd[a-z]*[0-9]*\)/\1/p') ; do
    mdadm --manage /dev/${MD_NUM} --remove ${dev} \
	|| error_exit "Cannot remove spare device ${dev}"
done

mdadm --detail /dev/${MD_NUM}

logger "${MD_NAME}: success"

echo "Umount filesystem ..."
umount /mnt

stop_md $MD_NUM
