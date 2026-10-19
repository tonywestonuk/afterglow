#!/bin/sh
# BusyBox udhcpc's event script: applies the lease DHCP gives an interface (address, route, DNS).
case $1 in
    deconfig)
        ip addr flush dev "$interface"
        ip link set "$interface" up
        ;;
    bound | renew)
        ip addr replace "$ip/${mask:-24}" ${broadcast:+broadcast "$broadcast"} dev "$interface"
        for r in $router; do
            ip route replace default via "$r" dev "$interface"
            break
        done
        mkdir -p /etc
        : > /etc/resolv.conf
        for d in $dns; do echo "nameserver $d" >> /etc/resolv.conf; done
        echo "<6>afterglow: $interface has address $ip" > /dev/kmsg
        ;;
esac
