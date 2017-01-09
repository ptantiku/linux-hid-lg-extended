#!/bin/bash


#select keyboard device, should only have one device
FILE=$(find /sys/bus/hid/devices/*/ -name lcd_page|head -n 1)
BASEDIR=$(dirname $FILE)
NAME=$(cat $BASEDIR/name)
echo "Device = ${NAME}"

date
date +"%y %m %e" > $BASEDIR/date
date +%T > $BASEDIR/time
