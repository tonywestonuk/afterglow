package main

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"image"
	"image/color"
	"image/png"
	"math"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const testToken = "test-token-123"

func newTestServer(t *testing.T) *server {
	t.Helper()
	path := filepath.Join(t.TempDir(), "token")
	if err := os.WriteFile(path, []byte(testToken+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	return &server{tokenFile: path, tools: append(append(tools(), worldTools()...), textureTools()...)}
}

// post sends one JSON-RPC message and returns the HTTP status and the decoded response.
func post(t *testing.T, s *server, body string, mutate func(*http.Request)) (int, map[string]any) {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, "/mcp", strings.NewReader(body))
	req.Header.Set("Authorization", "Bearer "+testToken)
	req.Header.Set("Content-Type", "application/json")
	if mutate != nil {
		mutate(req)
	}
	rec := httptest.NewRecorder()
	s.ServeHTTP(rec, req)
	var resp map[string]any
	json.Unmarshal(rec.Body.Bytes(), &resp)
	return rec.Code, resp
}

func call(t *testing.T, s *server, name string, args map[string]any) (string, bool) {
	t.Helper()
	params, _ := json.Marshal(map[string]any{"name": name, "arguments": args})
	code, resp := post(t, s, `{"jsonrpc":"2.0","id":7,"method":"tools/call","params":`+string(params)+`}`, nil)
	if code != http.StatusOK || resp["result"] == nil {
		t.Fatalf("%s: HTTP %d, %v", name, code, resp)
	}
	result := resp["result"].(map[string]any)
	isError, _ := result["isError"].(bool)
	var texts []string
	for _, item := range result["content"].([]any) {
		if s, ok := item.(map[string]any)["text"].(string); ok {
			texts = append(texts, s)
		}
	}
	return strings.Join(texts, "\n"), isError
}

func TestAuthorization(t *testing.T) {
	s := newTestServer(t)
	ping := `{"jsonrpc":"2.0","id":1,"method":"ping"}`
	if code, _ := post(t, s, ping, func(r *http.Request) { r.Header.Del("Authorization") }); code != http.StatusUnauthorized {
		t.Errorf("no token: HTTP %d, want 401", code)
	}
	if code, _ := post(t, s, ping, func(r *http.Request) { r.Header.Set("Authorization", "Bearer wrong") }); code != http.StatusUnauthorized {
		t.Errorf("wrong token: HTTP %d, want 401", code)
	}
	if code, _ := post(t, s, ping, func(r *http.Request) { r.Header.Set("Origin", "http://evil.example") }); code != http.StatusForbidden {
		t.Errorf("browser origin: HTTP %d, want 403", code)
	}
	if code, resp := post(t, s, ping, nil); code != http.StatusOK || resp["result"] == nil {
		t.Errorf("good token: HTTP %d, %v", code, resp)
	}
	unpaired := &server{tokenFile: filepath.Join(t.TempDir(), "missing"), tools: tools()}
	if code, _ := post(t, unpaired, ping, nil); code != http.StatusServiceUnavailable {
		t.Errorf("not paired: HTTP %d, want 503", code)
	}
}

func TestHandshake(t *testing.T) {
	s := newTestServer(t)
	_, resp := post(t, s, `{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18",
		"capabilities":{},"clientInfo":{"name":"test","version":"1"}}}`, nil)
	result := resp["result"].(map[string]any)
	if result["protocolVersion"] != "2025-06-18" {
		t.Errorf("protocol version %v, want the client's 2025-06-18", result["protocolVersion"])
	}
	_, resp = post(t, s, `{"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"2099-01-01"}}`, nil)
	if v := resp["result"].(map[string]any)["protocolVersion"]; v != protocolVersions[0] {
		t.Errorf("unknown client version: got %v, want our newest %s", v, protocolVersions[0])
	}
	if code, _ := post(t, s, `{"jsonrpc":"2.0","method":"notifications/initialized"}`, nil); code != http.StatusAccepted {
		t.Errorf("notification: HTTP %d, want 202", code)
	}
	_, resp = post(t, s, `{"jsonrpc":"2.0","id":3,"method":"tools/list"}`, nil)
	var names []string
	for _, tl := range resp["result"].(map[string]any)["tools"].([]any) {
		names = append(names, tl.(map[string]any)["name"].(string))
	}
	if got := strings.Join(names, " "); got != "run read_file write_file list_dir status logs screenshot "+
		"load_extension unload_extension list_extensions start_world stop_world generate_texture fetch_texture" {
		t.Errorf("tools: %s", got)
	}
	_, resp = post(t, s, `{"jsonrpc":"2.0","id":4,"method":"no/such"}`, nil)
	if resp["error"] == nil {
		t.Errorf("unknown method: no error in %v", resp)
	}
}

func TestRun(t *testing.T) {
	s := newTestServer(t)
	out, isErr := call(t, s, "run", map[string]any{"command": "echo hello; echo oops >&2; exit 3"})
	if isErr || !strings.Contains(out, "hello\n--- stderr ---\noops\n--- exit status 3") {
		t.Errorf("run output:\n%s", out)
	}
	out, _ = call(t, s, "run", map[string]any{"command": "sleep 5", "timeout_seconds": 0.5})
	if !strings.Contains(out, "timed out") {
		t.Errorf("timeout:\n%s", out)
	}
	out, _ = call(t, s, "run", map[string]any{"command": "sleep 30 & echo started"})
	if !strings.Contains(out, "started") || !strings.Contains(out, "background process") {
		t.Errorf("background process holding output:\n%s", out)
	}
	if _, isErr := call(t, s, "run", map[string]any{"command": "true", "bogus": 1}); !isErr {
		t.Errorf("unknown argument accepted")
	}
}

func TestFiles(t *testing.T) {
	s := newTestServer(t)
	path := filepath.Join(t.TempDir(), "sub", "hello.sh")
	if out, isErr := call(t, s, "write_file", map[string]any{"path": path, "content": "echo hi\n", "mode": "0755"}); isErr {
		t.Fatalf("write_file: %s", out)
	}
	if info, _ := os.Stat(path); info == nil || info.Mode().Perm() != 0o755 {
		t.Errorf("mode not applied: %v", info)
	}
	call(t, s, "write_file", map[string]any{"path": path, "content": "echo there\n", "append": true})
	if out, _ := call(t, s, "read_file", map[string]any{"path": path}); out != "echo hi\necho there\n" {
		t.Errorf("read_file: %q", out)
	}
	if out, _ := call(t, s, "read_file", map[string]any{"path": path, "offset": 5, "length": 2}); !strings.HasPrefix(out, "hi") {
		t.Errorf("read_file offset/length: %q", out)
	}
	// Nothing under /dev, however it's reached; tested only with paths that can't touch a real disk
	dir := t.TempDir()
	os.Symlink("/dev", filepath.Join(dir, "devlink"))
	os.Symlink("/dev/null", filepath.Join(dir, "nulllink"))
	os.Symlink("/dev/afterglow-test-missing", filepath.Join(dir, "dangling"))
	os.WriteFile(filepath.Join(dir, "plain"), nil, 0o644)
	os.Symlink(filepath.Join(dir, "plain"), filepath.Join(dir, "plainlink"))
	for _, p := range []string{"/dev/null", "/dev/afterglow-test-missing", "/dev/../dev/afterglow-test-missing",
		filepath.Join(dir, "devlink", "afterglow-test-missing"), filepath.Join(dir, "devlink", "new", "dir", "file"),
		filepath.Join(dir, "nulllink"), filepath.Join(dir, "dangling")} {
		if out, isErr := call(t, s, "write_file", map[string]any{"path": p, "content": "x"}); !isErr {
			t.Errorf("write to %s was allowed: %s", p, out)
		}
	}
	if _, err := os.Lstat("/dev/afterglow-test-missing"); err == nil {
		t.Errorf("a file was created under /dev")
	}
	if out, isErr := call(t, s, "write_file", map[string]any{"path": filepath.Join(dir, "plainlink"), "content": "ok"}); isErr {
		t.Errorf("write through a link to a regular file was refused: %s", out)
	}
	if out, _ := call(t, s, "list_dir", map[string]any{"path": filepath.Dir(path)}); !strings.Contains(out, "-rwxr-xr-x") ||
		!strings.Contains(out, "hello.sh") {
		t.Errorf("list_dir: %s", out)
	}
}

func TestMDNS(t *testing.T) {
	ip := net.IPv4(192, 168, 1, 50)
	query := append([]byte{0x12, 0x34, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0}, encodeName("Afterglow.local")...)
	query = append(query, 0, 1, 0x80, 1) // type A, class IN with the unicast-response bit
	resp, unicast := mdnsAnswer(query, "afterglow.local.", ip, false)
	if resp == nil || !unicast {
		t.Fatalf("no answer, or not unicast as asked")
	}
	if resp[0] != 0 || resp[1] != 0 || !bytes.Contains(resp, append([]byte{0, 4}, ip.To4()...)) {
		t.Errorf("multicast-style answer: % x", resp)
	}
	if resp[11] != 1 || !bytes.Contains(resp, []byte{0, typeNSEC, 0x80, 1}) {
		t.Errorf("an A answer should carry an NSEC saying there's no AAAA: % x", resp)
	}
	resp, _ = mdnsAnswer(query, "afterglow.local.", ip, true)
	if resp[0] != 0x12 || resp[1] != 0x34 || resp[5] != 1 || resp[7] != 1 || resp[11] != 0 {
		t.Errorf("legacy answer should echo the ID and the question, without NSEC: % x", resp)
	}
	aaaa := append([]byte{0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0}, encodeName("afterglow.local")...)
	resp, _ = mdnsAnswer(append(aaaa, 0, typeAAAA, 0, 1), "afterglow.local.", ip, false)
	nameLen := len(encodeName("afterglow.local"))
	if resp == nil || resp[7] != 1 || resp[12+nameLen+1] != typeNSEC || !net.IP(resp[len(resp)-4:]).Equal(ip) {
		t.Errorf("an AAAA query should get NSEC as the answer and A as extra: % x", resp)
	}
	other := append([]byte{0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0}, encodeName("printer.local")...)
	if resp, _ := mdnsAnswer(append(other, 0, 1, 0, 1), "afterglow.local.", ip, false); resp != nil {
		t.Errorf("answered for another host")
	}
}

func TestExtensionArguments(t *testing.T) {
	s := newTestServer(t)
	for _, args := range []map[string]any{
		{"name": "Grass", "source": "x"},               // upper case
		{"name": "../etc", "source": "x"},              // a path
		{"name": "grass"},                              // neither source nor version
		{"name": "grass", "source": "x", "version": 2}, // both
		{"name": "grass", "source": "x", "edits": []any{map[string]any{"old": "a", "new": "b"}}},
		{"name": "nothing-to-edit", "edits": []any{map[string]any{"old": "a", "new": "b"}}},
	} {
		if out, isErr := call(t, s, "load_extension", args); !isErr {
			t.Errorf("load_extension %v accepted: %s", args, out)
		}
	}
}

func TestEdits(t *testing.T) {
	source := "int a = 1;\nint b = 2;\nint c = 1;\n"
	got, err := applyEdits(source, []edit{{Old: "b = 2", New: "b = 3"}, {Old: "int c", New: "long c"}})
	if err != nil || got != "int a = 1;\nint b = 3;\nlong c = 1;\n" {
		t.Errorf("applyEdits: %q, %v", got, err)
	}
	for _, bad := range [][]edit{{{Old: "= 1", New: "x"}}, {{Old: "missing", New: "x"}}, {{Old: "", New: "x"}}} {
		if _, err := applyEdits(source, bad); err == nil {
			t.Errorf("applyEdits %v: no error", bad)
		}
	}
}

// stripes makes a test image: smooth vertical bands, which don't line up at the left and right edges.
func stripes(w, h int) *image.NRGBA {
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			v := uint8(128 + 100*math.Sin(float64(x)/float64(w)*5))
			img.Set(x, y, color.NRGBA{v, v, 200, 255})
		}
	}
	return img
}

func TestSeamless(t *testing.T) {
	img := stripes(200, 100)
	if tilesAlready(img, true) || !tilesAlready(img, false) {
		t.Fatal("tilesAlready: wrong about the test image")
	}
	if !blendEdges(img) || !tilesAlready(img, true) {
		t.Error("blendEdges didn't make it tile across")
	}
	if blendEdges(img) {
		t.Error("blendEdges changed an image that already tiles")
	}
}

func TestTextures(t *testing.T) {
	dir := t.TempDir()
	defer func(f func() string, g string) { worldDir, imageGeneratorFile = f, g }(worldDir, imageGeneratorFile)
	worldDir = func() string { return dir }
	imageGeneratorFile = filepath.Join(dir, "image-generator")
	var picture bytes.Buffer
	png.Encode(&picture, stripes(600, 300))

	// An image generator with AUTOMATIC1111's API, and a plain web server
	var asked map[string]any
	generator := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/sdapi/v1/txt2img" {
			http.NotFound(w, r)
			return
		}
		asked = nil
		json.NewDecoder(r.Body).Decode(&asked)
		if _, ok := asked["tiling"]; ok && strings.Contains(asked["prompt"].(string), "strict") { // as Draw Things does
			w.WriteHeader(http.StatusBadRequest)
			w.Write([]byte(`{"error":"HTTPException","errors":"","detail":"Unrecognized keys: [\"tiling\"]","body":""}`))
			return
		}
		json.NewEncoder(w).Encode(map[string]any{"images": []string{base64.StdEncoding.EncodeToString(picture.Bytes())}})
	}))
	defer generator.Close()
	web := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.Write(picture.Bytes()) }))
	defer web.Close()

	s := newTestServer(t)
	if out, isErr := call(t, s, "generate_texture", map[string]any{"name": "lawn", "prompt": "grass"}); !isErr {
		t.Errorf("generate_texture with no server known: %s", out)
	}
	if out, isErr := call(t, s, "generate_texture", map[string]any{"name": "lawn", "prompt": "grass", "seamless": true,
		"server": generator.URL}); isErr || !strings.Contains(out, "edges blended") {
		t.Errorf("generate_texture: %s", out)
	}
	if asked["prompt"] != "grass" || asked["tiling"] != true || asked["width"] != nil {
		t.Errorf("the generator was asked for %v", asked)
	}
	if out, isErr := call(t, s, "generate_texture", map[string]any{"name": "strict", "prompt": "strict grass", "seamless": true,
		"size": 768}); isErr || !strings.Contains(out, "doesn't take tiling") || !strings.Contains(out, "edges blended") {
		t.Errorf("generate_texture from a strict generator: %s", out)
	}
	if asked["width"] != 768.0 || asked["tiling"] != nil {
		t.Errorf("the strict generator was last asked for %v", asked)
	}
	if out, isErr := call(t, s, "generate_texture", map[string]any{"name": "moss", "prompt": "moss"}); isErr {
		t.Errorf("generate_texture with the server remembered: %s", out)
	}
	if out, isErr := call(t, s, "fetch_texture", map[string]any{"name": "bands", "url": web.URL + "/bands.png"}); isErr ||
		!strings.Contains(out, "600x300") {
		t.Errorf("fetch_texture: %s", out)
	}
	if out, isErr := call(t, s, "fetch_texture", map[string]any{"name": "nope", "url": "file:///etc/passwd"}); !isErr {
		t.Errorf("fetch_texture of a file: URL accepted: %s", out)
	}
	if got := listTextures(); got != "textures: bands (600x300), lawn (600x300), moss (600x300), strict (600x300)\n" {
		t.Errorf("listTextures: %q", got)
	}
}
