package main

// A minimal mDNS responder (RFC 6762): answers A queries for <name>.local with this headset's IPv4
// address, so a computer on the same network reaches it by name whatever address DHCP gave it.

import (
	"encoding/binary"
	"errors"
	"net"
	"strings"
	"time"
)

var mdnsGroup = &net.UDPAddr{IP: net.IPv4(224, 0, 0, 251), Port: 5353}

// advertise answers on the first network interface with an IPv4 address, waiting for one to have an
// address and re-joining the multicast group if the interface goes quiet (its address may have changed).
func advertise(name string) {
	host := name + ".local."
	for {
		ifi, ip := multicastInterface()
		if ifi == nil {
			time.Sleep(5 * time.Second)
			continue
		}
		conn, err := net.ListenMulticastUDP("udp4", ifi, mdnsGroup)
		if err != nil {
			klog("mDNS on %s: %v", ifi.Name, err)
			time.Sleep(10 * time.Second)
			continue
		}
		klog("answering mDNS for %s on %s (%s)", strings.TrimSuffix(host, "."), ifi.Name, ip)
		buf := make([]byte, 9000)
		for {
			conn.SetReadDeadline(time.Now().Add(5 * time.Minute))
			n, from, err := conn.ReadFromUDP(buf)
			if err != nil {
				break
			}
			if _, now := multicastInterface(); now != nil && !now.Equal(ip) {
				break // address changed: start again on the new one
			}
			if resp, unicast := mdnsAnswer(buf[:n], host, ip, from.Port != 5353); resp != nil {
				to := mdnsGroup
				if unicast {
					to = from
				}
				conn.WriteToUDP(resp, to)
			}
		}
		conn.Close()
	}
}

func multicastInterface() (*net.Interface, net.IP) {
	ifaces, _ := net.Interfaces()
	for i := range ifaces {
		ifi := &ifaces[i]
		if ifi.Flags&net.FlagUp == 0 || ifi.Flags&net.FlagLoopback != 0 || ifi.Flags&net.FlagMulticast == 0 {
			continue
		}
		addrs, _ := ifi.Addrs()
		for _, a := range addrs {
			if ipn, ok := a.(*net.IPNet); ok && ipn.IP.To4() != nil {
				return ifi, ipn.IP.To4()
			}
		}
	}
	return nil, nil
}

const (
	typeA    = 1
	typeAAAA = 28
	typeNSEC = 47
	typeANY  = 255
)

// mdnsAnswer returns the response to a query that asks for host's address, and whether to send it
// straight back to the asker. The headset only answers with its IPv4 address, and says so with an
// NSEC record (RFC 6762 6.1): without one, a Mac waits 5 s for an IPv6 answer before connecting.
// legacy marks a query from an ordinary DNS client (any source port but 5353), which gets an
// ordinary DNS reply: same ID, the question repeated, no NSEC.
func mdnsAnswer(q []byte, host string, ip net.IP, legacy bool) ([]byte, bool) {
	if len(q) < 12 || q[2]&0x80 != 0 { // too short, or a response
		return nil, false
	}
	wantA, wantAAAA, unicast := false, false, legacy
	var question []byte
	off := 12
	for i := 0; i < int(binary.BigEndian.Uint16(q[4:])); i++ {
		name, next, err := readName(q, off)
		if err != nil || next+4 > len(q) {
			return nil, false
		}
		qtype, qclass := binary.BigEndian.Uint16(q[next:]), binary.BigEndian.Uint16(q[next+2:])
		if strings.EqualFold(name, host) && (qtype == typeA || qtype == typeAAAA || qtype == typeANY) {
			wantA = wantA || qtype != typeAAAA
			wantAAAA = wantAAAA || qtype == typeAAAA
			unicast = unicast || qclass&0x8000 != 0 // the "QU" bit asks for a unicast reply
			question = binary.BigEndian.AppendUint16(encodeName(host), qtype)
			question = binary.BigEndian.AppendUint16(question, 1)
		}
		off = next + 4
	}
	if !wantA && !wantAAAA {
		return nil, false
	}
	class, ttl := uint16(0x8001), uint32(120) // IN, with mDNS's cache-flush bit
	resp := make([]byte, 12, 128)
	if legacy {
		copy(resp[0:2], q[0:2])
		class, ttl = 1, 10
	}
	binary.BigEndian.PutUint16(resp[2:], 0x8400) // response, authoritative
	a := record(host, typeA, class, ttl, ip.To4())
	nsec := record(host, typeNSEC, class, ttl, append(encodeName(host), 0, 1, 0x40)) // bitmap: only A
	var answers, extra [][]byte
	switch {
	case legacy && wantA:
		answers = [][]byte{a}
	case legacy: // AAAA: no such record
	case wantA:
		answers, extra = [][]byte{a}, [][]byte{nsec}
	default:
		answers, extra = [][]byte{nsec}, [][]byte{a}
	}
	if legacy {
		binary.BigEndian.PutUint16(resp[4:], 1)
		resp = append(resp, question...)
	}
	binary.BigEndian.PutUint16(resp[6:], uint16(len(answers)))
	binary.BigEndian.PutUint16(resp[10:], uint16(len(extra)))
	for _, r := range append(answers, extra...) {
		resp = append(resp, r...)
	}
	return resp, unicast
}

func record(name string, typ, class uint16, ttl uint32, data []byte) []byte {
	r := encodeName(name)
	r = binary.BigEndian.AppendUint16(r, typ)
	r = binary.BigEndian.AppendUint16(r, class)
	r = binary.BigEndian.AppendUint32(r, ttl)
	r = binary.BigEndian.AppendUint16(r, uint16(len(data)))
	return append(r, data...)
}

func encodeName(name string) []byte {
	var b []byte
	for _, label := range strings.Split(strings.TrimSuffix(name, "."), ".") {
		b = append(append(b, byte(len(label))), label...)
	}
	return append(b, 0)
}

// readName decodes a (possibly compressed) DNS name at off; returns it with a trailing dot, and the
// offset just after it.
func readName(msg []byte, off int) (string, int, error) {
	var labels []string
	end := -1
	for jumps := 0; jumps < 16; {
		if off >= len(msg) {
			return "", 0, errors.New("truncated name")
		}
		switch l := int(msg[off]); {
		case l == 0:
			if end < 0 {
				end = off + 1
			}
			return strings.Join(labels, ".") + ".", end, nil
		case l&0xc0 == 0xc0:
			if off+1 >= len(msg) {
				return "", 0, errors.New("truncated pointer")
			}
			if end < 0 {
				end = off + 2
			}
			off = int(binary.BigEndian.Uint16(msg[off:]) & 0x3fff)
			jumps++
		default:
			if off+1+l > len(msg) {
				return "", 0, errors.New("truncated label")
			}
			labels = append(labels, string(msg[off+1:off+1+l]))
			off += 1 + l
		}
	}
	return "", 0, errors.New("too many compression pointers")
}
