#!/bin/bash

clear
echo "Displaying current status for Logitech Devices"
echo
echo "Current driver info is:"
echo
echo "For: hid_logitech_core"
echo
modinfo hid_logitech_core
echo
echo "For: hid_logitech_mx5500"
echo
modinfo hid_logitech_mx5500
echo

FILES=$(find /sys/bus/hid/devices/*/ -name battery)

if [ -z "$DBUS_SESSION_BUS_ADDRESS" ]
then
	if [ -z "$DISPLAY" ]
	then
		export $(grep DISPLAY -z /proc/$(ps U $(id -u) | grep dbus-daemon | head -1 | cut -d' ' -f2)/environ)
	fi
	export $(grep ^DBUS_SESSION_BUS_ADDRESS $HOME/.dbus/session-bus/$(dbus-uuidgen --get)-$(echo $DISPLAY | sed -e 's/\([^:]*:\)//g' -e 's/\..*$//g'))
fi

for FILE in $FILES
do
	LEVEL=$(cat $FILE)
	LEVEL=${LEVEL//%/}
	NAME=$(cat $(dirname $FILE)/name)
	echo "Battery level:" 
	echo "The batteries of device ${NAME} are reporting ${LEVEL}% remaining"
	notify-send "Battery level" "The batteries of device ${NAME} are reporting. Only ${LEVEL}% remaining" -t 5000
	NAME=$(cat $(dirname $FILE)/date)
	echo
	echo "Keyboard Date:" 
	echo "${NAME} " 

	NAME=$(cat $(dirname $FILE)/time)
	echo
	echo "Keyboard Time - ignore leading 00: " 
	echo "${NAME} " 

	NAME=$(cat $(dirname $FILE)/lcd_page)
	echo
	echo "LCD_PAGE is:"
	echo "${NAME} " 
	echo
done
