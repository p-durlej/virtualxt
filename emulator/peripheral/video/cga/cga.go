// +build sdl

/*
Copyright (C) 2019-2020 Andreas T Jonsson

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

package cga

import (
	"fmt"
	"math/rand"
	"sync"
	"sync/atomic"
	"time"

	"github.com/andreas-jonsson/virtualxt/emulator/dialog"
	"github.com/andreas-jonsson/virtualxt/emulator/memory"
	"github.com/andreas-jonsson/virtualxt/emulator/processor"
	"github.com/veandco/go-sdl2/sdl"
)

const (
	memorySize = 0x4000
	memoryBase = 0xB8000
)

var applicationStart = time.Now()

var cgaColor = []uint32{
	0x000000,
	0x0000AA,
	0x00AA00,
	0x00AAAA,
	0xAA0000,
	0xAA00AA,
	0xAA5500,
	0xAAAAAA,
	0x555555,
	0x5555FF,
	0x55FF55,
	0x55FFFF,
	0xFF5555,
	0xFF55FF,
	0xFFFF55,
	0xFFFFFF,
}

type consoleCursor struct {
	update, visible bool
	position        uint16
}

type Device struct {
	lock     sync.RWMutex
	quitChan chan struct{}

	dirtyMemory bool
	mem         [memorySize]byte
	crtReg      [0x100]byte

	crtAddr, modeCtrlReg,
	colorCtrlReg, refresh byte

	window   *sdl.Window
	renderer *sdl.Renderer
	surface  *sdl.Surface
	texture  *sdl.Texture

	windowTitleTicker  *time.Ticker
	atomicCycleCounter int32

	cursor consoleCursor
	p      processor.Processor
}

func (m *Device) Install(p processor.Processor) error {
	m.p = p
	m.windowTitleTicker = time.NewTicker(time.Second)
	m.quitChan = make(chan struct{})

	// Scramble memory.
	rand.Read(m.mem[:])

	if err := p.InstallMemoryDevice(m, memoryBase, memoryBase+memorySize); err != nil {
		return err
	}
	if err := p.InstallIODevice(m, 0x3B0, 0x3DF); err != nil {
		return err
	}
	return m.startRenderLoop()
}

func (m *Device) Name() string {
	return "Color Graphics Adapter"
}

func (m *Device) Reset() {
	m.colorCtrlReg = 0x20
	m.modeCtrlReg = 1
	m.cursor.visible = true
}

func (m *Device) Step(cycles int) error {
	atomic.AddInt32(&m.atomicCycleCounter, int32(cycles))
	return nil
}

func (m *Device) Close() error {
	m.quitChan <- struct{}{}
	<-m.quitChan
	sdl.Do(func() {
		sdl.QuitSubSystem(sdl.INIT_VIDEO)
	})
	return nil
}

func blit32(pixels []byte, offset int, color uint32) {
	pixels[offset] = byte((color & 0xFF0000) >> 16)
	pixels[offset+1] = byte((color & 0x00FF00) >> 8)
	pixels[offset+2] = byte(color & 0x0000FF)
	pixels[offset+3] = 0xFF
}

func blinkTick() bool {
	return ((time.Since(applicationStart)/time.Millisecond)/500)%2 == 0
}

func (m *Device) blitChar(ch, attrib byte, x, y int) {
	pixels := m.surface.Pixels()
	bgColor := cgaColor[(attrib&0x70)>>4]
	blink := attrib&0x80 != 0 && m.modeCtrlReg&0x20 != 0
	fgColor := cgaColor[attrib&0xF]

	if blink && blinkTick() {
		fgColor = bgColor
	}

	charWidth := 1
	if m.modeCtrlReg&1 == 0 {
		charWidth = 2
	}

	for i := 0; i < 8; i++ {
		glyphLine := cgaFont[int(ch)*8+i]
		for j := 0; j < 8; j++ {
			mask := byte(0x80 >> j)
			col := fgColor
			if glyphLine&mask == 0 {
				col = bgColor
			}
			offset := (int(m.surface.W)*(y+i) + x*charWidth + j*charWidth) * 4
			blit32(pixels, offset, col)
			if charWidth == 2 { // 40 columns?
				blit32(pixels, offset+4, col)
			}
		}
	}
}

func (m *Device) startRenderLoop() error {
	var err error
	sdl.Do(func() {
		if err = sdl.InitSubSystem(sdl.INIT_VIDEO); err != nil {
			return
		}

		sdl.SetHint(sdl.HINT_RENDER_SCALE_QUALITY, "0")
		sdl.SetHint(sdl.HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, "1")
		if m.window, m.renderer, err = sdl.CreateWindowAndRenderer(640, 480, sdl.WINDOW_RESIZABLE); err != nil {
			return
		}
		m.window.SetTitle("VirtualXT")
		if m.surface, err = sdl.CreateRGBSurface(0, 640, 200, 32, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF); err != nil {
			return
		}
		if m.texture, err = m.renderer.CreateTexture(sdl.PIXELFORMAT_ABGR8888, sdl.TEXTUREACCESS_STREAMING, 640, 200); err != nil {
			return
		}
		err = m.renderer.SetLogicalSize(640, 480)
	})
	if err != nil {
		return err
	}

	go func() {
		ticker := time.NewTicker(time.Second / 60)
		defer ticker.Stop()

		for {
			select {
			case <-m.quitChan:
				close(m.quitChan)
				return
			case <-ticker.C:
				sdl.Do(func() {
					m.lock.RLock()
					defer m.lock.RUnlock()

					select {
					case <-m.windowTitleTicker.C:
						hlp := " (Press F12 for menu)"
						if dialog.MainMenuWasOpen() || time.Since(applicationStart) > time.Second*10 {
							hlp = ""
						}
						numCycles := float64(atomic.SwapInt32(&m.atomicCycleCounter, 0))
						m.window.SetTitle(fmt.Sprintf("VirtualXT - %.2f MIPS%s", numCycles/1000000, hlp))
					default:
					}

					blink := blinkTick()
					if m.dirtyMemory || m.cursor.update || blink {
						backgroundColor := cgaColor[m.colorCtrlReg&0xF]
						m.renderer.SetDrawColor(byte(backgroundColor&0xFF0000), byte(backgroundColor&0x00FF00), byte(backgroundColor&0x0000FF), 0xFF)
						m.renderer.Clear()

						// In graphics mode?
						if m.modeCtrlReg&2 != 0 {
							dst := m.surface.Pixels()

							// Is in high-resolution mode?
							if m.modeCtrlReg&0x10 != 0 {
								for y := 0; y < 200; y++ {
									for x := 0; x < 640; x++ {
										addr := (y>>1)*80 + (y&1)*8192 + (x >> 3)
										pixel := (m.mem[addr] >> (7 - (x & 7))) & 1
										col := cgaColor[pixel*15]
										offset := (y*640 + x) * 4
										blit32(dst, offset, col)
									}
								}
							} else {
								palette := (m.colorCtrlReg >> 5) & 1
								intensity := ((m.colorCtrlReg >> 4) & 1) << 3

								for y := 0; y < 200; y++ {
									for x := 0; x < 320; x++ {
										addr := (y>>1)*80 + (y&1)*8192 + (x >> 2)
										pixel := m.mem[addr]

										switch x & 3 {
										case 0:
											pixel = (pixel >> 6) & 3
										case 1:
											pixel = (pixel >> 4) & 3
										case 2:
											pixel = (pixel >> 2) & 3
										case 3:
											pixel = pixel & 3
										}

										col := backgroundColor
										if pixel != 0 {
											col = cgaColor[pixel*2+palette+intensity]
										}

										offset := (y*640 + x*2) * 4
										blit32(dst, offset, col)
										blit32(dst, offset+4, col)
									}
								}
							}
						} else {
							numCol := 80
							if m.modeCtrlReg&1 == 0 {
								numCol = 40
							}

							videoPage := int(m.crtReg[0xC]<<8) + int(m.crtReg[0xD])
							numChar := numCol * 25

							for i := 0; i < numChar*2; i += 2 {
								ch := m.mem[videoPage+i]
								idx := i / 2
								m.blitChar(ch, m.mem[videoPage+i+1], (idx%numCol)*8, (idx/numCol)*8)
							}

							if blink {
								x := int(m.cursor.position) % numCol
								y := int(m.cursor.position) / numCol
								if x < 80 && y < 25 {
									attr := (m.mem[videoPage+(numCol*2*y+x*2+1)] & 0x70) | 0xF
									m.blitChar('_', attr, x*8, y*8)
								}
							}
						}

						m.texture.Update(nil, m.surface.Pixels(), int(m.surface.Pitch))
					}

					m.renderer.Copy(m.texture, nil, nil)
					m.renderer.Present()
				})
			}
		}
	}()

	return nil
}

func (m *Device) In(port uint16) byte {
	m.lock.Lock()
	defer m.lock.Unlock()

	switch port {
	case 0x3D1, 0x3D3, 0x3D5, 0x3D7:
		return m.crtReg[m.crtAddr]
	case 0x3DA:
		m.refresh ^= 0x9
		return m.refresh
	case 0x3B9:
		return m.colorCtrlReg
	}
	return 0
}

func (m *Device) Out(port uint16, data byte) {
	m.lock.Lock()
	defer m.lock.Unlock()

	switch port {
	case 0x3D0, 0x3D2, 0x3D4, 0x3D6:
		m.crtAddr = data
	case 0x3D1, 0x3D3, 0x3D5, 0x3D7:
		m.crtReg[m.crtAddr] = data
		switch m.crtAddr {
		case 0xA:
			m.cursor.update = true
			m.cursor.visible = data&0x20 != 0
		case 0xE:
			m.cursor.update = true
			m.cursor.position = (m.cursor.position & 0x00FF) | (uint16(data) << 8)
		case 0xF:
			m.cursor.update = true
			m.cursor.position = (m.cursor.position & 0xFF00) | uint16(data)
		}
	case 0x3D8:
		m.modeCtrlReg = data
	case 0x3B9:
		m.colorCtrlReg = data
	}
}

func (m *Device) ReadByte(addr memory.Pointer) byte {
	m.lock.RLock()
	v := m.mem[(addr-memoryBase)&0x3FFF]
	m.lock.RUnlock()
	return v
}

func (m *Device) WriteByte(addr memory.Pointer, data byte) {
	m.lock.Lock()
	m.dirtyMemory = true
	m.mem[(addr-memoryBase)&0x3FFF] = data
	m.lock.Unlock()
}
