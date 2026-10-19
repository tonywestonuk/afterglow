package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unicode/utf8"
)

type tool struct {
	Name        string         `json:"name"`
	Description string         `json:"description"`
	InputSchema map[string]any `json:"inputSchema"`
	call        func(args json.RawMessage) ([]any, error)
}

// schema builds a JSON Schema object from "name": {type, description} pairs.
func schema(required []string, props map[string]any) map[string]any {
	s := map[string]any{"type": "object", "properties": props, "additionalProperties": false}
	if len(required) > 0 {
		s["required"] = required
	}
	return s
}

func prop(typ, description string) map[string]any {
	return map[string]any{"type": typ, "description": description}
}

func tools() []tool {
	return []tool{
		{
			Name: "run",
			Description: "Run a shell command on the headset (BusyBox sh, as root) and return its output and exit " +
				"status. Commands that start background processes must redirect their output " +
				"(e.g. `cmd > /tmp/cmd.log 2>&1 &`), or the call waits for them.",
			InputSchema: schema([]string{"command"}, map[string]any{
				"command":         prop("string", "Shell command line"),
				"timeout_seconds": prop("number", "Kill it after this long (default 30, at most 600)"),
				"cwd":             prop("string", "Working directory (default /)"),
			}),
			call: runCommand,
		},
		{
			Name: "read_file",
			Description: "Read a file on the headset. Text comes back as text; images (PNG, JPEG, GIF, WebP) as " +
				"images; other binary data as base64.",
			InputSchema: schema([]string{"path"}, map[string]any{
				"path":     prop("string", "Absolute path"),
				"offset":   prop("integer", "Byte offset to start at (default 0)"),
				"length":   prop("integer", "Bytes to read at most (default 1 MiB, at most 16 MiB)"),
				"encoding": map[string]any{"type": "string", "enum": []string{"auto", "text", "base64"}, "description": "Default auto"},
			}),
			call: readFile,
		},
		{
			Name: "write_file",
			Description: "Write (or append to) a file on the headset, creating parent directories. Only " +
				"regular files: devices are refused. The headset's partitions are mounted read-only, so " +
				"files land in RAM and are lost at restart.",
			InputSchema: schema([]string{"path", "content"}, map[string]any{
				"path":     prop("string", "Absolute path"),
				"content":  prop("string", "File content: text, or base64 if encoding is base64"),
				"encoding": map[string]any{"type": "string", "enum": []string{"text", "base64"}, "description": "Default text"},
				"mode":     prop("string", "Permissions for a new file, octal (default 0644; 0755 for a script)"),
				"append":   prop("boolean", "Append instead of replacing"),
			}),
			call: writeFile,
		},
		{
			Name:        "list_dir",
			Description: "List a directory on the headset: type and permissions, size, name (and link targets).",
			InputSchema: schema([]string{"path"}, map[string]any{"path": prop("string", "Absolute path")}),
			call:        listDir,
		},
		{
			Name: "status",
			Description: "The headset at a glance: uptime, load, memory, battery, temperature, the Snapdragon's " +
				"subsystems, network addresses and Wi-Fi.",
			InputSchema: schema(nil, map[string]any{}),
			call:        status,
		},
		{
			Name: "logs",
			Description: "Recent lines of the kernel log (which also has afterglow's and Android daemons' " +
				"messages), or of a log file such as /tmp/wpa_supplicant.log.",
			InputSchema: schema(nil, map[string]any{
				"lines": prop("integer", "How many of the most recent lines (default 100, at most 5000)"),
				"match": prop("string", "Only lines matching this regular expression (Go syntax)"),
				"file":  prop("string", "A log file to read instead of the kernel log"),
			}),
			call: logs,
		},
		{
			Name: "screenshot",
			Description: "What the headset's displays show, as the wearer sees them: both eyes side by side " +
				"(left eye on the left), or one eye. Each eye's image includes afterglow's correction for " +
				"its panel's slight roll, so level lines can look tilted by a degree or two here.",
			InputSchema: schema(nil, map[string]any{
				"eye":       map[string]any{"type": "string", "enum": []string{"both", "left", "right"}, "description": "Default both"},
				"downscale": prop("integer", "Shrink by this factor, 1 to 4 (default 2: both eyes at 1440x800)"),
			}),
			call: screenshot,
		},
	}
}

// decode reads a tool's arguments, rejecting unknown ones so mistakes don't pass silently.
func decode(args json.RawMessage, v any) error {
	d := json.NewDecoder(bytes.NewReader(args))
	d.DisallowUnknownFields()
	if err := d.Decode(v); err != nil {
		return fmt.Errorf("invalid arguments: %w", err)
	}
	return nil
}

func clip(s string, n int) string {
	if len(s) > n {
		return s[:n] + "…"
	}
	return s
}

// capped keeps the start and the end of a command's output, up to capHalf bytes each.
type capped struct {
	head, tail []byte
	total      int
}

const capHalf = 48 << 10

func (c *capped) Write(p []byte) (int, error) {
	n := len(p)
	c.total += n
	if room := capHalf - len(c.head); room > 0 {
		k := min(room, len(p))
		c.head = append(c.head, p[:k]...)
		p = p[k:]
	}
	c.tail = append(c.tail, p...)
	if len(c.tail) > 2*capHalf {
		c.tail = append([]byte(nil), c.tail[len(c.tail)-capHalf:]...)
	}
	return n, nil
}

func (c *capped) String() string {
	tail := c.tail
	if len(tail) > capHalf {
		tail = tail[len(tail)-capHalf:]
	}
	s := string(c.head)
	if omitted := c.total - len(c.head) - len(tail); omitted > 0 {
		s += fmt.Sprintf("\n[… %d bytes omitted …]\n", omitted)
	}
	return strings.ToValidUTF8(s+string(tail), "�")
}

func runCommand(args json.RawMessage) ([]any, error) {
	var a struct {
		Command string  `json:"command"`
		Timeout float64 `json:"timeout_seconds"`
		Cwd     string  `json:"cwd"`
	}
	if err := decode(args, &a); err != nil {
		return nil, err
	}
	if strings.TrimSpace(a.Command) == "" {
		return nil, errors.New("command is empty")
	}
	timeout := 30 * time.Second
	if a.Timeout > 0 {
		timeout = time.Duration(min(a.Timeout, 600) * float64(time.Second))
	}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, "/bin/sh", "-c", a.Command)
	cmd.Dir = "/"
	if a.Cwd != "" {
		cmd.Dir = a.Cwd
	}
	cmd.Env = []string{"PATH=/sbin:/bin:/usr/sbin:/usr/bin", "HOME=/", "TERM=dumb"}
	// Its own process group, so a timeout kills everything it started
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	cmd.Cancel = func() error { return syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL) }
	cmd.WaitDelay = 2 * time.Second // don't wait on background processes still holding its output
	var stdout, stderr capped
	cmd.Stdout, cmd.Stderr = &stdout, &stderr
	klog("run: %s", clip(a.Command, 200))
	start := time.Now()
	err := cmd.Run()
	elapsed := time.Since(start).Seconds()

	var exit string
	var exitErr *exec.ExitError
	switch {
	case ctx.Err() == context.DeadlineExceeded:
		exit = fmt.Sprintf("timed out after %.0f s and was killed", timeout.Seconds())
	case errors.Is(err, exec.ErrWaitDelay):
		exit = fmt.Sprintf("exit status %d (%.2f s); a background process kept its output open, so later "+
			"output is missing: redirect background processes' output", cmd.ProcessState.ExitCode(), elapsed)
	case errors.As(err, &exitErr) || err == nil:
		exit = fmt.Sprintf("exit status %d (%.2f s)", cmd.ProcessState.ExitCode(), elapsed)
	default:
		return nil, err
	}
	var out strings.Builder
	out.WriteString(stdout.String())
	if stderr.total > 0 {
		if stdout.total > 0 && !strings.HasSuffix(out.String(), "\n") {
			out.WriteString("\n")
		}
		out.WriteString("--- stderr ---\n" + stderr.String())
	}
	if out.Len() > 0 && !strings.HasSuffix(out.String(), "\n") {
		out.WriteString("\n")
	}
	out.WriteString("--- " + exit)
	return []any{text(out.String())}, nil
}

func readFile(args json.RawMessage) ([]any, error) {
	var a struct {
		Path     string `json:"path"`
		Offset   int64  `json:"offset"`
		Length   int64  `json:"length"`
		Encoding string `json:"encoding"`
	}
	if err := decode(args, &a); err != nil {
		return nil, err
	}
	length := int64(1 << 20)
	if a.Length > 0 {
		length = min(a.Length, 16<<20)
	}
	f, err := os.Open(a.Path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return nil, err
	}
	if info.IsDir() {
		return nil, fmt.Errorf("%s is a directory: use list_dir", a.Path)
	}
	if !info.Mode().IsRegular() {
		// Device files can block forever or never end
		return nil, fmt.Errorf("%s is not a regular file (%s): use run, e.g. with dd", a.Path, info.Mode().Type())
	}
	if a.Offset > 0 {
		if _, err := f.Seek(a.Offset, io.SeekStart); err != nil {
			return nil, err
		}
	}
	data, err := io.ReadAll(io.LimitReader(f, length+1))
	if err != nil {
		return nil, err
	}
	note := ""
	if int64(len(data)) > length {
		data = data[:length]
		note = fmt.Sprintf("\n[… stopped after %d bytes of %d; read on with offset %d]", length, info.Size(), a.Offset+length)
	}
	if a.Encoding == "" || a.Encoding == "auto" {
		switch mime := http.DetectContentType(data); mime {
		case "image/png", "image/jpeg", "image/gif", "image/webp":
			if note == "" {
				return []any{map[string]any{"type": "image", "data": base64.StdEncoding.EncodeToString(data), "mimeType": mime}}, nil
			}
		}
		if !utf8.Valid(data) {
			a.Encoding = "base64"
		}
	}
	if a.Encoding == "base64" {
		return []any{text(fmt.Sprintf("%d bytes from offset %d, base64:\n%s%s", len(data), a.Offset,
			base64.StdEncoding.EncodeToString(data), note))}, nil
	}
	return []any{text(strings.ToValidUTF8(string(data), "�") + note)}, nil
}

func writeFile(args json.RawMessage) ([]any, error) {
	var a struct {
		Path     string `json:"path"`
		Content  string `json:"content"`
		Encoding string `json:"encoding"`
		Mode     string `json:"mode"`
		Append   bool   `json:"append"`
	}
	if err := decode(args, &a); err != nil {
		return nil, err
	}
	if !filepath.IsAbs(a.Path) {
		return nil, errors.New("path must be absolute")
	}
	data := []byte(a.Content)
	if a.Encoding == "base64" {
		var err error
		if data, err = base64.StdEncoding.DecodeString(a.Content); err != nil {
			return nil, fmt.Errorf("content isn't valid base64: %w", err)
		}
	}
	mode := fs.FileMode(0o644)
	if a.Mode != "" {
		m, err := strconv.ParseUint(a.Mode, 8, 32)
		if err != nil || m > 0o7777 {
			return nil, fmt.Errorf("mode %q isn't octal permissions like 0644", a.Mode)
		}
		mode = fs.FileMode(m)
	}
	// Nothing under /dev, even through links, and nothing that isn't a regular file: writing a disk
	// by accident must be impossible here. (/dev only holds the nodes afterglow makes, so a path
	// there that doesn't exist yet would otherwise become a file.)
	target, err := resolve(a.Path)
	if err != nil {
		return nil, fmt.Errorf("%v: refused", err)
	}
	if target == "/dev" || strings.HasPrefix(target, "/dev/") {
		return nil, fmt.Errorf("%s is under /dev: refused (write devices deliberately, with run)", a.Path)
	}
	if info, err := os.Stat(target); err == nil && !info.Mode().IsRegular() {
		return nil, fmt.Errorf("%s is not a regular file (%s): refused", a.Path, info.Mode().Type())
	}
	if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
		return nil, err
	}
	flags := os.O_WRONLY | os.O_CREATE | os.O_TRUNC
	if a.Append {
		flags = os.O_WRONLY | os.O_CREATE | os.O_APPEND
	}
	f, err := os.OpenFile(target, flags, mode)
	if err != nil {
		return nil, err
	}
	_, err = f.Write(data)
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err == nil && a.Mode != "" {
		err = os.Chmod(target, mode)
	}
	if err != nil {
		return nil, err
	}
	klog("wrote %s (%d bytes)", a.Path, len(data))
	return []any{text(fmt.Sprintf("wrote %d bytes to %s", len(data), a.Path))}, nil
}

// resolve follows every link in the part of path that exists, so a check on the result can't be
// sidestepped by a link (to /dev, say) earlier in the path.
func resolve(path string) (string, error) {
	path = filepath.Clean(path)
	rest := ""
	for p := path; ; p = filepath.Dir(p) {
		if r, err := filepath.EvalSymlinks(p); err == nil {
			return filepath.Join(r, rest), nil
		}
		if info, err := os.Lstat(p); err == nil && info.Mode()&fs.ModeSymlink != 0 {
			return "", fmt.Errorf("%s is a link to nothing", p)
		}
		if p == "/" {
			return path, nil
		}
		rest = filepath.Join(filepath.Base(p), rest)
	}
}

func listDir(args json.RawMessage) ([]any, error) {
	var a struct {
		Path string `json:"path"`
	}
	if err := decode(args, &a); err != nil {
		return nil, err
	}
	entries, err := os.ReadDir(a.Path)
	if err != nil {
		return nil, err
	}
	var out strings.Builder
	for i, e := range entries {
		if i == 2000 {
			fmt.Fprintf(&out, "[… %d more]\n", len(entries)-i)
			break
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		fmt.Fprintf(&out, "%s %10d  %s", info.Mode(), info.Size(), e.Name())
		if info.Mode()&fs.ModeSymlink != 0 {
			if target, err := os.Readlink(filepath.Join(a.Path, e.Name())); err == nil {
				out.WriteString(" -> " + target)
			}
		}
		out.WriteString("\n")
	}
	if out.Len() == 0 {
		out.WriteString("(empty)")
	}
	return []any{text(out.String())}, nil
}

func readTrim(path string) string {
	data, _ := os.ReadFile(path)
	return strings.TrimSpace(string(data))
}

func status(args json.RawMessage) ([]any, error) {
	var out strings.Builder
	line := func(label, format string, v ...any) {
		fmt.Fprintf(&out, "%-12s "+format+"\n", append([]any{label + ":"}, v...)...)
	}

	up, _ := strconv.ParseFloat(strings.Fields(readTrim("/proc/uptime") + " 0")[0], 64)
	line("afterglow", "kernel %s, afterglowd %s, up %s", readTrim("/proc/sys/kernel/osrelease"), version,
		(time.Duration(up) * time.Second).String())
	load := strings.Fields(readTrim("/proc/loadavg"))
	if len(load) >= 3 {
		line("load", "%s %s %s", load[0], load[1], load[2])
	}
	mem := map[string]int{}
	for _, l := range strings.Split(readTrim("/proc/meminfo"), "\n") {
		var k string
		var v int
		if n, _ := fmt.Sscanf(l, "%s %d", &k, &v); n == 2 {
			mem[strings.TrimSuffix(k, ":")] = v
		}
	}
	line("memory", "%d MB available of %d MB", mem["MemAvailable"]/1024, mem["MemTotal"]/1024)
	if c := readTrim("/sys/class/power_supply/battery/capacity"); c != "" {
		t, _ := strconv.Atoi(readTrim("/sys/class/power_supply/battery/temp"))
		line("battery", "%s%%, %s, %.1f °C", c, readTrim("/sys/class/power_supply/battery/status"), float64(t)/10)
	}
	// The SoC's own sensors report tenths of a degree
	hottest := 0
	zones, _ := filepath.Glob("/sys/class/thermal/thermal_zone*")
	for _, z := range zones {
		if strings.HasPrefix(readTrim(z+"/type"), "tsens_tz_sensor") {
			if t, err := strconv.Atoi(readTrim(z + "/temp")); err == nil && t > hottest {
				hottest = t
			}
		}
	}
	if hottest > 0 {
		line("SoC", "%.1f °C (hottest sensor)", float64(hottest)/10)
	}
	var subsys []string
	devs, _ := filepath.Glob("/sys/bus/msm_subsys/devices/*")
	for _, d := range devs {
		subsys = append(subsys, readTrim(d+"/name")+" "+strings.ToLower(readTrim(d+"/state")))
	}
	line("subsystems", "%s", strings.Join(subsys, ", "))
	ifaces, _ := net.Interfaces()
	for _, ifi := range ifaces {
		if ifi.Flags&net.FlagUp == 0 || ifi.Flags&net.FlagLoopback != 0 {
			continue
		}
		var addrs []string
		as, _ := ifi.Addrs()
		for _, a := range as {
			addrs = append(addrs, a.String())
		}
		if len(addrs) > 0 {
			line("network", "%s %s", ifi.Name, strings.Join(addrs, " "))
		}
	}
	if _, err := os.Stat("/var/run/wpa_supplicant/wlan0"); err == nil {
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		defer cancel()
		st, _ := exec.CommandContext(ctx, "wpa_cli", "-i", "wlan0", "status").Output()
		sig, _ := exec.CommandContext(ctx, "wpa_cli", "-i", "wlan0", "signal_poll").Output()
		kv := map[string]string{}
		for _, l := range strings.Split(string(st)+string(sig), "\n") {
			if k, v, ok := strings.Cut(l, "="); ok {
				kv[k] = v
			}
		}
		if kv["wpa_state"] == "COMPLETED" {
			line("wifi", "connected, %s MHz, %s dBm, %s Mbit/s", kv["freq"], kv["RSSI"], kv["LINKSPEED"])
		} else {
			line("wifi", "%s (join a network: tools/console.py --wifi)", strings.ToLower(kv["wpa_state"]))
		}
	}
	procs, _ := filepath.Glob("/proc/[0-9]*")
	line("processes", "%d", len(procs))
	return []any{text(out.String())}, nil
}

var kmsgRecord = regexp.MustCompile(`^(\d+),\d+,(\d+),[^;]*;(.*)$`)

func logs(args json.RawMessage) ([]any, error) {
	var a struct {
		Lines int    `json:"lines"`
		Match string `json:"match"`
		File  string `json:"file"`
	}
	if err := decode(args, &a); err != nil {
		return nil, err
	}
	n := 100
	if a.Lines > 0 {
		n = min(a.Lines, 5000)
	}
	var match *regexp.Regexp
	if a.Match != "" {
		var err error
		if match, err = regexp.Compile(a.Match); err != nil {
			return nil, err
		}
	}
	var lines []string
	keep := func(l string) {
		if match == nil || match.MatchString(l) {
			lines = append(lines, l)
			if len(lines) > 2*n {
				lines = append([]string(nil), lines[len(lines)-n:]...)
			}
		}
	}
	if a.File != "" {
		data, err := os.ReadFile(a.File)
		if err != nil {
			return nil, err
		}
		for _, l := range strings.Split(strings.TrimRight(string(data), "\n"), "\n") {
			keep(l)
		}
	} else {
		// One record per read; raw syscalls, as Go's poller would make a non-blocking read wait
		fd, err := syscall.Open("/dev/kmsg", syscall.O_RDONLY|syscall.O_NONBLOCK|syscall.O_CLOEXEC, 0)
		if err != nil {
			return nil, err
		}
		defer syscall.Close(fd)
		buf := make([]byte, 8192)
		for {
			k, err := syscall.Read(fd, buf)
			if err == syscall.EPIPE { // overwritten while reading: carry on with the next record
				continue
			}
			if err != nil || k <= 0 {
				break
			}
			first, _, _ := strings.Cut(string(buf[:k]), "\n")
			if m := kmsgRecord.FindStringSubmatch(first); m != nil {
				us, _ := strconv.ParseInt(m[2], 10, 64)
				keep(fmt.Sprintf("[%5d.%06d] %s", us/1e6, us%1e6, m[3]))
			}
		}
	}
	if len(lines) > n {
		lines = lines[len(lines)-n:]
	}
	if len(lines) == 0 {
		return []any{text("(no matching lines)")}, nil
	}
	return []any{text(strings.ToValidUTF8(strings.Join(lines, "\n"), "�"))}, nil
}
