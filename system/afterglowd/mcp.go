package main

// MCP over Streamable HTTP, the subset a tools-only server needs: JSON-RPC requests POSTed to /mcp,
// each answered with one JSON response. No server-initiated messages, so no event stream, and no
// sessions.

import (
	"crypto/subtle"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"slices"
	"strings"
)

// Protocol versions this server can speak, newest first. All of them are the same for a server
// that only offers tools.
var protocolVersions = []string{"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"}

const maxRequest = 64 << 20 // a write_file of up to ~48 MB, base64-encoded

const instructions = `This is a Meta Quest 1 headset running afterglow, an open OS: Meta's Linux 4.4 kernel with
afterglow's own userspace (BusyBox, no Android). You are root on it.

- Storage: /storage, if present, is afterglow's own and is kept across restarts (ext4 on the spare
  slot's system partition). Everything else outside mounted partitions is RAM and is lost at
  restart. The headset's own partitions are mounted read-only. Never write to a partition or block
  device: persist, private, vision, devinfo, modemst1/2, fsg and fsc hold irreplaceable per-device
  data. Ask the person before restarting the headset; they may be wearing it.
- Displays: two panels, one 2880x1600 framebuffer (/dev/fb0). The screenshot tool shows them as
  the wearer sees them.
- Hardware bring-up: sh /afterglow/bringup.sh (no arguments lists the steps). Wi-Fi:
  sh /afterglow/wifi.sh. Logs of daemons started by bringup.sh: /tmp/<name>.log.
- Android's programs and firmware are on the read-only /system, /vendor and /vendor/firmware_mnt.
- Source and docs: https://github.com/tonywestonuk/afterglow`

type request struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params"`
}

type rpcError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

type response struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id"`
	Result  any             `json:"result,omitempty"`
	Error   *rpcError       `json:"error,omitempty"`
}

type server struct {
	tokenFile string
	tools     []tool
}

func (s *server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	// Browsers send Origin; MCP clients don't. Refusing it stops web pages (including via DNS
	// rebinding) from reaching the headset through someone's browser.
	if r.Header.Get("Origin") != "" {
		http.Error(w, "requests from web pages are not accepted", http.StatusForbidden)
		return
	}
	if status, msg := s.authorize(r); status != http.StatusOK {
		http.Error(w, msg, status)
		return
	}
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		http.Error(w, "POST JSON-RPC messages here; this server sends no events", http.StatusMethodNotAllowed)
		return
	}
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxRequest))
	if err != nil {
		http.Error(w, err.Error(), http.StatusRequestEntityTooLarge)
		return
	}
	var req request
	if err := json.Unmarshal(body, &req); err != nil || req.JSONRPC != "2.0" || req.Method == "" {
		reply(w, response{JSONRPC: "2.0", ID: json.RawMessage("null"),
			Error: &rpcError{-32700, "expected one JSON-RPC 2.0 message"}})
		return
	}
	if len(req.ID) == 0 { // a notification (initialized, cancelled): nothing to answer
		w.WriteHeader(http.StatusAccepted)
		return
	}
	result, rerr := s.handle(req)
	reply(w, response{JSONRPC: "2.0", ID: req.ID, Result: result, Error: rerr})
}

// authorize checks the bearer token against the one tools/console.py --pair put on the headset.
func (s *server) authorize(r *http.Request) (int, string) {
	data, err := os.ReadFile(s.tokenFile)
	token := strings.TrimSpace(string(data))
	if err != nil || token == "" {
		return http.StatusServiceUnavailable, "not paired: run tools/console.py --pair on the computer connected over USB"
	}
	given, ok := strings.CutPrefix(r.Header.Get("Authorization"), "Bearer ")
	if !ok || subtle.ConstantTimeCompare([]byte(given), []byte(token)) != 1 {
		return http.StatusUnauthorized, "wrong or missing token"
	}
	return http.StatusOK, ""
}

func reply(w http.ResponseWriter, resp response) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

func (s *server) handle(req request) (any, *rpcError) {
	switch req.Method {
	case "initialize":
		var p struct {
			ProtocolVersion string `json:"protocolVersion"`
			ClientInfo      struct {
				Name    string `json:"name"`
				Version string `json:"version"`
			} `json:"clientInfo"`
		}
		json.Unmarshal(req.Params, &p)
		agreed := protocolVersions[0]
		if slices.Contains(protocolVersions, p.ProtocolVersion) {
			agreed = p.ProtocolVersion
		}
		klog("client connected: %s %s (MCP %s)", p.ClientInfo.Name, p.ClientInfo.Version, agreed)
		return map[string]any{
			"protocolVersion": agreed,
			"capabilities":    map[string]any{"tools": map[string]any{}},
			"serverInfo":      map[string]any{"name": "afterglowd", "version": version},
			"instructions":    instructions,
		}, nil
	case "ping":
		return map[string]any{}, nil
	case "tools/list":
		return map[string]any{"tools": s.tools}, nil
	case "tools/call":
		var p struct {
			Name      string          `json:"name"`
			Arguments json.RawMessage `json:"arguments"`
		}
		if err := json.Unmarshal(req.Params, &p); err != nil {
			return nil, &rpcError{-32602, "invalid params"}
		}
		i := slices.IndexFunc(s.tools, func(t tool) bool { return t.Name == p.Name })
		if i < 0 {
			return nil, &rpcError{-32602, "unknown tool: " + p.Name}
		}
		if len(p.Arguments) == 0 {
			p.Arguments = json.RawMessage("{}")
		}
		content, err := s.tools[i].call(p.Arguments)
		if err != nil {
			// A failed tool is a result the model should see, not a protocol error
			klog("%s failed: %v", p.Name, err)
			return map[string]any{"content": []any{text(err.Error())}, "isError": true}, nil
		}
		return map[string]any{"content": content}, nil
	}
	return nil, &rpcError{-32601, "method not found: " + req.Method}
}

func text(s string) map[string]any {
	return map[string]any{"type": "text", "text": s}
}
