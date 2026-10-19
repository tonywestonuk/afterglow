package main

// Video of what the wearer sees, for a player on the computer (VLC): start_capture has the world put
// each new frame, scaled down, in a shared file (system/world/capture.c), and the stream at a private
// URL sends them on as Motion JPEG until end_capture. The URL's key is random and dies with the
// capture, so the pairing token never goes in a URL.

import (
	"bytes"
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"image"
	"image/jpeg"
	"net/http"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

const capturePath = "/tmp/afterglow-capture"

var capture struct {
	sync.Mutex
	key string
	fps int
}

func captureTools() []tool {
	return []tool{
		{
			Name: "start_capture",
			Description: "Stream video of what the wearer sees to the computer: returns a private URL that plays in " +
				"VLC (Motion JPEG over HTTP) until end_capture. Open it on the computer, e.g. " +
				"/Applications/VLC.app/Contents/MacOS/VLC --network-caching=300 URL (or ffmpeg -i URL out.mp4 to " +
				"record a file). Needs the world running.",
			InputSchema: schema(nil, map[string]any{
				"eye":       map[string]any{"type": "string", "enum": []string{"left", "right", "both"}, "description": "Default left"},
				"view":      map[string]any{"type": "string", "enum": []string{"eyes", "panels"}, "description": "eyes (default): undistorted; panels: warped for the lenses, as the displays show it"},
				"downscale": prop("integer", "Shrink by this factor, 1 to 4 (default 2: one eye at 720x800)"),
				"fps":       prop("integer", "Frames a second, 1 to 30 (default 25)"),
			}),
			call: startCapture,
		},
		{
			Name:        "end_capture",
			Description: "Stop the video stream started by start_capture (its URL stops working).",
			InputSchema: schema(nil, map[string]any{}),
			call:        endCapture,
		},
	}
}

func startCapture(args json.RawMessage) ([]any, error) {
	var a struct {
		Eye       string `json:"eye"`
		View      string `json:"view"`
		Downscale int    `json:"downscale"`
		FPS       int    `json:"fps"`
	}
	if err := decode(args, &a); err != nil {
		return nil, err
	}
	if a.Eye == "" {
		a.Eye = "left"
	}
	if a.View == "" {
		a.View = "eyes"
	}
	if a.Downscale == 0 {
		a.Downscale = 2
	}
	if a.FPS == 0 {
		a.FPS = 25
	}
	if (a.Eye != "left" && a.Eye != "right" && a.Eye != "both") || (a.View != "eyes" && a.View != "panels") ||
		a.Downscale < 1 || a.Downscale > 4 || a.FPS < 1 || a.FPS > 30 {
		return nil, errors.New("eye left, right or both; view eyes or panels; downscale 1 to 4; fps 1 to 30")
	}
	width, height := 1440/a.Downscale, 1600/a.Downscale
	if a.Eye == "both" {
		width *= 2
	}
	width, height = width&^1, height&^1 // even, as video wants
	reply, err := worldCommand(fmt.Sprintf("capture start %s %s %d %d", a.View, a.Eye, width, height), 5*time.Second)
	if err != nil {
		return nil, errors.New("the world isn't running: start_world first")
	}
	if strings.HasPrefix(reply, "error") {
		return nil, errors.New(strings.TrimSpace(reply))
	}
	key := make([]byte, 16)
	rand.Read(key)
	capture.Lock()
	capture.key, capture.fps = hex.EncodeToString(key), a.FPS
	url := "http://afterglow.local:7070/capture/" + capture.key + ".mjpg"
	capture.Unlock()
	klog("capture started: %dx%d at %d fps", width, height, a.FPS)
	return []any{text(fmt.Sprintf("Streaming %s (%s) at %dx%d, %d fps: %s\n"+
		"Watch it on the computer: /Applications/VLC.app/Contents/MacOS/VLC --network-caching=300 '%s' &\n"+
		"(or open -a VLC '%s'; to record a file: ffmpeg -i '%s' -c:v libx264 -pix_fmt yuv420p capture.mp4)\n"+
		"The URL works until end_capture.", a.Eye, a.View, width, height, a.FPS, url, url, url, url))}, nil
}

func endCapture(args json.RawMessage) ([]any, error) {
	if err := decode(args, &struct{}{}); err != nil {
		return nil, err
	}
	capture.Lock()
	running := capture.key != ""
	capture.key = ""
	capture.Unlock()
	worldCommand("capture stop", 5*time.Second)
	if !running {
		return []any{text("there was no capture running")}, nil
	}
	klog("capture ended")
	return []any{text("capture ended")}, nil
}

func captureLive(key string) (bool, int) {
	capture.Lock()
	defer capture.Unlock()
	return capture.key != "" && subtle.ConstantTimeCompare([]byte(key), []byte(capture.key)) == 1, capture.fps
}

// serveCapture streams the capture as Motion JPEG (multipart/x-mixed-replace), to anyone with the URL.
func serveCapture(w http.ResponseWriter, r *http.Request) {
	key := strings.TrimSuffix(strings.TrimPrefix(r.URL.Path, "/capture/"), ".mjpg")
	live, fps := captureLive(key)
	if !live {
		http.NotFound(w, r)
		return
	}
	f, err := os.Open(capturePath)
	if err != nil {
		http.Error(w, "no capture from the world", http.StatusServiceUnavailable)
		return
	}
	defer f.Close()
	var header [64]byte
	if _, err := f.ReadAt(header[:], 0); err != nil || string(header[:6]) != "AGCAP1" {
		http.Error(w, "no capture from the world", http.StatusServiceUnavailable)
		return
	}
	width := int(*(*uint32)(unsafe.Pointer(&header[8])))
	height := int(*(*uint32)(unsafe.Pointer(&header[12])))
	frameSize := width * height * 4
	mem, err := syscall.Mmap(int(f.Fd()), 0, 64+2*frameSize, syscall.PROT_READ, syscall.MAP_SHARED)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	defer syscall.Munmap(mem)
	sequence := (*uint32)(unsafe.Pointer(&mem[16]))

	w.Header().Set("Content-Type", "multipart/x-mixed-replace; boundary=afterglowframe")
	w.Header().Set("Cache-Control", "no-cache")
	flusher, _ := w.(http.Flusher)
	img := image.NewRGBA(image.Rect(0, 0, width, height))
	var out bytes.Buffer
	tick := time.NewTicker(time.Second / time.Duration(fps))
	defer tick.Stop()
	last := uint32(0)
	klog("capture: %s watching", r.RemoteAddr)
	for {
		select {
		case <-r.Context().Done():
			klog("capture: %s stopped watching", r.RemoteAddr)
			return
		case <-tick.C:
		}
		if live, _ := captureLive(key); !live {
			return
		}
		n := atomic.LoadUint32(sequence)
		if n == 0 || n == last {
			continue
		}
		slot := mem[64+int((n-1)%2)*frameSize:][:frameSize]
		copy(img.Pix, slot)
		if atomic.LoadUint32(sequence)-n >= 2 { // the world came round to this slot while it was copied
			continue
		}
		last = n
		for i := 3; i < len(img.Pix); i += 4 {
			img.Pix[i] = 255
		}
		out.Reset()
		if err := jpeg.Encode(&out, img, &jpeg.Options{Quality: 75}); err != nil {
			return
		}
		if _, err := fmt.Fprintf(w, "--afterglowframe\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", out.Len()); err != nil {
			return
		}
		if _, err := w.Write(append(out.Bytes(), '\r', '\n')); err != nil {
			return
		}
		if flusher != nil {
			flusher.Flush()
		}
	}
}
