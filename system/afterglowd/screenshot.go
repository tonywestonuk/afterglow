package main

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"image"
	"image/png"
	"os"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

const (
	fbioGetVScreenInfo = 0x4600
	fbioGetFScreenInfo = 0x4602
)

// screenshot shows what the wearer sees. While the world runs it asks the world for its newest frame
// whole ("eyes": the eye buffers, undistorted; "panels": warped for the lenses), since the framebuffer
// flips 72 times a second and reading it takes longer than that; otherwise ("screen") it reads the page
// the display is showing, which the panels show rotated 180° (its left half is the right eye).
func screenshot(args json.RawMessage) ([]any, error) {
	var a struct {
		Eye       string `json:"eye"`
		Downscale int    `json:"downscale"`
		View      string `json:"view"`
	}
	if err := decode(args, &a); err != nil {
		return nil, err
	}
	k := 2
	if a.Downscale != 0 {
		k = a.Downscale
	}
	if k < 1 || k > 4 {
		return nil, fmt.Errorf("downscale must be 1 to 4")
	}
	if a.View == "" {
		a.View = "screen"
		if _, err := worldCommand("list", 2*time.Second); err == nil {
			a.View = "eyes"
		}
	}

	var xres, yres, rOff, gOff, bOff int
	var pixel func(x, y int) uint32 // as the wearer sees it, (0, 0) top left
	switch a.View {
	case "eyes", "panels":
		snap := "/tmp/afterglow-snapshot.rgba"
		reply, err := worldCommand("snapshot "+snap+" "+a.View, 10*time.Second)
		if err != nil || strings.HasPrefix(reply, "error") {
			return nil, fmt.Errorf("the world's snapshot: %v %s(view \"screen\" reads the display itself)", err,
				strings.TrimSpace(reply)+" ")
		}
		data, err := os.ReadFile(snap)
		os.Remove(snap)
		xres, yres = 2880, 1600
		if err != nil || len(data) != xres*yres*4 {
			return nil, fmt.Errorf("the world's snapshot is unreadable: %v", err)
		}
		rOff, gOff, bOff = 0, 8, 16
		pixel = func(x, y int) uint32 { return binary.LittleEndian.Uint32(data[(y*xres+x)*4:]) }
	case "screen":
		page, stride, xr, yr, offs, err := readFramebuffer()
		if err != nil {
			return nil, err
		}
		xres, yres, rOff, gOff, bOff = xr, yr, offs[0], offs[1], offs[2]
		pixel = func(x, y int) uint32 { return binary.LittleEndian.Uint32(page[(yres-1-y)*stride+(xres-1-x)*4:]) }
	default:
		return nil, fmt.Errorf("view must be eyes, panels or screen")
	}

	x0, w := 0, xres
	switch a.Eye {
	case "", "both":
	case "left":
		w = xres / 2
	case "right":
		x0, w = xres/2, xres/2
	default:
		return nil, fmt.Errorf("eye must be both, left or right")
	}
	ow, oh := w/k, yres/k
	img := image.NewNRGBA(image.Rect(0, 0, ow, oh))
	for oy := 0; oy < oh; oy++ {
		for ox := 0; ox < ow; ox++ {
			var r, g, b int
			for dy := 0; dy < k; dy++ {
				for dx := 0; dx < k; dx++ {
					p := pixel(x0+ox*k+dx, oy*k+dy)
					r += int(p>>rOff) & 0xff
					g += int(p>>gOff) & 0xff
					b += int(p>>bOff) & 0xff
				}
			}
			i := img.PixOffset(ox, oy)
			img.Pix[i], img.Pix[i+1], img.Pix[i+2], img.Pix[i+3] = uint8(r/(k*k)), uint8(g/(k*k)), uint8(b/(k*k)), 255
		}
	}
	var out bytes.Buffer
	if err := (&png.Encoder{CompressionLevel: png.BestSpeed}).Encode(&out, img); err != nil {
		return nil, err
	}
	which := map[string]string{"": "both eyes, left eye on the left", "both": "both eyes, left eye on the left",
		"left": "left eye", "right": "right eye"}[a.Eye]
	how := map[string]string{"eyes": "the world's newest frame, undistorted",
		"panels": "the world's newest frame as it goes to the panels, warped for the lenses",
		"screen": "the display as it is"}[a.View]
	return []any{
		map[string]any{"type": "image", "data": base64.StdEncoding.EncodeToString(out.Bytes()), "mimeType": "image/png"},
		text(fmt.Sprintf("%s as the wearer sees them, %dx%d (1/%d scale): %s", which, ow, oh, k, how)),
	}, nil
}

// readFramebuffer copies the page the display is showing, with its geometry and colour layout.
func readFramebuffer() (page []byte, stride, xres, yres int, offsets [3]int, err error) {
	fd, err := syscall.Open("/dev/fb0", syscall.O_RDONLY|syscall.O_CLOEXEC, 0)
	if err != nil {
		return nil, 0, 0, 0, offsets, err
	}
	defer syscall.Close(fd)
	var v [160]byte // struct fb_var_screeninfo
	var f [80]byte  // struct fb_fix_screeninfo (64-bit)
	for _, q := range []struct {
		req uintptr
		buf unsafe.Pointer
	}{{fbioGetVScreenInfo, unsafe.Pointer(&v)}, {fbioGetFScreenInfo, unsafe.Pointer(&f)}} {
		if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), q.req, uintptr(q.buf)); errno != 0 {
			return nil, 0, 0, 0, offsets, fmt.Errorf("framebuffer ioctl: %w", errno)
		}
	}
	u32 := func(b []byte, off int) int { return int(binary.LittleEndian.Uint32(b[off:])) }
	xres, yres = u32(v[:], 0), u32(v[:], 4)
	xoff, yoff, bpp := u32(v[:], 16), u32(v[:], 20), u32(v[:], 24)
	offsets = [3]int{u32(v[:], 32), u32(v[:], 44), u32(v[:], 56)}
	smemLen := u32(f[:], 24)
	stride = u32(f[:], 48)
	if bpp != 32 {
		return nil, 0, 0, 0, offsets, fmt.Errorf("unsupported framebuffer format: %d bits per pixel", bpp)
	}
	mem, err := syscall.Mmap(fd, 0, smemLen, syscall.PROT_READ, syscall.MAP_SHARED)
	if err != nil {
		return nil, 0, 0, 0, offsets, err
	}
	defer syscall.Munmap(mem)
	start := yoff*stride + xoff*4
	if start+yres*stride > len(mem) {
		return nil, 0, 0, 0, offsets, fmt.Errorf("displayed page is outside the framebuffer")
	}
	page = make([]byte, yres*stride) // one copy out of (slow, uncached) video memory
	copy(page, mem[start:start+yres*stride])
	return page, stride, xres, yres, offsets, nil
}
