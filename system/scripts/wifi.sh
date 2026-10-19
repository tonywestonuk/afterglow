#!/bin/sh
# Wi-Fi from afterglow's shell, through wpa_supplicant (started by bringup.sh wifi). Networks are saved
# in /storage/wifi when the headset has afterglow storage, so it reconnects by itself after a restart;
# otherwise they last until the restart.
#   sh /afterglow/wifi.sh scan                       networks in range, strongest last
#   sh /afterglow/wifi.sh join SSID_HEX PASS_HEX     add a network and connect (hex, so any characters
#                                                    survive the shell; empty PASS_HEX for an open one)
#   sh /afterglow/wifi.sh join-key SSID_HEX KEY_HEX  the same with the 64-digit WPA2 key derived from the
#                                                    password, so the password itself isn't stored
#                                                    (WPA2 and WPA2/WPA3 networks; not WPA3-only ones)
#   sh /afterglow/wifi.sh status                     connection state and address
#   sh /afterglow/wifi.sh forget                     remove every saved network
#   sh /afterglow/wifi.sh country CC                 regulatory country (ISO 3166, e.g. GB): which
#                                                    channels and power are allowed; the driver assumes US
# From a computer, tools/console.py --wifi asks for the network and password and runs join-key.
set -u
cli() { wpa_cli -i wlan0 "$@"; }
events=/tmp/wpa_supplicant.log   # wpa_supplicant's output (bringup.sh)
unhex() { printf "$(echo "$1" | sed 's/../\\x&/g')"; }

# Saved networks with this SSID (wpa_cli shows a printable SSID quoted, any other in hex)
same_ssid() {
    for n in $(cli list_networks | tail -n +2 | cut -f 1); do
        cur=$(cli get_network "$n" ssid)
        if [ "$cur" = "\"$(unhex "$1")\"" ] || [ "$cur" = "$1" ]; then echo "$n"; fi
    done
}

# Add a network (SSID as hex, then set_network NAME VALUE pairs for its security) and connect to it.
# Saved networks with the same SSID are replaced only once the new one works.
join() {
    ssid=$1
    shift
    olds=$(same_ssid "$ssid")
    for o in $olds; do cli disable_network "$o" > /dev/null; done
    seen=$(wc -l < "$events")
    id=$(cli add_network | tail -n 1)
    cli set_network "$id" ssid "$ssid" > /dev/null
    while [ $# -ge 2 ]; do
        cli set_network "$id" "$1" "$2" | grep -q OK || { cli remove_network "$id" > /dev/null; echo "rejected $1"; exit 1; }
        shift 2
    done
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
            for o in $olds; do cli enable_network "$o" > /dev/null; done
            echo "not connected after 20 s ($why)"
            exit 1
        fi
        sleep 1
    done
    for o in $olds; do cli remove_network "$o" > /dev/null; done
    cli save_config > /dev/null
    echo "connected to $(cli status | sed -n 's/^ssid=//p')"
}

case ${1:-} in
    scan)
        cli scan > /dev/null && sleep 5
        cli scan_results | tail -n +2 | sort -t "$(printf '\t')" -k3 -n |
            awk -F '\t' '{ printf "%5s dBm  %4s MHz  %-34s %s\n", $3, $2, ($5 == "" ? "(hidden)" : $5), $4 }'
        ;;
    join)
        pass=$(unhex "${3:-}")
        if [ -n "$pass" ]; then
            # The passphrase serves WPA2 and WPA3 (SAE) alike; PMF is used when the network offers it
            join "${2:?SSID as hex}" psk "\"$pass\"" key_mgmt "WPA-PSK WPA-PSK-SHA256 SAE" ieee80211w 1
        else
            join "${2:?SSID as hex}" key_mgmt NONE
        fi
        ;;
    join-key)
        echo "${3:-}" | grep -qE '^[0-9a-fA-F]{64}$' || { echo "the key must be 64 hex digits"; exit 1; }
        join "${2:?SSID as hex}" psk "$3" key_mgmt "WPA-PSK WPA-PSK-SHA256" ieee80211w 1
        ;;
    status)
        cli status | grep -E '^(wpa_state|ssid|freq|key_mgmt|ip_address)='
        cli signal_poll | grep -E '^(RSSI|LINKSPEED)='
        ;;
    forget)
        cli remove_network all > /dev/null && cli save_config > /dev/null && echo "no saved networks"
        ;;
    country)
        cc=$(echo "${2:?country code}" | tr a-z A-Z)
        cli set country "$cc" | grep -q OK || { echo "rejected country $cc"; exit 1; }
        cli save_config > /dev/null
        echo "country $cc"
        ;;
    *)
        sed -n '5,15p' "$0"
        exit 1
        ;;
esac
