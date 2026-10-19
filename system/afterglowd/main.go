// afterglowd is afterglow's MCP server. Claude Code (or any MCP client) on a computer connects to it
// over the network and works on the headset through its tools: run commands, read and write files,
// see what the displays show, read the logs.
//
// Every request needs the pairing token, which only reaches the headset over USB
// (tools/console.py --pair) and is kept in RAM. The headset also answers mDNS for afterglow.local,
// so the computer finds it whatever address DHCP gave it.
package main

import (
	"flag"
	"fmt"
	"net/http"
	"os"
	"time"
)

const version = "0.1.0"

var kmsg *os.File

// klog writes to the kernel log, where afterglow's USB console shows it, and to stderr.
func klog(format string, args ...any) {
	line := fmt.Sprintf(format, args...)
	fmt.Fprintln(os.Stderr, line)
	if kmsg != nil {
		fmt.Fprintf(kmsg, "<6>afterglowd: %s\n", line)
	}
}

func main() {
	listen := flag.String("listen", ":7070", "address to serve MCP on (path /mcp)")
	tokenFile := flag.String("token", "/run/afterglowd/token", "file holding the pairing token")
	name := flag.String("name", "afterglow", "host name to answer mDNS for (<name>.local); empty for none")
	flag.Parse()

	kmsg, _ = os.OpenFile("/dev/kmsg", os.O_WRONLY, 0)
	if *name != "" {
		go advertise(*name)
	}
	mux := http.NewServeMux()
	mux.Handle("/mcp", &server{tokenFile: *tokenFile, tools: tools()})
	srv := &http.Server{Addr: *listen, Handler: mux, ReadHeaderTimeout: 10 * time.Second}
	klog("version %s serving MCP on %s/mcp", version, *listen)
	if err := srv.ListenAndServe(); err != nil {
		klog("%v", err)
		os.Exit(1)
	}
}
