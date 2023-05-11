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

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>

#define VXT_LIBC
#define VXTU_LIBC_IO
#include <vxt/vxt.h>
#include <vxt/vxtu.h>

#include <modules.h>
#include <frontend.h>

#include <SDL.h>
#include <ini.h>
#include <microui.h>

#include "mu_renderer.h"
#include "window.h"
#include "keys.h"
#include "docopt.h"
#include "icons.h"

#define CONFIG_FILE_NAME "config.ini"
#define MIN_CLOCKS_PER_STEP 1
#define MAX_PENALTY_USEC 1000

struct ini_config {
	struct DocoptArgs *args;
};
struct ini_config config = {0};

FILE *trace_op_output = NULL;
FILE *trace_offset_output = NULL;

#define SYNC(...) {								   	\
	if (SDL_LockMutex(emu_mutex) == -1)			   	\
		printf("sync error: %s\n", SDL_GetError());	\
	{ __VA_ARGS__ ; }							   	\
	if (SDL_UnlockMutex(emu_mutex) == -1)		   	\
		printf("sync error: %s\n", SDL_GetError());	\
}

#ifdef PI8088
	extern struct vxt_validator *pi8088_validator(void);
#endif

static const char button_map[256] = {
	[ SDL_BUTTON_LEFT   & 0xFF ] = MU_MOUSE_LEFT,
	[ SDL_BUTTON_RIGHT  & 0xFF ] = MU_MOUSE_RIGHT,
	[ SDL_BUTTON_MIDDLE & 0xFF ] = MU_MOUSE_MIDDLE,
};

static const char key_map[256] = {
	[ SDLK_LSHIFT       & 0xFF ] = MU_KEY_SHIFT,
	[ SDLK_RSHIFT       & 0xFF ] = MU_KEY_SHIFT,
	[ SDLK_LCTRL        & 0xFF ] = MU_KEY_CTRL,
	[ SDLK_RCTRL        & 0xFF ] = MU_KEY_CTRL,
	[ SDLK_LALT         & 0xFF ] = MU_KEY_ALT,
	[ SDLK_RALT         & 0xFF ] = MU_KEY_ALT,
	[ SDLK_RETURN       & 0xFF ] = MU_KEY_RETURN,
	[ SDLK_BACKSPACE    & 0xFF ] = MU_KEY_BACKSPACE,
};

Uint32 last_title_update = 0;
int num_cycles = 0;
double cpu_frequency = (double)VXT_DEFAULT_FREQUENCY / 1000000.0;
enum vxt_cpu_type cpu_type = VXT_CPU_8088;

int num_devices = 0;
struct vxt_pirepheral *devices[VXT_MAX_PIREPHERALS] = { NULL };
#define APPEND_DEVICE(d) { devices[num_devices++] = (d); }

SDL_atomic_t running = {1};
SDL_mutex *emu_mutex = NULL;
SDL_Thread *emu_thread = NULL;

SDL_Texture *framebuffer = NULL;
SDL_Point framebuffer_size = {640, 200};

char floppy_image_path[FILENAME_MAX] = {0};
char new_floppy_image_path[FILENAME_MAX] = {0};

const char *modules_search_path = NULL;

int str_buffer_len = 0;
char *str_buffer = NULL;

#define AUDIO_FREQUENCY 44100
#define AUDIO_LATENCY 10

SDL_AudioDeviceID audio_device = 0;
SDL_AudioSpec audio_spec = {0};

#define MAX_AUDIO_ADAPTERS 8
int num_audio_adapters = 0;
struct frontend_audio_adapter audio_adapters[MAX_AUDIO_ADAPTERS] = {0};

struct frontend_video_adapter video_adapter = {0};
struct frontend_disk_controller disk_controller = {0};
struct frontend_joystick_controller joystick_controller = {0};

struct frontend_interface front_interface = {0};

static void trigger_breakpoint(void) {
	SDL_TriggerBreakpoint();
}

static int text_width(mu_Font font, const char *text, int len) {
	(void)font;
	if (len == -1)
		len = strlen(text);
	return mr_get_text_width(text, len);
}

static int text_height(mu_Font font) {
	(void)font;
	return mr_get_text_height();
}

static const char *sprint(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	int size = vsnprintf(NULL, 0, fmt, args) + 1;
	va_end(args);

	if (str_buffer_len < size) {
		str_buffer_len = size;
		str_buffer = SDL_realloc(str_buffer, size);
	}

	va_start(args, fmt);
	vsnprintf(str_buffer, size, fmt, args);
	va_end(args);
	return str_buffer;
}

static const char *mgetline(void) {
	static char buffer[1024] = {0};
	char *str = fgets(buffer, sizeof(buffer), stdin);
	for (char *p = str; *p; p++) {
		if (*p == '\n') {
			*p = 0;
			break;
		}
	}
	return str;
}

static _Bool pdisasm(vxt_system *s, vxt_pointer start, int size, int lines) { // TODO: Change to vxt_bool type.
	#ifdef __APPLE__
		int fh;
		char name[128] = {0};
		strncpy(name, "disasm.XXXXXX", sizeof(name) - 1);
		fh = mkstemp(name);
		if (fh == -1)
			return false;
		FILE *tmpf = fdopen(fh, "wb");
	#else
		char *name = tmpnam(NULL);
		if (!name)
			return false;
		FILE *tmpf = fopen(name, "wb");
	#endif

	if (!tmpf)
		return false;

	for (int i = 0; i < size; i++) {
		vxt_byte v = vxt_system_read_byte(s, start + i);
		if (fwrite(&v, 1, 1, tmpf) != 1) {
			fclose(tmpf);
			remove(name);
			return false;
		}
	}
	fclose(tmpf);

	bool ret = system(sprint("ndisasm -i -b 16 -o %d \"%s\" | head -%d", start, name, lines)) == 0;
	remove(name);
	return ret;
}

static void tracer(vxt_system *s, vxt_pointer addr, vxt_byte data) {
	(void)s; (void)addr;
	fwrite(&data, 1, 1, trace_op_output);
	fwrite(&addr, sizeof(vxt_pointer), 1, trace_offset_output);
}

static int emu_loop(void *ptr) {
	vxt_system *vxt = (vxt_system*)ptr;
	Sint64 penalty = 0;
	double frequency = cpu_frequency;
	Uint64 start = SDL_GetPerformanceCounter();

	struct vxt_pirepheral *ppi = NULL;
	for (int i = 0; i < VXT_MAX_PIREPHERALS; i++) {
		ppi = vxt_system_pirepheral(vxt, i);
		if (ppi && (vxt_pirepheral_class(ppi) == VXT_PCLASS_PPI))
			break;
	}

	while (SDL_AtomicGet(&running)) {
		struct vxt_step res;
		SYNC(
			res = vxt_system_step(vxt, MIN_CLOCKS_PER_STEP);
			if (res.err != VXT_NO_ERROR) {
				if (res.err == VXT_USER_TERMINATION)
					SDL_AtomicSet(&running, 0);
				else
					printf("step error: %s", vxt_error_str(res.err));
			}
			num_cycles += res.cycles;
		);

		for (;;) {
			const Uint64 f = SDL_GetPerformanceFrequency() / (Uint64)(frequency * 1000000.0);
			if (!f) {
				penalty = 0;
				break;
			}

			const Sint64 max_penalty = (Sint64)(frequency * (double)MAX_PENALTY_USEC);
			const Sint64 c = (Sint64)((SDL_GetPerformanceCounter() - start) / f) + penalty;
			const Sint64 d = c - (Uint64)res.cycles;
			if (d >= 0) {
				penalty = (d > max_penalty) ? max_penalty : d;
				break;
			}
		}
		start = SDL_GetPerformanceCounter();
	}
	return 0;
}

static SDL_Rect *target_rect(SDL_Window *window, SDL_Rect *rect) {
	const float crtAspect = 4.0f / 3.0f;
	SDL_Rect windowRect = {0};
	SDL_GetWindowSize(window, &windowRect.w, &windowRect.h);

	int targetWidth = (int)((float)windowRect.h * crtAspect);
	int targetHeight = windowRect.h;

	if (((float)windowRect.w / (float)windowRect.h) < crtAspect) {
		targetWidth = windowRect.w;
		targetHeight = (int)((float)windowRect.w / crtAspect);
	}

	*rect = (SDL_Rect){windowRect.w / 2 - targetWidth / 2, windowRect.h / 2 - targetHeight / 2, targetWidth, targetHeight};
	return rect;
}

static int render_callback(int width, int height, const vxt_byte *rgba, void *userdata) {
	if ((framebuffer_size.x != width) || (framebuffer_size.y != height)) {
		SDL_DestroyTexture(framebuffer);
		framebuffer = SDL_CreateTexture(userdata, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height);
		if (!framebuffer) {
			printf("SDL_CreateTexture() failed with error %s\n", SDL_GetError());
			return -1;
		}
		framebuffer_size = (SDL_Point){width, height};
	}

	int status = SDL_UpdateTexture(framebuffer, NULL, rgba, width * 4);
	if (status != 0)
		printf("SDL_UpdateTexture() failed with error %s\n", SDL_GetError());
	return status;
}

static void audio_callback(void *udata, uint8_t *stream, int len) {
	(void)udata;
	len /= 2;
	
	SYNC(
		for (int i = 0; i < len; i++) {
			vxt_word sample = 0;
			for (int j = 0; j < num_audio_adapters; j++) {
				struct frontend_audio_adapter *a = &audio_adapters[j];
				sample += a->generate_sample(a->device, audio_spec.freq);
			}

			((vxt_int16*)stream)[i] = sample;
			if (audio_spec.channels > 1)
				((vxt_int16*)stream)[++i] = sample;
		}
	);
}

static void disk_activity_cb(int disk, void *data) {
	(void)disk;
	SDL_AtomicSet((SDL_atomic_t*)data, (int)SDL_GetTicks() + 0xFF);
}

static vxt_word pow2(vxt_word v) {
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	return ++v;
}

static int read_file(vxt_system *s, void *fp, vxt_byte *buffer, int size) {
	(void)s;
	return (int)fread(buffer, 1, (size_t)size, (FILE*)fp);
}

static int write_file(vxt_system *s, void *fp, vxt_byte *buffer, int size) {
	(void)s;
	return (int)fwrite(buffer, 1, (size_t)size, (FILE*)fp);
}

static int seek_file(vxt_system *s, void *fp, int offset, enum vxtu_disk_seek whence) {
	(void)s;
	switch (whence) {
		case VXTU_SEEK_START: return (int)fseek((FILE*)fp, (long)offset, SEEK_SET);
		case VXTU_SEEK_CURRENT: return (int)fseek((FILE*)fp, (long)offset, SEEK_CUR);
		case VXTU_SEEK_END: return (int)fseek((FILE*)fp, (long)offset, SEEK_END);
		default: return -1;
	}
}

static int tell_file(vxt_system *s, void *fp) {
	(void)s;
	return (int)ftell((FILE*)fp);
}

static vxt_byte emu_control(enum frontend_ctrl_command cmd, void *userdata) {
	(void)userdata;
	if (cmd == FRONTEND_CTRL_SHUTDOWN) {
		printf("Guest OS shutdown!\n");
		SDL_AtomicSet(&running, 0);
	}
	return 0;
}

static bool set_audio_adapter(const struct frontend_audio_adapter *adapter) {
	if (num_audio_adapters >= MAX_AUDIO_ADAPTERS)
		return false;

	printf("Setup audio pirepheral: %s\n", vxt_pirepheral_name(adapter->device));
	memcpy(&audio_adapters[num_audio_adapters++], adapter, sizeof(struct frontend_audio_adapter));
	return true;
}

static bool set_video_adapter(const struct frontend_video_adapter *adapter) {
	if (video_adapter.device)
		return false;

	printf("Setup video pirepheral: %s\n", vxt_pirepheral_name(adapter->device));
	video_adapter = *adapter;
	return true;
}

static bool set_disk_controller(const struct frontend_disk_controller *controller) {
	if (disk_controller.device)
		return false;

	printf("Setup disk controller: %s\n", vxt_pirepheral_name(controller->device));
	disk_controller = *controller;
	return true;
}

static bool set_joystick_controller(const struct frontend_joystick_controller *controller) {
	if (joystick_controller.device)
		return false;

	printf("Setup joystick controller: %s\n", vxt_pirepheral_name(controller->device));
	joystick_controller = *controller;
	return true;
}

static struct vxt_pirepheral *load_bios(const char *path, vxt_pointer base) {
	size_t path_len = strcspn(path, "@");
	char *file_path = (char*)SDL_malloc(path_len + 1);
	memcpy(file_path, path, path_len);
	file_path[path_len] = 0;
	
	int size = 0;
	vxt_byte *data = vxtu_read_file(&realloc, file_path, &size);
	SDL_free(file_path);

	if (!data) {
		printf("vxtu_read_file() failed!\n");
		return NULL;
	}

	if (path_len != strlen(path)) {
		unsigned int addr = 0;
		if (sscanf(path + path_len + 1, "%x", &addr) == 1) {
			base = (vxt_pointer)addr;
		} else {
			printf("Invalid address format!\n");
			return NULL;
		}
	}
	
	struct vxt_pirepheral *rom = vxtu_memory_create(&realloc, base, size, true);
	if (!vxtu_memory_device_fill(rom, data, size)) {
		printf("vxtu_memory_device_fill() failed!\n");
		return NULL;
	}
	(void)realloc(data, 0);

	printf("Loaded BIOS @ 0x%X-0x%X: %s\n", base, base + size - 1, path);
	return rom;
}

static int load_config(void *user, const char *section, const char *name, const char *value) {
	(void)user; (void)section; (void)name; (void)value;
	struct ini_config *config = (struct ini_config*)user;
	if (!strcmp("args", section)) {
		if (!strcmp("debug", name))
			config->args->debug |= atoi(value);
		else if (!strcmp("halt", name))
			config->args->halt |= atoi(value);
		else if (!strcmp("no-mouse", name))
			config->args->no_mouse |= atoi(value);
		else if (!strcmp("no-cga", name))
			config->args->no_cga |= atoi(value);
		else if (!strcmp("no-disk", name))
			config->args->no_disk |= atoi(value);
		else if (!strcmp("hdboot", name))
			config->args->hdboot |= atoi(value);
		else if (!strcmp("mute", name))
			config->args->mute |= atoi(value);
		else if (!strcmp("v20", name))
			config->args->v20 |= atoi(value);
		else if (!strcmp("no-activity", name))
			config->args->no_activity |= atoi(value);
		else if (!strcmp("bios", name) && !config->args->bios) {
			static char bios_image_path[FILENAME_MAX] = {0};
			strncpy(bios_image_path, value, FILENAME_MAX - 1);
			config->args->bios = bios_image_path;
		} else if (!strcmp("harddrive", name) && !config->args->harddrive) {
			static char harddrive_image_path[FILENAME_MAX] = {0};
			strncpy(harddrive_image_path, value, FILENAME_MAX - 1);
			config->args->harddrive = harddrive_image_path;
		}
	}
	return 1;
}

static int load_modules(void *user, const char *section, const char *name, const char *value) {
	if (!strcmp("modules", section)) {
		for (
			int i = 0;
			#ifdef VXTU_STATIC_MODULES
				true;
			#else
				i < 1;
			#endif
			i++
		) {
			vxtu_module_entry_func *(*constructors)(int(*)(const char*, ...)) = NULL;

			#ifdef VXTU_STATIC_MODULES
				const struct vxtu_module_entry *e = &vxtu_module_table[i];
				if (!e->name)
					break;
				else if (strcmp(e->name, name))
					continue;

				constructors = e->entry;
			#else
				char buffer[FILENAME_MAX];
				sprintf(buffer, "%s/%s-module."
					#ifdef _WIN32
						"dll"
					#else
						"so"
					#endif
					, modules_search_path, name);

				void *lib = SDL_LoadObject(buffer);
				if (!lib) {
					printf("ERROR: Could not load module: %s\n", name);
					printf("ERROR: %s\n", SDL_GetError());
					continue;
				}

				sprintf(buffer, "_vxtu_module_%s_entry", name);
				constructors = SDL_LoadFunction(lib, buffer);

				if (!constructors) {
					printf("ERROR: Could not load module entry: %s\n", SDL_GetError());
					SDL_UnloadObject(lib);
					continue;
				}
			#endif

			vxtu_module_entry_func *const_func = constructors(vxt_logger());
			if (!const_func) {
				printf("ERROR: Module '%s' does not return entries!\n", name);
				continue;
			}

			for (vxtu_module_entry_func *f = const_func; *f; f++) {
				struct vxt_pirepheral *p = (*f)((vxt_allocator*)user, (void*)&front_interface, value);
				if (!p)
					continue; // Assume the module chose not to be loaded.
				APPEND_DEVICE(p);
			}

			printf("%d - %s", i + 1, name);
			if (*value) printf(" = %s\n", value);
			else putc('\n', stdout);
		}
	}
	return 1;
}

static int configure_pirepherals(void *user, const char *section, const char *name, const char *value) {
	return (vxt_system_configure(user, section, name, value) == VXT_NO_ERROR) ? 1 : 0;
}

static void write_default_config(const char *path, bool clean) {
	FILE *fp;
	if (!clean && (fp = fopen(path, "r"))) {
		fclose(fp);
		return;
	}

	printf("WARNING: No config file found. Creating new default: %s\n", path);
	if (!(fp = fopen(path, "w"))) {
		printf("ERROR: Could not create config file: %s\n", path);
		return;
	}

	fprintf(fp,
		"[modules]\n"
		"adlib=\n"
		"rifs=\n"
		"ctrl=\n"
		"joystick=0x201\n"
		";ems=lotech_ems\n"
		";vga=bios/et4000.bin\n"
		";fdc=\n"
		";rtc=\n"
		";network=eth0\n"
		";isa=ch36x\n"
		";serial_dbg=sdbg1\n"
		"\n[args]\n"
		";bios=bios/pcxtbios_640.bin\n"
		";no-cga=1\n"
		";no-disk=1\n"
		";v20=1\n"
		";debug=1\n"
		";hdboot=1\n"
		";harddrive=boot/freedos_hd.img\n"
		"\n[sdbg1]\n"
		"port=0x3F8\n"
		"\n[ch36x]\n"
		"device=/dev/ch36xpci0\n"
		"port=0x201\n"
		"\n[lotech_ems]\n"
		"memory=0xD0000\n"
		"port=0x260\n"
		"\n[rifs]\n"
		"port=0x178\n"
	);
	fclose(fp);
}

int main(int argc, char *argv[]) {
	// This is a hack because there seems to be a bug in DocOpt
	// that prevents us from adding trailing parameters.
	char *rifs_path = NULL;
	if ((argc == 2) && (*argv[1] != '-')) {
		rifs_path = argv[1];
		argc = 1;
	}

	struct DocoptArgs args = docopt(argc, argv, true, vxt_lib_version());
	args.rifs = rifs_path ? rifs_path : args.rifs;

	if (!args.config) {
		args.config = SDL_GetPrefPath("virtualxt", "VirtualXT-SDL2-" VXT_VERSION);
		if (!args.config) {
			printf("No config path!\n");
			return -1;
		}
	}
	printf("Config path: %s\n", args.config);

	const char *base_path = SDL_GetBasePath();
	base_path = base_path ? base_path : "./";
	printf("Base path: %s\n", base_path);

	{
		const char *path = sprint("%s/" CONFIG_FILE_NAME, args.config);
		write_default_config(path, args.clean != 0);

		config.args = &args;
		if (ini_parse(path, &load_config, &config)) {
			printf("Can't open, or parse: %s\n", path);
			return -1;
		}
	}

	if (!args.modules) {
		args.modules = SDL_getenv("VXT_DEFAULT_MODULES_PATH");
		if (!args.modules) args.modules = "modules";
	}
	modules_search_path = args.modules;

	if (!args.bios) {
		args.bios = SDL_getenv("VXT_DEFAULT_BIOS_PATH");
		if (!args.bios) args.bios = "bios/pcxtbios.bin";
	}

	if (!args.extension) {
		args.extension = SDL_getenv("VXT_DEFAULT_VXTX_BIOS_PATH");
		if (!args.extension) args.extension = "bios/vxtx.bin";
	}

	if (!args.harddrive && !args.floppy) {
		args.harddrive = SDL_getenv("VXT_DEFAULT_HD_IMAGE");
		if (!args.harddrive) args.harddrive = "boot/freedos_hd.img";
	}

	args.debug |= args.halt;
	if (args.debug)
		printf("Internal debugger enabled!\n");

	if (args.v20) {
		cpu_type = VXT_CPU_V20;
		#ifdef VXT_CPU_286
			printf("CPU type: 286\n");
		#else
			printf("CPU type: V20\n");
		#endif
	} else {
		printf("CPU type: 8088\n");
	}

	if (args.frequency)
		cpu_frequency = strtod(args.frequency, NULL);
	printf("CPU frequency: %.2f MHz\n", cpu_frequency);

	#if !defined(_WIN32) && !defined(__APPLE__)
		SDL_setenv("SDL_VIDEODRIVER", "x11", 1);
	#endif

	if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
		printf("SDL_Init() failed with error %s\n", SDL_GetError());
		return -1;
	}

	SDL_SetHint(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, "1");
	SDL_Window *window = SDL_CreateWindow(
			"VirtualXT", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			640, 480, SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE
		);

	if (!window) {
		printf("SDL_CreateWindow() failed with error %s\n", SDL_GetError());
		return -1;
	}

	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
	SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
	if (!renderer) {
		printf("SDL_CreateRenderer() failed with error %s\n", SDL_GetError());
		return -1;
	}

	framebuffer = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, framebuffer_size.x, framebuffer_size.y);
	if (!framebuffer) {
		printf("SDL_CreateTexture() failed with error %s\n", SDL_GetError());
		return -1;
	}

	SDL_RWops *rwops = SDL_RWFromConstMem(disk_activity_icon, sizeof(disk_activity_icon));
	SDL_Surface *surface = SDL_LoadBMP_RW(rwops, 1);
	if (!surface) {
		printf("SDL_LoadBMP_RW() failed with error %s\n", SDL_GetError());
		return -1;
	}

	SDL_Texture *disk_icon_texture = SDL_CreateTextureFromSurface(renderer, surface);
	if (!disk_icon_texture) {
		printf("SDL_CreateTextureFromSurface() failed with error %s\n", SDL_GetError());
		return -1;
	}
	SDL_FreeSurface(surface);

	mr_renderer *mr = mr_init(renderer);
	mu_Context *ctx = SDL_malloc(sizeof(mu_Context));
	mu_init(ctx);
	ctx->text_width = text_width;
	ctx->text_height = text_height;

	int num_sticks = 0;
	SDL_Joystick *sticks[2] = {NULL};
	
	printf("Initialize joysticks:\n");
	if (!(num_sticks = SDL_NumJoysticks())) {
		printf("No joystick found!\n");
	} else {
		for (int i = 0; i < num_sticks; i++) {
			const char *name = SDL_JoystickNameForIndex(i);
			if (!name) name = "Unknown Joystick";

			if ((i < 2) && (sticks[i] = SDL_JoystickOpen(i))) {
				printf("%d - *%s\n", i + 1, name);
				continue;
			}
			printf("%d - %s\n", i + 1, name);
		}
	}

	vxt_set_logger(&printf);
	vxt_set_breakpoint(&trigger_breakpoint);

	struct vxt_pirepheral *dbg = NULL;
	if (args.debug) {
		struct vxtu_debugger_interface dbgif = {&pdisasm, &mgetline, &printf};
		dbg = vxtu_debugger_create(&realloc, &dbgif);
	}

	struct vxt_pirepheral *rom = load_bios(args.bios, 0xFE000);
	if (!rom) return -1;

	struct vxt_pirepheral *ppi = vxtu_ppi_create(&realloc);
	struct vxt_pirepheral *mouse = args.no_mouse ? NULL : vxtu_mouse_create(&realloc, 0x3F8, 4); // COM1

	APPEND_DEVICE(vxtu_memory_create(&realloc, 0x0, 0x100000, false));
	APPEND_DEVICE(rom);

	struct vxtu_disk_interface disk_interface = {
		&read_file, &write_file, &seek_file, &tell_file
	};

	if (!args.no_disk) {
		struct vxt_pirepheral *rom = load_bios(args.extension, 0xE0000);
		if (!rom)
			return -1;

		struct frontend_disk_controller c = {
			.device = vxtu_disk_create(&realloc, &disk_interface),
			.mount = &vxtu_disk_mount,
			.unmount = &vxtu_disk_unmount,
			.set_boot = &vxtu_disk_set_boot_drive
		};
		set_disk_controller(&c);

		APPEND_DEVICE(rom);
		APPEND_DEVICE(disk_controller.device);
	}

	if (!args.no_mouse)
		APPEND_DEVICE(mouse);

	APPEND_DEVICE(vxtu_pic_create(&realloc));
	APPEND_DEVICE(vxtu_dma_create(&realloc));
	APPEND_DEVICE(vxtu_pit_create(&realloc));
	APPEND_DEVICE(ppi);

	front_interface.interface_version = FRONTEND_INTERFACE_VERSION;
	front_interface.set_video_adapter = &set_video_adapter;
	front_interface.set_audio_adapter = &set_audio_adapter;
	front_interface.set_disk_controller = &set_disk_controller;
	front_interface.set_joystick_controller = &set_joystick_controller;
	front_interface.ctrl.callback = &emu_control;
	front_interface.disk.di = disk_interface;

	SDL_atomic_t icon_fade = {0};
	if (!args.no_activity) {
		front_interface.disk.activity_callback = &disk_activity_cb;
		front_interface.disk.userdata = &icon_fade;
	}

	if (args.no_cga) {
		vxtu_ppi_set_xt_switches(ppi, 0);
	} else {
		struct frontend_video_adapter a = {
			vxtu_cga_create(&realloc),
			&vxtu_cga_border_color,
			&vxtu_cga_snapshot,
			&vxtu_cga_render
		};
		set_video_adapter(&a);
		APPEND_DEVICE(a.device);
	}

	#ifdef VXTU_STATIC_MODULES
		printf("Modules are staticlly linked!\n");
	#endif
	printf("Loaded modules:\n");

	if (!args.no_modules && ini_parse(sprint("%s/" CONFIG_FILE_NAME, args.config), &load_modules, (void*)&realloc)) {
		printf("ERROR: Could not load all modules!\n");
		return -1;
	}

	APPEND_DEVICE(dbg);

	vxt_system *vxt = vxt_system_create(&realloc, cpu_type, (int)(cpu_frequency * 1000000.0), devices);
	if (!vxt) {
		printf("Could not create system!\n");
		return -1;
	}

	if (ini_parse(sprint("%s/" CONFIG_FILE_NAME, args.config), &configure_pirepherals, vxt)) {
		printf("ERROR: Could not configure all pirepherals!\n");
		return -1;
	}

	#ifdef VXTU_MODULE_RIFS
		if (args.rifs) {
			bool wr = *args.rifs == '*';
			const char *root = wr ? &args.rifs[1] : args.rifs;
			vxt_system_configure(vxt, "rifs", "writable", wr ? "1" : "0");
			vxt_system_configure(vxt, "rifs", "root", root);
		}
	#endif

	if (args.trace) {
		if (!(trace_op_output = fopen(args.trace, "wb"))) {
			printf("Could not open: %s\n", args.trace);
			return -1;
		}
		static char buffer[512] = {0};
		snprintf(buffer, sizeof(buffer), "%s.offset", args.trace);
		if (!(trace_offset_output = fopen(buffer, "wb"))) {
			printf("Could not open: %s\n", buffer);
			return -1;
		}
		vxt_system_set_tracer(vxt, &tracer);
	}

	#ifdef PI8088
		vxt_system_set_validator(vxt, pi8088_validator());
	#endif

	vxt_error err = vxt_system_initialize(vxt);
	if (err != VXT_NO_ERROR) {
		printf("vxt_system_initialize() failed with error %s\n", vxt_error_str(err));
		return -1;
	}

	struct frontend_audio_adapter ppi_audio = { ppi, &vxtu_ppi_generate_sample };
	set_audio_adapter(&ppi_audio);

	printf("Installed pirepherals:\n");
	for (int i = 1; i < VXT_MAX_PIREPHERALS; i++) {
		struct vxt_pirepheral *device = vxt_system_pirepheral(vxt, (vxt_byte)i);
		if (device)
			printf("%d - %s\n", i, vxt_pirepheral_name(device));
	}

	if (args.floppy) {
		strncpy(floppy_image_path, args.floppy, sizeof(floppy_image_path) - 1);

		FILE *fp = fopen(floppy_image_path, "rb+");
		if (fp && (disk_controller.mount(disk_controller.device, 0, fp) == VXT_NO_ERROR))
			printf("Floppy image: %s\n", floppy_image_path);
	}

	if (args.harddrive) {
		FILE *fp = fopen(args.harddrive, "rb+");
		if (fp && (disk_controller.mount(disk_controller.device, 128, fp) == VXT_NO_ERROR)) {
			printf("Harddrive image: %s\n", args.harddrive);
			if (args.hdboot || !args.floppy)
				disk_controller.set_boot(disk_controller.device, 128);
		}
	}

	vxt_system_reset(vxt);
	vxt_system_registers(vxt)->debug = (bool)args.halt;

	if (!(emu_mutex = SDL_CreateMutex())) {
		printf("SDL_CreateMutex failed!\n");
		return -1;
	}

	if (!(emu_thread = SDL_CreateThread(&emu_loop, "emulator loop", vxt))) {
		printf("SDL_CreateThread failed!\n");
		return -1;
	}

	if (args.mute) {
		printf("Audio is muted!\n");
	} else {
		SDL_AudioSpec spec = {0};
		spec.freq = AUDIO_FREQUENCY;
		spec.format = AUDIO_S16;
		spec.channels = 1;
		spec.samples = pow2((AUDIO_FREQUENCY / 1000) * AUDIO_LATENCY);
		spec.callback = &audio_callback;

		int allowed_changes = SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE;

		if (!(audio_device = SDL_OpenAudioDevice(NULL, false, &spec, &audio_spec, allowed_changes))) {
			printf("SDL_OpenAudioDevice() failed with error %s\n", SDL_GetError());
			return -1;
		}
		SDL_PauseAudioDevice(audio_device, 0);
	}

	while (SDL_AtomicGet(&running)) {
		for (SDL_Event e; SDL_PollEvent(&e);) {
			if (has_open_windows) {
				switch (e.type) {
					case SDL_MOUSEMOTION: mu_input_mousemove(ctx, e.motion.x, e.motion.y); break;
					case SDL_MOUSEWHEEL: mu_input_scroll(ctx, 0, e.wheel.y * -30); break;
					case SDL_TEXTINPUT: mu_input_text(ctx, e.text.text); break;
					case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
						int b = button_map[e.button.button & 0xFF];
						if (b && e.type == SDL_MOUSEBUTTONDOWN) { mu_input_mousedown(ctx, e.button.x, e.button.y, b); }
						if (b && e.type == SDL_MOUSEBUTTONUP) { mu_input_mouseup(ctx, e.button.x, e.button.y, b); }
						break;
					}
					case SDL_KEYDOWN: case SDL_KEYUP: {
						int c = key_map[e.key.keysym.sym & 0xFF];
						if (c && e.type == SDL_KEYDOWN) { mu_input_keydown(ctx, c); }
						if (c && e.type == SDL_KEYUP) { mu_input_keyup(ctx, c); }
						break;
					}
				}
			}

			switch (e.type) {
				case SDL_QUIT:
					SDL_AtomicSet(&running, 0);
					break;
				case SDL_MOUSEMOTION:
					if (mouse && SDL_GetRelativeMouseMode() && !has_open_windows) {
						Uint32 state = SDL_GetMouseState(NULL, NULL);
						struct vxtu_mouse_event ev = {0, e.motion.xrel, e.motion.yrel};
						if (state & SDL_BUTTON_LMASK)
							ev.buttons |= VXTU_MOUSE_LEFT;
						if (state & SDL_BUTTON_RMASK)
							ev.buttons |= VXTU_MOUSE_RIGHT;
						SYNC(vxtu_mouse_push_event(mouse, &ev));
					}
					break;
				case SDL_MOUSEBUTTONDOWN:
					if (mouse && !has_open_windows) {
						if (e.button.button == SDL_BUTTON_MIDDLE) {
							SDL_SetRelativeMouseMode(false);
							break;
						}
						SDL_SetRelativeMouseMode(true);

						struct vxtu_mouse_event ev = {0};
						if (e.button.button == SDL_BUTTON_LEFT)
							ev.buttons |= VXTU_MOUSE_LEFT;
						if (e.button.button == SDL_BUTTON_RIGHT)
							ev.buttons |= VXTU_MOUSE_RIGHT;
						SYNC(vxtu_mouse_push_event(mouse, &ev));
					}
					break;
				case SDL_DROPFILE:
					strncpy(new_floppy_image_path, e.drop.file, sizeof(new_floppy_image_path) - 1);
					open_window(ctx, "Mount");
					break;
				case SDL_JOYAXISMOTION:
				case SDL_JOYBUTTONDOWN:
				case SDL_JOYBUTTONUP:
				{
					assert(e.jaxis.which == e.jbutton.which);
					SDL_Joystick *js = SDL_JoystickFromInstanceID(e.jaxis.which);
					if (js && ((sticks[0] == js) || (sticks[1] == js))) {
						struct frontend_joystick_event ev = {
							(sticks[0] == js) ? FRONTEND_JOYSTICK_1 : FRONTEND_JOYSTICK_2,
							(SDL_JoystickGetButton(js, 0) ? FRONTEND_JOYSTICK_A : 0) | (SDL_JoystickGetButton(js, 1) ? FRONTEND_JOYSTICK_B : 0),
							SDL_JoystickGetAxis(js, 0),
							SDL_JoystickGetAxis(js, 1)
						};
						if (joystick_controller.device) SYNC(
							joystick_controller.push_event(joystick_controller.device, &ev)
						)
					}
					break;
				}
				case SDL_KEYDOWN:
					if (!has_open_windows && (e.key.keysym.sym != SDLK_F11) && (e.key.keysym.sym != SDLK_F12))
						SYNC(vxtu_ppi_key_event(ppi, sdl_to_xt_scan(e.key.keysym.scancode), false));
					break;
				case SDL_KEYUP:
					if (e.key.keysym.sym == SDLK_F11) {
						if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) {
							SDL_SetWindowFullscreen(window, 0);
							SDL_SetRelativeMouseMode(false);
						} else {
							SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
							SDL_SetRelativeMouseMode(true);
						}
						break;
					} else if (e.key.keysym.sym == SDLK_F12) {
						if (args.debug && (e.key.keysym.mod & KMOD_ALT)) {
							SDL_SetWindowFullscreen(window, 0);
							SDL_SetRelativeMouseMode(false);
							SYNC(vxtu_debugger_interrupt(dbg));					
						} else if ((e.key.keysym.mod & KMOD_CTRL)) {
							open_window(ctx, "Eject");
						} else {
							open_window(ctx, "Help");
						}
						break;
					}

					if (!has_open_windows)
						SYNC(vxtu_ppi_key_event(ppi, sdl_to_xt_scan(e.key.keysym.scancode) | VXTU_KEY_UP_MASK, false));
					break;
			}
		}

		// Update titlebar.
		Uint32 ticks = SDL_GetTicks();
		if ((ticks - last_title_update) > 500) {
			last_title_update = ticks;

			char buffer[100];
			double mhz;

			SYNC(
				mhz = (double)num_cycles / 500000.0;
				num_cycles = 0;
			);

			const char *name = "8088";
			if (cpu_type == VXT_CPU_V20) {
				#ifdef VXT_CPU_286
					name = "286";
				#else
					name = "V20";
				#endif
			}
			
			if (ticks > 10000) {
				snprintf(buffer, sizeof(buffer), "VirtualXT - %s@%.2f MHz", name, mhz);
			} else {
				snprintf(buffer, sizeof(buffer), "VirtualXT - <Press F12 for help>");
			}
			SDL_SetWindowTitle(window, buffer);
		}

		{ // Update all windows and there functions.
			has_open_windows = false;
			mu_begin(ctx);

			help_window(ctx);
			error_window(ctx);

			if (eject_window(ctx, (*floppy_image_path != 0) ? floppy_image_path: NULL)) {
				SYNC(disk_controller.unmount(disk_controller.device, 0));
				*floppy_image_path = 0;
			}

			if (mount_window(ctx, new_floppy_image_path)) {
				FILE *fp = fopen(new_floppy_image_path, "rb+");
				if (!fp) {
					open_error_window(ctx, "Could not open floppy image file!");
				} else {
					vxt_error err = VXT_NO_ERROR;
					SYNC(err = disk_controller.mount(disk_controller.device, 0, fp));
					if (err != VXT_NO_ERROR) {
						open_error_window(ctx, "Could not mount floppy image file!");
						fclose(fp);
					} else {
						strncpy(floppy_image_path, new_floppy_image_path, sizeof(floppy_image_path));
					}
				}
			}

			mu_end(ctx);
			if (has_open_windows)
				SDL_SetRelativeMouseMode(false);
		}

		{ // Final rendering.
			vxt_dword bg = 0;
			SDL_Rect rect = {0};

			SYNC(
				bg = video_adapter.border_color(video_adapter.device);
				video_adapter.snapshot(video_adapter.device);
			);
			video_adapter.render(video_adapter.device, &render_callback, renderer);

			mr_clear(mr, mu_color(bg & 0x0000FF, (bg & 0x00FF00) >> 8, (bg & 0xFF0000) >> 16, 0xFF));
			if (SDL_RenderCopy(renderer, framebuffer, NULL, target_rect(window, &rect)) != 0)
				printf("SDL_RenderCopy() failed with error %s\n", SDL_GetError());

			mu_Command *cmd = NULL;
			while (mu_next_command(ctx, &cmd)) {
				switch (cmd->type) {
					case MU_COMMAND_TEXT: mr_draw_text(mr, cmd->text.str, cmd->text.pos, cmd->text.color); break;
					case MU_COMMAND_RECT: mr_draw_rect(mr, cmd->rect.rect, cmd->rect.color); break;
					case MU_COMMAND_ICON: mr_draw_icon(mr, cmd->icon.id, cmd->icon.rect, cmd->icon.color); break;
					case MU_COMMAND_CLIP: mr_set_clip_rect(mr, cmd->clip.rect); break;
				}
			}

			int fade = SDL_AtomicGet(&icon_fade) - (int)SDL_GetTicks();
			if (fade > 0) {
				SDL_Rect dst = { 4, 2, 20, 20 };
				SDL_SetTextureAlphaMod(disk_icon_texture, (fade > 0xFF) ? 0xFF : fade);
				SDL_RenderCopy(renderer, disk_icon_texture, NULL, &dst);
			}

			mr_present(mr);
		}
	}

	SDL_WaitThread(emu_thread, NULL);

	if (audio_device)
		SDL_CloseAudioDevice(audio_device);

	SDL_DestroyMutex(emu_mutex);

	vxt_system_destroy(vxt);

	if (trace_op_output)
		fclose(trace_op_output);
	if (trace_offset_output)
		fclose(trace_offset_output);

	if (str_buffer)
		SDL_free(str_buffer);

	for (int i = 0; i < 2; i++) {
		if (sticks[i])
			SDL_JoystickClose(sticks[i]);
	}

	mr_destroy(mr);
	SDL_free(ctx);

	SDL_DestroyTexture(disk_icon_texture); 
	SDL_DestroyTexture(framebuffer);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
