// Copyright (c) 2019-2023 Andreas T Jonsson <mail@andreasjonsson.se>
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software in
//    a product, an acknowledgment (see the following) in the product
//    documentation is required.
//
//    Portions Copyright (c) 2019-2023 Andreas T Jonsson <mail@andreasjonsson.se>
//
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source distribution.

const c = @cImport(@cInclude("main.h"));
const std = @import("std");

export fn wasm_video_rgba_memory_pointer() [*]const u8 {
    return @ptrCast([*]const u8, c.video_rgba_memory_pointer());
}

export fn wasm_video_width() i32 {
    return @as(i32, c.video_width());
}

export fn wasm_video_height() i32 {
    return @as(i32, c.video_height());
}

export fn wasm_send_key(scan: i32) void {
    c.send_key(@as(c_int, scan));
}

export fn wasm_send_mouse(xrel: i32, yrel: i32, buttons: u32) void {
    c.send_mouse(@as(c_int, xrel), @as(c_int, yrel), @as(c_uint, buttons));
}

export fn wasm_step_emulation(cycles: i32) i32 {
    return c.step_emulation(@as(c_int, cycles));
}

export fn wasm_initialize_emulator() void {
    c.initialize_emulator();
}

// -------- Embedded files --------

const pcxtbios = @embedFile("../../bios/pcxtbios.bin");
const vxtx = @embedFile("../../bios/vxtx.bin");

export fn get_pcxtbios_data() [*]const u8 {
    return pcxtbios;
}

export fn get_pcxtbios_size() u32 {
    return pcxtbios.len;
}

export fn get_vxtx_data() [*]const u8 {
    return vxtx;
}

export fn get_vxtx_size() u32 {
    return vxtx.len;
}
