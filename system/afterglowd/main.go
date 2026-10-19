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
	"path/filepath"
	"strings"
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
	s := &server{tokenFile: *tokenFile, tools: append(tools(), worldTools()...)}
	go s.showStatus("/run/afterglow/status/30-claude")
	mux := http.NewServeMux()
	mux.Handle("/mcp", s)
	srv := &http.Server{Addr: *listen, Handler: mux, ReadHeaderTimeout: 10 * time.Second}
	klog("version %s serving MCP on %s/mcp", version, *listen)
	if err := srv.ListenAndServe(); err != nil {
		klog("%v", err)
		os.Exit(1)
	}
}

// showStatus keeps afterglowd's line on the headset's status screen (system/init/status.c) current:
// whether a computer is paired, and which client last connected. Pairing only writes the token
// file, so it's checked every couple of seconds.
func (s *server) showStatus(path string) {
	shown := ""
	for {
		text := "not paired"
		if data, err := os.ReadFile(s.tokenFile); err == nil && strings.TrimSpace(string(data)) != "" {
			text = "paired · afterglow.local"
		}
		if client, _ := s.client.Load().(string); client != "" {
			text = client + " connected"
		}
		// Written aside and renamed into place; the status screen skips names starting with a dot
		temp := filepath.Join(filepath.Dir(path), "."+filepath.Base(path))
		if text != shown && os.MkdirAll(filepath.Dir(path), 0o755) == nil &&
			os.WriteFile(temp, []byte("Claude\t"+text+"\n"), 0o644) == nil && os.Rename(temp, path) == nil {
			shown = text
		}
		time.Sleep(2 * time.Second)
	}
}
