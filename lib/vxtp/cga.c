/*
Copyright (c) 2019-2022 Andreas T Jonsson

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
*/

#include "vxtp.h"
#include "cga_font.h"

#define MEMORY_SIZE 0x10000
#define MEMORY_START 0xB0000
#define CGA_BASE 0x8000
#define SCANLINE_TIMING 31469

#define INT64 long long

vxt_dword cga_palette[] = {
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
};

struct snapshot {
    vxt_byte mem[MEMORY_SIZE];
    vxt_byte rgba_surface[720 * 350 * 4];

    bool cursor_visible;
    int cursor_offset;

    bool hgc_mode;
    int video_page;
    vxt_byte mode_ctrl_reg;
    vxt_byte color_ctrl_reg;
};

VXT_PIREPHERAL(cga_video, {
    vxt_byte mem[MEMORY_SIZE];
    bool is_dirty;
    struct snapshot snap;

    bool hgc_mode;
    int hgc_base;
    vxt_byte hgc_enable;

    bool cursor_visible;
    int cursor_offset;

    INT64 last_scanline;
	int current_scanline;

    INT64 (*ustics)(void);
    INT64 application_start;

    vxt_byte mode_ctrl_reg;
    vxt_byte color_ctrl_reg;
    vxt_byte status_reg;
    vxt_byte crt_addr;
    vxt_byte crt_reg[0x100];
})

static vxt_byte read(struct vxt_pirepheral *p, vxt_pointer addr) {
    return (VXT_GET_DEVICE(cga_video, p))->mem[addr - MEMORY_START];
}

static void write(struct vxt_pirepheral *p, vxt_pointer addr, vxt_byte data) {
    VXT_DEC_DEVICE(c, cga_video, p);
    c->mem[addr - MEMORY_START] = data;
    c->is_dirty = true;
}

static vxt_byte in(struct vxt_pirepheral *p, vxt_word port) {
    VXT_DEC_DEVICE(c, cga_video, p);
    switch (port) {
        case 0x3B1:
        case 0x3B3:
        case 0x3B5:
        case 0x3B7:
	    case 0x3D1:
        case 0x3D3:
        case 0x3D5:
        case 0x3D7:
            return c->crt_reg[c->crt_addr];
        case 0x3D9:
		    return c->color_ctrl_reg;
        case 0x3BA:
            if (!c->hgc_mode)
                return 0xFF;
            break;
        case 0x3DA:
            if (c->hgc_mode)
                return 0xFF;
            break;
        default:
            return 0;
    }
    
    vxt_byte status = c->status_reg;
    c->status_reg &= 0xFE;
    return status;
}

static void out(struct vxt_pirepheral *p, vxt_word port, vxt_byte data) {
    VXT_DEC_DEVICE(c, cga_video, p);
    c->is_dirty = true;
    switch (port) {
        case 0x3B0:
        case 0x3B2:
        case 0x3B4:
        case 0x3B6:
        case 0x3D0:
        case 0x3D2:
        case 0x3D4:
        case 0x3D6:
            c->crt_addr = data;
            break;
        case 0x3B1:
        case 0x3B3:
        case 0x3B5:
        case 0x3B7:
        case 0x3D1:
        case 0x3D3:
        case 0x3D5:
        case 0x3D7:
        	c->crt_reg[c->crt_addr] = data;
            switch (c->crt_addr) {
                case 0xA:
                    c->cursor_visible = (data & 0x20) != 0;
                    break;
                case 0xE:
                    c->cursor_offset = (c->cursor_offset & 0x00FF) | ((vxt_word)data << 8);
                    break;
                case 0xF:
                    c->cursor_offset = (c->cursor_offset & 0xFF00) | (vxt_word)data;
                    break;
            }
            break;
        case 0x3B8: // Bit 7 is HGC page and bit 1 is gfx select.
            c->hgc_mode = ((c->hgc_enable & 1) & ((data & 2) >> 1)) != 0;
            c->hgc_base = 0;
            if ((c->hgc_enable >> 1) & (data >> 7))
                c->hgc_base = CGA_BASE;
            break;
        case 0x3D8:
            c->mode_ctrl_reg = data;
            break;
        case 0x3D9:
            c->color_ctrl_reg = data;
            break;
        case 0x3BF: // Enable HGC.
            // Set bit 0 to enable bit 1 of 3B8.
            // Set bit 1 to enable bit 7 of 3B8 and the second 32k of RAM ("Full" mode).
            c->hgc_enable = data & 3;
            break;
        }
}

static vxt_error install(vxt_system *s, struct vxt_pirepheral *p) {
    vxt_system_install_mem(s, p, MEMORY_START, (MEMORY_START + MEMORY_SIZE) - 1);
    vxt_system_install_io(s, p, 0x3B0, 0x3BF);
    vxt_system_install_io(s, p, 0x3D0, 0x3DF);
    return VXT_NO_ERROR;
}

static vxt_error destroy(struct vxt_pirepheral *p) {
    vxt_system_allocator(VXT_GET_SYSTEM(cga_video, p))(p, 0);
    return VXT_NO_ERROR;
}

static vxt_error reset(struct vxt_pirepheral *p) {
    VXT_DEC_DEVICE(c, cga_video, p);

    c->cursor_visible = true;
    c->cursor_offset = 0;
    c->is_dirty = true;

    c->mode_ctrl_reg = 1;
    c->color_ctrl_reg = 0x20;
    c->status_reg = 0;
    
    c->hgc_enable = 0;
    c->hgc_base = 0;
    c->hgc_mode = false;

    return VXT_NO_ERROR;
}

static const char *name(struct vxt_pirepheral *p) {
    (void)p;
    return "CGA/HGC Compatible Device";
}

static enum vxt_pclass pclass(struct vxt_pirepheral *p) {
    (void)p; return VXT_PCLASS_VIDEO;
}

static vxt_error step(struct vxt_pirepheral *p, int cycles) {
    (void)cycles;
    VXT_DEC_DEVICE(c, cga_video, p);

	INT64 t = c->ustics() * 1000ll;
	INT64 d = t - c->last_scanline;
	INT64 scanlines = d / SCANLINE_TIMING;

	if (scanlines > 0) {
		INT64 offset = d % SCANLINE_TIMING;
		c->last_scanline = t - offset;
        c->current_scanline = (c->current_scanline + (int)scanlines) % 525;
		if (c->current_scanline > 479)
			c->status_reg = 88;
		else
			c->status_reg = 0;
		c->status_reg |= 1;
	}
    return VXT_NO_ERROR;
}

struct vxt_pirepheral *vxtp_cga_create(vxt_allocator *alloc, INT64 (*ustics)(void)) {
    struct vxt_pirepheral *p = (struct vxt_pirepheral*)alloc(NULL, VXT_PIREPHERAL_SIZE(cga_video));
    vxt_memclear(p, VXT_PIREPHERAL_SIZE(cga_video));
    VXT_DEC_DEVICE(c, cga_video, p);

    c->ustics = ustics;
    c->application_start = ustics();

    p->install = &install;
    p->destroy = &destroy;
    p->name = &name;
    p->pclass = &pclass;
    p->reset = &reset;
    p->step = &step;
    p->io.read = &read;
    p->io.write = &write;
    p->io.in = &in;
    p->io.out = &out;
    return p;
}

static bool blink_tick(struct vxt_pirepheral *p) {
    VXT_DEC_DEVICE(c, cga_video, p);
    return (((c->ustics() - c->application_start) / 500000) % 2) == 0;
}

static void blit_char(struct vxt_pirepheral *p, vxt_byte ch, vxt_byte attr, int x, int y) {
    VXT_DEC_DEVICE(c, cga_video, p);
    struct snapshot *snap = &c->snap;

    int bg_color_index = (attr & 0x70) >> 4;
	int fg_color_index = attr & 0xF;

	if (attr & 0x80) {
		if (snap->mode_ctrl_reg & 0x20) {
			if (blink_tick(p))
				fg_color_index = bg_color_index;
		} else {
			// High intensity!
			bg_color_index += 8;
		}
	}

	vxt_dword bg_color = cga_palette[bg_color_index];
	vxt_dword fg_color = cga_palette[fg_color_index];
    int width = (snap->mode_ctrl_reg & 1) ? 640 : 320;

	for (int i = 0; i < 8; i++) {
		vxt_byte glyph_line = cga_font[(int)ch * 8 + i];
		for (int j = 0; j < 8; j++) {
			vxt_byte mask = 0x80 >> j;
			vxt_dword color = (glyph_line & mask) ? fg_color : bg_color;
		
			int offset = (width * (y + i) + x + j) * 4;
            snap->rgba_surface[offset++] = 0xFF;
            snap->rgba_surface[offset++] = (vxt_byte)(color & 0x0000FF);
            snap->rgba_surface[offset++] = (vxt_byte)((color & 0x00FF00) >> 8);
            snap->rgba_surface[offset] = (vxt_byte)((color & 0xFF0000) >> 16);    
		}
	}
}

vxt_dword vxtp_cga_border_color(struct vxt_pirepheral *p) {
    return cga_palette[(VXT_GET_DEVICE(cga_video, p))->color_ctrl_reg & 0xF];
}

bool vxtp_cga_snapshot(struct vxt_pirepheral *p) {
    VXT_DEC_DEVICE(c, cga_video, p);
    if (!c->is_dirty)
        return false;

    for (int i = 0; i < MEMORY_SIZE; i++)
        c->snap.mem[i] = c->mem[i];

    c->snap.cursor_visible = c->cursor_visible;
    c->snap.cursor_offset = c->cursor_offset;
    c->snap.mode_ctrl_reg = c->mode_ctrl_reg;
    c->snap.color_ctrl_reg = c->color_ctrl_reg;
    c->snap.video_page = ((int)c->crt_reg[0xC] << 8) + (int)c->crt_reg[0xD];

    c->is_dirty = false;
    return true;
}

int vxtp_cga_render(struct vxt_pirepheral *p, int (*f)(int,int,const vxt_byte*,void*), void *userdata) {
    VXT_DEC_DEVICE(c, cga_video, p);
    struct snapshot *snap = &c->snap;

    if (snap->hgc_mode) {
        return -1; // TODO
    } else if (snap->mode_ctrl_reg & 2) { // In CGA graphics mode?
        return -1; // TODO
    } else { // We are in text mode.
        int num_col = (snap->mode_ctrl_reg & 1) ? 80 : 40;
        int num_char = num_col * 25;

        for (int i = 0; i < num_char * 2; i += 2) {
            int idx = i / 2;
            int cell_offset = CGA_BASE + snap->video_page + i;
            vxt_byte ch = snap->mem[cell_offset];
            vxt_byte attr = snap->mem[cell_offset + 1];
            blit_char(p, ch, attr, (idx % num_col) * 8, (idx / num_col) * 8);
        }

        if (blink_tick(p) && snap->cursor_visible) {
            int x = snap->cursor_offset % num_col;
            int y = snap->cursor_offset / num_col;
            if (x < num_col && y < 25) {
                vxt_byte attr = (snap->mem[CGA_BASE + snap->video_page + (num_col * 2 * y + x * 2 + 1)] & 0x70) | 0xF;
                blit_char(p, '_', attr, x * 8, y * 8);
            }
        }
        return f(num_col * 8, 200, snap->rgba_surface, userdata);
    }
    return -1;
}
