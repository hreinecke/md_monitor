#!/bin/bash
# MD monitor script
#

EVENT=$1
MD=$2
DEV=$3

/sbin/md_monitor -c "${EVENT}:${MD}@${DEV}"

