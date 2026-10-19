package main

// The world (system/world) and its extensions, for Claude: an extension is C source; afterglowd keeps
// every version on storage, compiles it to WebAssembly on the headset (wasm-cc.sh), and has the world
// load it while it runs. The world is started when first needed and comes back with the extensions it
// had.

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

var (
	worldBinary = "/afterglow/world"
	worldSocket = "/run/afterglow/world.sock"
	compiler    = "/afterglow/wasm-cc.sh"
	extNameRule = regexp.MustCompile(`^[a-z][a-z0-9_-]{0,31}$`)
	worldMu     sync.Mutex // one world command at a time
)

// worldDir is where extensions' versions, the list of loaded ones and textures live: on storage if
// there is some, so a world comes back as it was after a restart.
var worldDir = func() string {
	if _, err := os.Stat("/storage/lost+found"); err == nil {
		return "/storage/world"
	}
	return "/tmp/world"
}

func worldTools() []tool {
	return []tool{
		{
			Name: "load_extension",
			Description: "Add to the running 3D world, or replace part of it: compile a C extension (the API is in " +
				"/afterglow/sdk/afterglow.h and shapes.h on the headset; read them first) to WebAssembly and load it " +
				"while the world runs. A new version of an extension replaces the old one, which first undoes " +
				"everything it did. Compiler errors, shader errors and crashes come back here; the world keeps " +
				"running. Send source for a new extension; to change one, send edits instead (much quicker than " +
				"the whole source again). Every version is kept: pass version alone to go back to an earlier one. " +
				"placeholder: true puts a road-works sign beside what it makes, for a quick stand-in while you " +
				"write the real thing; loading the real one under the same name replaces both. Take a screenshot to see it.",
			InputSchema: schema([]string{"name"}, map[string]any{
				"name":   prop("string", "Extension name: lower case, digits, - or _ (e.g. grass)"),
				"source": prop("string", "The extension's whole C source"),
				"edits": map[string]any{
					"type": "array",
					"description": "Changes to the newest version's source (or to version's, if given), made in order: " +
						"each old must appear exactly once",
					"items": schema([]string{"old", "new"}, map[string]any{
						"old": prop("string", "Text in the source, copied exactly"),
						"new": prop("string", "What replaces it"),
					}),
				},
				"version":     prop("integer", "Load this earlier version (alone), or the version edits apply to"),
				"placeholder": prop("boolean", "A quick stand-in: the world puts a road-works sign beside it"),
			}),
			call: loadExtension,
		},
		{
			Name:        "unload_extension",
			Description: "Remove an extension from the world; everything it made or changed goes back to how it was.",
			InputSchema: schema([]string{"name"}, map[string]any{"name": prop("string", "Extension name")}),
			call:        unloadExtension,
		},
		{
			Name:        "list_extensions",
			Description: "The world's extensions: which are loaded and running (or why one failed), and the versions kept of each.",
			InputSchema: schema(nil, map[string]any{}),
			call:        listExtensions,
		},
		{
			Name:        "start_world",
			Description: "Start the 3D world (a wireframe floor, plus the extensions it had last time) if it isn't running.",
			InputSchema: schema(nil, map[string]any{}),
			call:        startWorld,
		},
		{
			Name: "recentre",
			Description: "Make the way the wearer faces now the world's ahead (-z). Head tracking is turning only (the IMU), " +
				"so the heading drifts slowly; it's recentred whenever the world starts.",
			InputSchema: schema(nil, map[string]any{}),
			call:        recentreWorld,
		},
		{
			Name:        "stop_world",
			Description: "Stop the 3D world and go back to the headset's status screen. Its extensions are loaded again next time.",
			InputSchema: schema(nil, map[string]any{}),
			call:        stopWorld,
		},
	}
}

// worldCommand sends one command to the world and returns its reply.
func worldCommand(command string, timeout time.Duration) (string, error) {
	conn, err := net.DialTimeout("unix", worldSocket, 2*time.Second)
	if err != nil {
		return "", err
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(timeout))
	if _, err := io.WriteString(conn, command+"\n"); err != nil {
		return "", err
	}
	reply, err := io.ReadAll(conn)
	return string(reply), err
}

// active is the list of loaded extensions and their versions, kept so the world comes back as it was.
func readActive() map[string]int {
	active := map[string]int{}
	data, _ := os.ReadFile(filepath.Join(worldDir(), "active.json"))
	json.Unmarshal(data, &active)
	return active
}

func writeActive(active map[string]int) {
	data, _ := json.Marshal(active)
	os.WriteFile(filepath.Join(worldDir(), "active.json"), data, 0o644)
}

func versionPath(name string, version int, ext string) string {
	return filepath.Join(worldDir(), "extensions", name, fmt.Sprintf("v%d.%s", version, ext))
}

// loadCommand is the world's command to load a version; placeholders are marked by a file beside it.
func loadCommand(name string, version int) string {
	command := fmt.Sprintf("load %s %s", name, versionPath(name, version, "wasm"))
	if _, err := os.Stat(versionPath(name, version, "placeholder")); err == nil {
		command += " placeholder"
	}
	return command
}

// newestVersion is the highest version of an extension kept, or 0.
func newestVersion(name string) int {
	newest := 0
	existing, _ := filepath.Glob(filepath.Join(worldDir(), "extensions", name, "v*.c"))
	for _, path := range existing {
		if n, err := strconv.Atoi(strings.TrimSuffix(strings.TrimPrefix(filepath.Base(path), "v"), ".c")); err == nil && n > newest {
			newest = n
		}
	}
	return newest
}

type edit struct {
	Old string `json:"old"`
	New string `json:"new"`
}

// applyEdits makes each edit in turn; each old text must appear exactly once.
func applyEdits(source string, edits []edit) (string, error) {
	for i, e := range edits {
		if e.Old == "" {
			return "", fmt.Errorf("edit %d: old is empty", i+1)
		}
		switch n := strings.Count(source, e.Old); n {
		case 1:
			source = strings.Replace(source, e.Old, e.New, 1)
		case 0:
			return "", fmt.Errorf("edit %d: old text isn't in the source (it must match exactly, spaces included)", i+1)
		default:
			return "", fmt.Errorf("edit %d: old text appears %d times; include more around it so it's unique", i+1, n)
		}
	}
	return source, nil
}

// ensureWorld starts the world if it isn't running, with the extensions it had. It returns a note
// for the reply when it had to.
func ensureWorld() (string, error) {
	if _, err := worldCommand("list", 5*time.Second); err == nil {
		return "", nil
	}
	cmd := exec.Command(worldBinary)
	cmd.Env = []string{"PATH=/sbin:/bin:/usr/sbin:/usr/bin",
		"LD_LIBRARY_PATH=/system/lib64/vndk-sp-29:/system/lib64/vndk-29:/vendor/lib64:/vendor/lib64/egl",
		"AFTERGLOW_WORLD_DIR=" + worldDir()}
	log, err := os.OpenFile("/tmp/world.log", os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o644)
	if err != nil {
		return "", err
	}
	defer log.Close()
	cmd.Stdout, cmd.Stderr = log, log
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	if err := cmd.Start(); err != nil {
		return "", fmt.Errorf("starting the world: %w", err)
	}
	go cmd.Wait()
	klog("world started (pid %d)", cmd.Process.Pid)
	deadline := time.Now().Add(15 * time.Second)
	for {
		if _, err := worldCommand("list", 5*time.Second); err == nil {
			break
		}
		if time.Now().After(deadline) {
			return "", errors.New("the world didn't start: see /tmp/world.log")
		}
		time.Sleep(200 * time.Millisecond)
	}
	note := "started the world"
	active := readActive()
	names := make([]string, 0, len(active))
	for name := range active {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		reply, _ := worldCommand(loadCommand(name, active[name]), 60*time.Second)
		note += fmt.Sprintf("; %s v%d: %s", name, active[name], strings.TrimSpace(reply))
	}
	return note, nil
}

func loadExtension(args json.RawMessage) ([]any, error) {
	var a struct {
		Name        string `json:"name"`
		Source      string `json:"source"`
		Edits       []edit `json:"edits"`
		Version     int    `json:"version"`
		Placeholder bool   `json:"placeholder"`
	}
	if err := decode(args, &a); err != nil {
		return nil, err
	}
	if !extNameRule.MatchString(a.Name) {
		return nil, errors.New("name: lower case letters, digits, - or _, starting with a letter, up to 32")
	}
	if a.Source != "" && (len(a.Edits) > 0 || a.Version != 0) || a.Source == "" && len(a.Edits) == 0 && a.Version == 0 {
		return nil, errors.New("give source (a new version), edits (to the newest version, or to version), or version alone (to go back to it)")
	}
	worldMu.Lock()
	defer worldMu.Unlock()

	version := a.Version
	if len(a.Edits) > 0 {
		base := a.Version
		if base == 0 {
			base = newestVersion(a.Name)
		}
		old, err := os.ReadFile(versionPath(a.Name, base, "c"))
		if err != nil {
			return nil, fmt.Errorf("there's no version %d of %s to edit", base, a.Name)
		}
		if a.Source, err = applyEdits(string(old), a.Edits); err != nil {
			return nil, fmt.Errorf("%s v%d unchanged: %w", a.Name, base, err)
		}
	}
	if a.Source != "" {
		dir := filepath.Join(worldDir(), "extensions", a.Name)
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return nil, err
		}
		version = newestVersion(a.Name) + 1
		source := versionPath(a.Name, version, "c")
		if err := os.WriteFile(source, []byte(a.Source), 0o644); err != nil {
			return nil, err
		}
		if a.Placeholder {
			os.WriteFile(versionPath(a.Name, version, "placeholder"), nil, 0o644)
		}
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
		defer cancel()
		start := time.Now()
		cc := exec.CommandContext(ctx, "/bin/sh", compiler, source, versionPath(a.Name, version, "wasm"), a.Name+".c")
		cc.Env = append(os.Environ(), "AFTERGLOW_WORLD_DIR="+worldDir()) // its include/ holds shared headers
		out, err := cc.CombinedOutput()
		if err != nil {
			klog("extension %s v%d didn't compile", a.Name, version)
			return []any{text(fmt.Sprintf("%s v%d didn't compile (the world is unchanged):\n%s", a.Name, version,
				strings.ToValidUTF8(string(out), "�")))}, errCompile
		}
		klog("extension %s v%d compiled in %.1f s", a.Name, version, time.Since(start).Seconds())
	} else if _, err := os.Stat(versionPath(a.Name, version, "wasm")); err != nil {
		return nil, fmt.Errorf("there's no version %d of %s", version, a.Name)
	}

	note, err := ensureWorld()
	if err != nil {
		return nil, err
	}
	start := time.Now()
	reply, err := worldCommand(loadCommand(a.Name, version), 60*time.Second)
	if err != nil {
		return nil, fmt.Errorf("the world didn't answer: %w (see /tmp/world.log)", err)
	}
	result := fmt.Sprintf("%s v%d: %s(%.1f s in all)", a.Name, version, reply, time.Since(start).Seconds())
	if note != "" {
		result = note + "\n" + result
	}
	if strings.HasPrefix(reply, "error") {
		return []any{text(result)}, errLoad
	}
	active := readActive()
	active[a.Name] = version
	writeActive(active)
	return []any{text(result)}, nil
}

func unloadExtension(args json.RawMessage) ([]any, error) {
	var a struct {
		Name string `json:"name"`
	}
	if err := decode(args, &a); err != nil {
		return nil, err
	}
	worldMu.Lock()
	defer worldMu.Unlock()
	active := readActive()
	delete(active, a.Name)
	writeActive(active)
	reply, err := worldCommand("unload "+a.Name, 10*time.Second)
	if err != nil {
		return []any{text("the world isn't running; " + a.Name + " won't be loaded when it starts")}, nil
	}
	return []any{text(reply)}, nil
}

func listExtensions(args json.RawMessage) ([]any, error) {
	if err := decode(args, &struct{}{}); err != nil {
		return nil, err
	}
	worldMu.Lock()
	defer worldMu.Unlock()
	var out strings.Builder
	if reply, err := worldCommand("list", 5*time.Second); err == nil {
		out.WriteString("world running:\n" + reply)
	} else {
		out.WriteString("world not running (load_extension starts it)\n")
	}
	dirs, _ := filepath.Glob(filepath.Join(worldDir(), "extensions", "*"))
	active := readActive()
	for _, dir := range dirs {
		versions, _ := filepath.Glob(filepath.Join(dir, "v*.wasm"))
		name := filepath.Base(dir)
		fmt.Fprintf(&out, "%s: %d compiled version(s) kept", name, len(versions))
		if v, ok := active[name]; ok {
			fmt.Fprintf(&out, ", v%d loaded", v)
			if strings.HasSuffix(loadCommand(name, v), " placeholder") {
				out.WriteString(" (a placeholder)")
			}
		}
		out.WriteString("\n")
	}
	out.WriteString(listTextures())
	return []any{text(out.String())}, nil
}

func startWorld(args json.RawMessage) ([]any, error) {
	if err := decode(args, &struct{}{}); err != nil {
		return nil, err
	}
	worldMu.Lock()
	defer worldMu.Unlock()
	note, err := ensureWorld()
	if err != nil {
		return nil, err
	}
	if note == "" {
		note = "the world was already running"
	}
	return []any{text(note)}, nil
}

func recentreWorld(args json.RawMessage) ([]any, error) {
	if err := decode(args, &struct{}{}); err != nil {
		return nil, err
	}
	worldMu.Lock()
	defer worldMu.Unlock()
	reply, err := worldCommand("recentre", 5*time.Second)
	if err != nil {
		return nil, errors.New("the world isn't running")
	}
	return []any{text(reply)}, nil
}

func stopWorld(args json.RawMessage) ([]any, error) {
	if err := decode(args, &struct{}{}); err != nil {
		return nil, err
	}
	worldMu.Lock()
	defer worldMu.Unlock()
	if _, err := worldCommand("quit", 5*time.Second); err != nil {
		return []any{text("the world wasn't running")}, nil
	}
	return []any{text("world stopped; the status screen is back")}, nil
}

var (
	errCompile = errors.New("compile failed")
	errLoad    = errors.New("load failed")
)
