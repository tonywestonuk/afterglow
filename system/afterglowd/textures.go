package main

// Textures for the world: images fetched from the web or made by an image generator on the network
// (Stable Diffusion and the like, through the AUTOMATIC1111 web API that most of them offer), kept as
// PNG in <world dir>/textures/NAME.png for extensions' texture_load("NAME"). Claude never handles the
// image itself, only a small preview to check it.

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"image"
	"image/draw"
	_ "image/jpeg"
	"image/png"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

const maxTextureSide = 2048

var imageGeneratorFile = "/run/afterglowd/image-generator" // the generator's address, remembered

func textureTools() []tool {
	return []tool{
		{
			Name: "generate_texture",
			Description: "Have an image generator on the network (Stable Diffusion and the like, through the " +
				"AUTOMATIC1111 web API: AUTOMATIC1111, Forge, SD.Next, Draw Things' HTTP server...) paint a texture, " +
				"and keep it on the headset for extensions' texture_load(\"name\"). Describe a material seen " +
				"straight on, evenly lit (e.g. \"short green lawn grass, top-down photo, even light\"). Takes " +
				"seconds to minutes: put a placeholder up first. Returns a preview.",
			InputSchema: schema([]string{"name", "prompt"}, map[string]any{
				"name":     prop("string", "Texture name: lower case, digits, - or _ (e.g. grass)"),
				"prompt":   prop("string", "What to paint"),
				"seamless": prop("boolean", "Make it repeat without visible seams (for floors, walls, terrain)"),
				"size":     prop("integer", "Pixels each way: 256 to 1024, a multiple of 64 (default 512)"),
				"server": prop("string", "The generator's address, e.g. http://192.168.1.20:7860 (remembered; it "+
					"must accept connections from the network, not just from its own computer)"),
			}),
			call: generateTexture,
		},
		{
			Name: "fetch_texture",
			Description: "Download an image (PNG or JPEG, over http or https) and keep it on the headset as a texture " +
				"for extensions' texture_load(\"name\"). Good sources of free (CC0) textures: Poly Haven, ambientCG. " +
				"Returns a preview.",
			InputSchema: schema([]string{"name", "url"}, map[string]any{
				"name":     prop("string", "Texture name: lower case, digits, - or _ (e.g. bricks)"),
				"url":      prop("string", "The image's address"),
				"seamless": prop("boolean", "Make it repeat without visible seams, if it doesn't already"),
			}),
			call: fetchTexture,
		},
	}
}

func textureDir() string { return filepath.Join(worldDir(), "textures") }

func fetchTexture(args json.RawMessage) ([]any, error) {
	var a struct {
		Name     string `json:"name"`
		URL      string `json:"url"`
		Seamless bool   `json:"seamless"`
	}
	if err := decode(args, &a); err != nil {
		return nil, err
	}
	if !extNameRule.MatchString(a.Name) {
		return nil, errors.New("name: lower case letters, digits, - or _, starting with a letter, up to 32")
	}
	if u, err := url.Parse(a.URL); err != nil || (u.Scheme != "http" && u.Scheme != "https") {
		return nil, errors.New("url: an http or https address")
	}
	resp, err := (&http.Client{Timeout: 2 * time.Minute}).Get(a.URL)
	if err != nil {
		return nil, fmt.Errorf("fetching it: %w%s", err, clockHint(err))
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("fetching it: %s", resp.Status)
	}
	data, err := io.ReadAll(io.LimitReader(resp.Body, 64<<20))
	if err != nil {
		return nil, err
	}
	return saveTexture(a.Name, data, a.Seamless, "from "+a.URL)
}

// clockHint explains certificate errors that come from the headset's clock being wrong.
func clockHint(err error) string {
	if strings.Contains(err.Error(), "certificate") && time.Now().Year() < 2026 {
		return fmt.Sprintf(" (the headset's clock says %s; it's set by NTP once Wi-Fi is up)", time.Now().Format("2006-01-02"))
	}
	return ""
}

func generateTexture(args json.RawMessage) ([]any, error) {
	var a struct {
		Name     string `json:"name"`
		Prompt   string `json:"prompt"`
		Seamless bool   `json:"seamless"`
		Size     int    `json:"size"`
		Server   string `json:"server"`
	}
	if err := decode(args, &a); err != nil {
		return nil, err
	}
	if !extNameRule.MatchString(a.Name) {
		return nil, errors.New("name: lower case letters, digits, - or _, starting with a letter, up to 32")
	}
	if a.Size == 0 {
		a.Size = 512
	}
	if a.Size < 256 || a.Size > 1024 || a.Size%64 != 0 {
		return nil, errors.New("size: 256 to 1024, a multiple of 64")
	}
	if a.Server != "" {
		if u, err := url.Parse(a.Server); err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
			return nil, errors.New("server: an address such as http://192.168.1.20:7860")
		}
		os.WriteFile(imageGeneratorFile, []byte(strings.TrimRight(a.Server, "/")+"\n"), 0o600)
	} else if saved, err := os.ReadFile(imageGeneratorFile); err == nil {
		a.Server = strings.TrimSpace(string(saved))
	} else {
		return nil, errors.New("no image generator known yet: pass server, the address of one with the " +
			"AUTOMATIC1111 web API (e.g. AUTOMATIC1111 or Forge started with --api --listen, or Draw Things with " +
			"its HTTP API server on and reachable from the network); it's remembered")
	}

	request, _ := json.Marshal(map[string]any{
		"prompt":          a.Prompt,
		"negative_prompt": "text, words, watermark, signature, logo, frame, border, vignette",
		"width":           a.Size,
		"height":          a.Size,
		"tiling":          a.Seamless,
		"batch_size":      1,
		"n_iter":          1,
	})
	start := time.Now()
	resp, err := (&http.Client{Timeout: 10 * time.Minute}).Post(a.Server+"/sdapi/v1/txt2img", "application/json",
		bytes.NewReader(request))
	if err != nil {
		return nil, fmt.Errorf("the image generator at %s: %w (is it running, with its API on and reachable "+
			"from the network?)", a.Server, err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 256<<20))
	if err != nil {
		return nil, err
	}
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("the image generator at %s said %s: %s", a.Server, resp.Status,
			strings.ToValidUTF8(string(body[:min(len(body), 500)]), "?"))
	}
	var result struct {
		Images []string `json:"images"`
	}
	if err := json.Unmarshal(body, &result); err != nil || len(result.Images) == 0 {
		return nil, fmt.Errorf("the image generator at %s didn't send an image back (is it an AUTOMATIC1111-style API?)", a.Server)
	}
	encoded := result.Images[0]
	if i := strings.Index(encoded, ","); strings.HasPrefix(encoded, "data:") && i > 0 {
		encoded = encoded[i+1:]
	}
	data, err := base64.StdEncoding.DecodeString(encoded)
	if err != nil {
		return nil, fmt.Errorf("the image generator's image: %w", err)
	}
	return saveTexture(a.Name, data, a.Seamless, fmt.Sprintf("generated in %.0f s", time.Since(start).Seconds()))
}

// saveTexture checks an image, shrinks it if it's huge, makes it seamless if asked, and keeps it as
// textureDir()/NAME.png. It returns a preview and a note of what it did.
func saveTexture(name string, data []byte, seamless bool, origin string) ([]any, error) {
	decoded, format, err := image.Decode(bytes.NewReader(data))
	if err != nil {
		return nil, fmt.Errorf("not a PNG or JPEG image: %w", err)
	}
	img := toNRGBA(decoded)
	notes := []string{fmt.Sprintf("%s, %s", origin, format)}
	if w, h := img.Rect.Dx(), img.Rect.Dy(); w > maxTextureSide || h > maxTextureSide {
		k := float64(maxTextureSide) / float64(max(w, h))
		img = shrink(img, max(1, int(float64(w)*k)), max(1, int(float64(h)*k)))
		notes = append(notes, fmt.Sprintf("shrunk from %dx%d", w, h))
	}
	if seamless {
		if blendEdges(img) {
			notes = append(notes, "edges blended so it repeats without seams")
		} else {
			notes = append(notes, "it already repeats without seams")
		}
	}
	if err := os.MkdirAll(textureDir(), 0o755); err != nil {
		return nil, err
	}
	var out bytes.Buffer
	if err := (&png.Encoder{CompressionLevel: png.BestSpeed}).Encode(&out, img); err != nil {
		return nil, err
	}
	path := filepath.Join(textureDir(), name+".png")
	if err := os.WriteFile(path+".part", out.Bytes(), 0o644); err != nil {
		return nil, err
	}
	if err := os.Rename(path+".part", path); err != nil {
		return nil, err
	}
	for _, other := range []string{".jpg", ".jpeg"} { // the world would prefer the PNG anyway
		os.Remove(filepath.Join(textureDir(), name+other))
	}
	klog("texture %s: %dx%d (%s)", name, img.Rect.Dx(), img.Rect.Dy(), strings.Join(notes, "; "))

	preview := img
	if w, h := img.Rect.Dx(), img.Rect.Dy(); max(w, h) > 256 {
		preview = shrink(img, max(1, w*256/max(w, h)), max(1, h*256/max(w, h)))
	}
	if seamless { // two by two, so any seam would show
		tiled := image.NewNRGBA(image.Rect(0, 0, preview.Rect.Dx()*2, preview.Rect.Dy()*2))
		for i := 0; i < 4; i++ {
			at := image.Pt(i%2*preview.Rect.Dx(), i/2*preview.Rect.Dy())
			draw.Draw(tiled, preview.Rect.Add(at), preview, image.Point{}, draw.Src)
		}
		preview = tiled
	}
	var small bytes.Buffer
	png.Encode(&small, preview)
	return []any{
		map[string]any{"type": "image", "data": base64.StdEncoding.EncodeToString(small.Bytes()), "mimeType": "image/png"},
		text(fmt.Sprintf("texture %s: %dx%d (%s), saved as %s. In an extension: texture_load(\"%s\")%s", name,
			img.Rect.Dx(), img.Rect.Dy(), strings.Join(notes, "; "), path, name,
			map[bool]string{true: "; the preview shows it 2x2", false: ""}[seamless])),
	}, nil
}

func toNRGBA(src image.Image) *image.NRGBA {
	if n, ok := src.(*image.NRGBA); ok && n.Rect.Min == (image.Point{}) {
		return n
	}
	b := src.Bounds()
	dst := image.NewNRGBA(image.Rect(0, 0, b.Dx(), b.Dy()))
	draw.Draw(dst, dst.Rect, src, b.Min, draw.Src)
	return dst
}

// shrink averages each output pixel's area of the input.
func shrink(src *image.NRGBA, w, h int) *image.NRGBA {
	dst := image.NewNRGBA(image.Rect(0, 0, w, h))
	sw, sh := src.Rect.Dx(), src.Rect.Dy()
	for y := 0; y < h; y++ {
		y0, y1 := y*sh/h, max((y+1)*sh/h, y*sh/h+1)
		for x := 0; x < w; x++ {
			x0, x1 := x*sw/w, max((x+1)*sw/w, x*sw/w+1)
			var sum [4]int
			for sy := y0; sy < y1; sy++ {
				for sx := x0; sx < x1; sx++ {
					p := src.Pix[src.PixOffset(sx, sy):]
					for k := 0; k < 4; k++ {
						sum[k] += int(p[k])
					}
				}
			}
			n := (y1 - y0) * (x1 - x0)
			d := dst.Pix[dst.PixOffset(x, y):]
			for k := 0; k < 4; k++ {
				d[k] = uint8(sum[k] / n)
			}
		}
	}
	return dst
}

// blendEdges makes an image repeat without seams, if it doesn't already: across each way in turn, its
// outer fifth is blended into a copy shifted by half, which runs on smoothly past the edge. It
// reports whether it changed anything.
func blendEdges(img *image.NRGBA) bool {
	changed := false
	for _, across := range []bool{true, false} {
		if tilesAlready(img, across) {
			continue
		}
		changed = true
		w, h := img.Rect.Dx(), img.Rect.Dy()
		src := image.NewNRGBA(img.Rect)
		copy(src.Pix, img.Pix)
		n := w
		if !across {
			n = h
		}
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				i := x
				if !across {
					i = y
				}
				// 0 at the edges, 1 from a fifth of the way in: keep the original there
				t := float64(min(i, n-1-i)) / (0.2 * float64(n))
				if t >= 1 {
					continue
				}
				keep := t * t * (3 - 2*t)
				sx, sy := (x+w/2)%w, y
				if !across {
					sx, sy = x, (y+h/2)%h
				}
				a, b := src.Pix[src.PixOffset(x, y):], src.Pix[src.PixOffset(sx, sy):]
				d := img.Pix[img.PixOffset(x, y):]
				for k := 0; k < 4; k++ {
					d[k] = uint8(float64(a[k])*keep + float64(b[k])*(1-keep) + 0.5)
				}
			}
		}
	}
	return changed
}

// tilesAlready: is the step from one edge to the other no bigger than a typical step between neighbours?
func tilesAlready(img *image.NRGBA, across bool) bool {
	w, h := img.Rect.Dx(), img.Rect.Dy()
	diff := func(x0, y0, x1, y1 int) int {
		a, b := img.Pix[img.PixOffset(x0, y0):], img.Pix[img.PixOffset(x1, y1):]
		d := 0
		for k := 0; k < 3; k++ {
			d += max(int(a[k])-int(b[k]), int(b[k])-int(a[k]))
		}
		return d
	}
	edge, inside, count := 0, 0, 0
	if across {
		for y := 0; y < h; y++ {
			edge += diff(w-1, y, 0, y)
			inside += diff(w/3, y, w/3+1, y) + diff(2*w/3, y, 2*w/3+1, y)
			count++
		}
	} else {
		for x := 0; x < w; x++ {
			edge += diff(x, h-1, x, 0)
			inside += diff(x, h/3, x, h/3+1) + diff(x, 2*h/3, x, 2*h/3+1)
			count++
		}
	}
	return float64(edge) <= 1.5*float64(inside)/2+float64(count)*3
}

// listTextures is a line for list_extensions: the textures kept, with their sizes.
func listTextures() string {
	paths, _ := filepath.Glob(filepath.Join(textureDir(), "*"))
	var names []string
	for _, path := range paths {
		ext := filepath.Ext(path)
		if ext != ".png" && ext != ".jpg" && ext != ".jpeg" {
			continue
		}
		f, err := os.Open(path)
		if err != nil {
			continue
		}
		config, _, err := image.DecodeConfig(f)
		f.Close()
		if err == nil {
			names = append(names, fmt.Sprintf("%s (%dx%d)", strings.TrimSuffix(filepath.Base(path), ext), config.Width, config.Height))
		}
	}
	if len(names) == 0 {
		return "textures: none yet (generate_texture, fetch_texture)\n"
	}
	sort.Strings(names)
	return "textures: " + strings.Join(names, ", ") + "\n"
}
