#!/bin/sh
# Wi-Fi from afterglow's shell, through wpa_supplicant (started by bringup.sh wifi).
#   sh /afterglow/wifi.sh scan                    networks in range, strongest last
#   sh /afterglow/wifi.sh join SSID_HEX PASS_HEX  add a network and connect (hex, so any characters
#                                                 survive the shell; empty PASS_HEX for an open one)
#   sh /afterglow/wifi.sh status                  connection state and address
#   sh /afterglow/wifi.sh country CC              regulatory country (ISO 3166, e.g. GB): which
#                                                 channels and power are allowed; the driver assumes US
# From a computer, tools/console.py --wifi asks for the network and password and runs join.
set -u
cli() { wpa_cli -i wlan0 "$@"; }
events=/tmp/wpa_supplicant.log   # wpa_supplicant's output (bringup.sh)

case ${1:-} in
    scan)
        cli scan > /dev/null && sleep 5
        cli scan_results | tail -n +2 | sort -t "$(printf '\t')" -k3 -n |
            awk -F '\t' '{ printf "%5s dBm  %4s MHz  %-34s %s\n", $3, $2, ($5 == "" ? "(hidden)" : $5), $4 }'
        ;;
    join)
        ssid=${2:?SSID as hex}
        pass=$(printf "$(echo "${3:-}" | sed 's/../\\x&/g')")
        seen=$(wc -l < "$events")
        id=$(cli add_network | tail -n 1)
        cli set_network "$id" ssid "$ssid" > /dev/null
        if [ -n "$pass" ]; then
            # The passphrase serves WPA2 and WPA3 (SAE) alike; PMF is used when the network offers it
            cli set_network "$id" psk "\"$pass\"" | grep -q OK || { echo "rejected password"; exit 1; }
            cli set_network "$id" key_mgmt WPA-PSK WPA-PSK-SHA256 SAE > /dev/null
            cli set_network "$id" ieee80211w 1 > /dev/null
        else
            cli set_network "$id" key_mgmt NONE > /dev/null
        fi
        cli enable_network "$id" > /dev/null
        i=0
        until cli status | grep -q '^wpa_state=COMPLETED'; do
            i=$((i + 1))
            if [ $i -gt 20 ]; then
                if tail -n +$((seen + 1)) "$events" | grep -q "id=$id .*reason=WRONG_KEY"; then
                    why="wrong password"
                else
                    why=$(cli status | grep wpa_state)
                fi
                cli remove_network "$id" > /dev/null   # don't keep trying it
                echo "not connected after 20 s ($why)"
                exit 1
            fi
            sleep 1
        done
        cli save_config > /dev/null
        echo "connected to $(cli status | sed -n 's/^ssid=//p')"
        ;;
    status)
        cli status | grep -E '^(wpa_state|ssid|freq|key_mgmt|ip_address)='
        cli signal_poll | grep -E '^(RSSI|LINKSPEED)='
        ;;
    country)
        cc=$(echo "${2:?country code}" | tr a-z A-Z)
        cli set country "$cc" | grep -q OK || { echo "rejected country $cc"; exit 1; }
        cli save_config > /dev/null
        echo "country $cc"
        ;;
    *)
        sed -n '3,9p' "$0"
        exit 1
        ;;
esac
