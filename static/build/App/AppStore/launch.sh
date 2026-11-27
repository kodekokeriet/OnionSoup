#!/bin/sh
sysdir=/mnt/SDCARD/.tmp_update

touch /tmp/stay_awake
cd $sysdir
if [ -x ./bin/appStore ]; then
	./bin/appStore
else
	st -q -e sh $sysdir/script/appstore.sh
fi
rm -f /tmp/stay_awake
# WiFi was off before the store turned it on: turn it off again (battery)
if [ -f /tmp/appstore_wifi_on ] && [ "$(/customer/app/jsonval wifi)" != 1 ]; then
	pkill -9 wpa_supplicant
	pkill -9 udhcpc
	/customer/app/axp_test wifioff
fi
rm -f /tmp/appstore_wifi_on
