package main

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"image"
	"image/png"
	"syscall"
	"unsafe"
)

const (
	fbioGetVScreenInfo = 0x4600
	fbioGetFScreenInfo = 0x4602
)

// screenshot reads the page the display is showing and turns it the way the wearer sees it: the
// panels show the framebuffer rotated 180°, so its left half is the right eye.
func screenshot(args json.RawMessage) ([]any, error) {
	var a struct {
		Eye       string `json:"eye"`
		Downscale int    `json:"downscale"`
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

	fd, err := syscall.Open("/dev/fb0", syscall.O_RDONLY|syscall.O_CLOEXEC, 0)
	if err != nil {
		return nil, err
	}
	defer syscall.Close(fd)
	var v [160]byte // struct fb_var_screeninfo
	var f [80]byte  // struct fb_fix_screeninfo (64-bit)
	for _, q := range []struct {
		req uintptr
		buf unsafe.Pointer
	}{{fbioGetVScreenInfo, unsafe.Pointer(&v)}, {fbioGetFScreenInfo, unsafe.Pointer(&f)}} {
		if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), q.req, uintptr(q.buf)); errno != 0 {
			return nil, fmt.Errorf("framebuffer ioctl: %w", errno)
		}
	}
	u32 := func(b []byte, off int) int { return int(binary.LittleEndian.Uint32(b[off:])) }
	xres, yres, xoff, yoff, bpp := u32(v[:], 0), u32(v[:], 4), u32(v[:], 16), u32(v[:], 20), u32(v[:], 24)
	rOff, gOff, bOff := u32(v[:], 32), u32(v[:], 44), u32(v[:], 56)
	smemLen, stride := u32(f[:], 24), u32(f[:], 48)
	if bpp != 32 {
		return nil, fmt.Errorf("unsupported framebuffer format: %d bits per pixel", bpp)
	}
	mem, err := syscall.Mmap(fd, 0, smemLen, syscall.PROT_READ, syscall.MAP_SHARED)
	if err != nil {
		return nil, err
	}
	defer syscall.Munmap(mem)
	start := yoff*stride + xoff*4
	if start+yres*stride > len(mem) {
		return nil, fmt.Errorf("displayed page is outside the framebuffer")
	}
	page := make([]byte, yres*stride) // one copy out of (slow, uncached) video memory
	copy(page, mem[start:start+yres*stride])

	// The wearer's view: (x, y) is framebuffer pixel (xres-1-x, yres-1-y)
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
				row := page[(yres-1-(oy*k+dy))*stride:]
				for dx := 0; dx < k; dx++ {
					p := binary.LittleEndian.Uint32(row[(xres-1-(x0+ox*k+dx))*4:])
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
	return []any{
		map[string]any{"type": "image", "data": base64.StdEncoding.EncodeToString(out.Bytes()), "mimeType": "image/png"},
		text(fmt.Sprintf("%s as the wearer sees them, %dx%d (1/%d scale)", which, ow, oh, k)),
	}, nil
}
