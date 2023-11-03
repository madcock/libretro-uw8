#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <wasm3.h>
#include <m3_env.h>

#include "loader.h"
#include "platform.h"
#include "libretro.h"

static retro_input_state_t input_state_cb;
static retro_input_poll_t input_poll_cb;
static retro_video_refresh_t video_cb;
static retro_environment_t environ_cb;
retro_audio_sample_t audio_cb;
#if defined(SF2000)
static retro_log_printf_t log_cb;
#endif

typedef struct {
	IM3Runtime runtime;
	wasm_rt_memory_t memory_c;
	Z_platform_instance_t platform_c;
	IM3Module cart;
} Uw8Runtime;

typedef struct AudioState {
	Uw8Runtime runtime;
	uint8_t* memory;
	IM3Function snd;
	bool hasSnd;
	uint8_t registers[32];
	uint32_t sampleIndex;
} AudioState;

typedef struct GameState {
	IM3Environment env;
	Uw8Runtime runtime;
	uint8_t* memory;
	uint8_t* initialMemory; // used for reset
	IM3Function updFunc;
	bool hasUpdFunc;
	uint32_t* pixels32;
	uint32_t frameNumber;
} GameState;

AudioState* audioState;
GameState* gameState;

#define MATH1(name) \
f32 Z_envZ_##name(struct Z_env_instance_t* i, f32 v) { \
	return name##f(v); \
}
#define MATH2(name) \
f32 Z_envZ_##name(struct Z_env_instance_t* i, f32 a, f32 b) { \
	return name##f(a, b); \
}
MATH1(acos); MATH1(asin); MATH1(atan); MATH2(atan2);
MATH1(cos); MATH1(sin); MATH1(tan);
MATH1(exp); MATH2(pow);
void Z_envZ_logChar(struct Z_env_instance_t* i, u32 c) {}

u32 reservedGlobal;
#define G_RESERVED(n) u32* Z_envZ_g_reserved##n(struct Z_env_instance_t* i) { return &reservedGlobal; }
G_RESERVED(0); G_RESERVED(1); G_RESERVED(2); G_RESERVED(3);
G_RESERVED(4); G_RESERVED(5); G_RESERVED(6); G_RESERVED(7);
G_RESERVED(8); G_RESERVED(9); G_RESERVED(10); G_RESERVED(11);
G_RESERVED(12); G_RESERVED(13); G_RESERVED(14); G_RESERVED(15);
wasm_rt_memory_t* Z_envZ_memory(struct Z_env_instance_t* i) { return (wasm_rt_memory_t*)i; }

void
retro_init(void)
{
#if defined(SF2000)
	struct retro_log_callback log;
	if(environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
		log_cb = log.log;
	else
		log_cb = NULL;
#endif
log_cb(RETRO_LOG_DEBUG, "before audioState = malloc(sizeof(AudioState));\n");
	audioState = malloc(sizeof(AudioState));
log_cb(RETRO_LOG_DEBUG, "before gameState = malloc(sizeof(GameState));\n");
	gameState = malloc(sizeof(GameState));
log_cb(RETRO_LOG_DEBUG, "after gameState = malloc(sizeof(GameState));\n");
}

void
retro_get_system_info(struct retro_system_info *info)
{
log_cb(RETRO_LOG_DEBUG, "retro_get_system_info()\n");
	memset(info, 0, sizeof(*info));
	info->library_name = "uw8";
	info->library_version = "0.2.2";
	info->need_fullpath = false;
	info->need_fullpath = true;	
	info->valid_extensions = "uw8|wasm";
log_cb(RETRO_LOG_DEBUG, "retro_get_system_info() done\n");
}

void
retro_get_system_av_info(struct retro_system_av_info *info)
{
log_cb(RETRO_LOG_DEBUG, "retro_get_system_av_info()\n");
	info->timing.fps = 60.0;
	info->timing.sample_rate = 44100.0;

	info->geometry.base_width = 320;
	info->geometry.base_height = 240;
	info->geometry.max_width = 320;
	info->geometry.max_height = 240;
	info->geometry.aspect_ratio = 4.0 / 3.0;
}

unsigned
retro_api_version(void)
{
log_cb(RETRO_LOG_DEBUG, "retro_api_version()\n");
	return RETRO_API_VERSION;
}

void
verifyM3(IM3Runtime runtime, M3Result result)
{
	if (result != m3Err_none)
	{
		M3ErrorInfo info;
		m3_GetErrorInfo(runtime, &info);
		fprintf(stderr, "WASM error: %s (%s)\n", result, info.message);
		exit(1);
	}
}

m3ApiRawFunction(math1)
{
	m3ApiReturnType(float);
	m3ApiGetArg(float, v);
	*raw_return = ((float(*)(float))_ctx->userdata)(v);
	m3ApiSuccess();
}

m3ApiRawFunction(math2)
{
	m3ApiReturnType(float);
	m3ApiGetArg(float, a);
	m3ApiGetArg(float, b);
	*raw_return = ((float(*)(float, float))_ctx->userdata)(a, b);
	m3ApiSuccess();
}

m3ApiRawFunction(nopFunc)
{
	m3ApiSuccess();
}

void
linkSystemFunctions(IM3Runtime runtime, IM3Module mod)
{
	m3_LinkRawFunctionEx(mod, "env", "acos", "f(f)", math1, acosf);
	m3_LinkRawFunctionEx(mod, "env", "asin", "f(f)", math1, asinf);
	m3_LinkRawFunctionEx(mod, "env", "atan", "f(f)", math1, atanf);
	m3_LinkRawFunctionEx(mod, "env", "atan2", "f(ff)", math2, atan2f);
	m3_LinkRawFunctionEx(mod, "env", "cos", "f(f)", math1, cosf);
	m3_LinkRawFunctionEx(mod, "env", "exp", "f(f)", math1, expf);
	m3_LinkRawFunctionEx(mod, "env", "log", "f(f)", math1, logf);
	m3_LinkRawFunctionEx(mod, "env", "sin", "f(f)", math1, sinf);
	m3_LinkRawFunctionEx(mod, "env", "tan", "f(f)", math1, tanf);
	m3_LinkRawFunctionEx(mod, "env", "pow", "f(ff)", math2, powf);

	m3_LinkRawFunction(mod, "env", "logChar", "v(i)", nopFunc);

	for(int i = 9; i < 64; ++i)
	{
		char name[128];
		sprintf(name, "reserved%d", i);
		m3_LinkRawFunction(mod, "env", name, "v()", nopFunc);
	}
}

m3ApiRawFunction(callFmod) {
	*(f32*)&_sp[0] = Z_platformZ_fmod((Z_platform_instance_t*)_ctx->userdata, *(f32*)&_sp[1], *(f32*)&_sp[2]);
	m3ApiSuccess();
}

m3ApiRawFunction(callRandom) {
	_sp[0] = Z_platformZ_random((Z_platform_instance_t*)_ctx->userdata);
	m3ApiSuccess();
}

m3ApiRawFunction(callRandomf) {
	*(f32*)&_sp[0] = Z_platformZ_randomf((Z_platform_instance_t*)_ctx->userdata);
	m3ApiSuccess();
}

m3ApiRawFunction(callRandomSeed) {
	Z_platformZ_randomSeed((Z_platform_instance_t*)_ctx->userdata, _sp[0]);
	m3ApiSuccess();
}

m3ApiRawFunction(callCls) {
	Z_platformZ_cls((Z_platform_instance_t*)_ctx->userdata, _sp[0]);
	m3ApiSuccess();
}

m3ApiRawFunction(callSetPixel) {
	Z_platformZ_setPixel((Z_platform_instance_t*)_ctx->userdata, _sp[0], _sp[1], _sp[2]);
	m3ApiSuccess();
}

m3ApiRawFunction(callGetPixel) {
	_sp[0] = Z_platformZ_getPixel((Z_platform_instance_t*)_ctx->userdata, _sp[1], _sp[2]);
	m3ApiSuccess();
}

m3ApiRawFunction(callHline) {
	Z_platformZ_hline((Z_platform_instance_t*)_ctx->userdata, _sp[0], _sp[1], _sp[2], _sp[3]);
	m3ApiSuccess();
}

m3ApiRawFunction(callRectangle) {
	Z_platformZ_rectangle((Z_platform_instance_t*)_ctx->userdata, *(f32*)&_sp[0], *(f32*)&_sp[1], *(f32*)&_sp[2], *(f32*)&_sp[3],_sp[4]);
	m3ApiSuccess();
}

m3ApiRawFunction(callCircle) {
	Z_platformZ_circle((Z_platform_instance_t*)_ctx->userdata, *(f32*)&_sp[0], *(f32*)&_sp[1], *(f32*)&_sp[2], _sp[3]);
	m3ApiSuccess();
}

m3ApiRawFunction(callRectangleOutline) {
	Z_platformZ_rectangleOutline((Z_platform_instance_t*)_ctx->userdata, *(f32*)&_sp[0], *(f32*)&_sp[1], *(f32*)&_sp[2], *(f32*)&_sp[3],_sp[4]);
	m3ApiSuccess();
}

m3ApiRawFunction(callCircleOutline) {
	Z_platformZ_circleOutline((Z_platform_instance_t*)_ctx->userdata, *(f32*)&_sp[0], *(f32*)&_sp[1], *(f32*)&_sp[2], _sp[3]);
	m3ApiSuccess();
}

m3ApiRawFunction(callLine) {
	Z_platformZ_line((Z_platform_instance_t*)_ctx->userdata, *(f32*)&_sp[0], *(f32*)&_sp[1], *(f32*)&_sp[2], *(f32*)&_sp[3],_sp[4]);
	m3ApiSuccess();
}

m3ApiRawFunction(callBlitSprite) {
	Z_platformZ_blitSprite((Z_platform_instance_t*)_ctx->userdata, _sp[0], _sp[1], _sp[2], _sp[3], _sp[4]);
	m3ApiSuccess();
}

m3ApiRawFunction(callGrabSprite) {
	Z_platformZ_grabSprite((Z_platform_instance_t*)_ctx->userdata, _sp[0], _sp[1], _sp[2], _sp[3], _sp[4]);
	m3ApiSuccess();
}

m3ApiRawFunction(callIsButtonPressed) {
	_sp[0] = Z_platformZ_isButtonPressed((Z_platform_instance_t*)_ctx->userdata, _sp[1]);
	m3ApiSuccess();
}

m3ApiRawFunction(callIsButtonTriggered) {
	_sp[0] = Z_platformZ_isButtonTriggered((Z_platform_instance_t*)_ctx->userdata, _sp[1]);
	m3ApiSuccess();
}

m3ApiRawFunction(callTime) {
	*(f32*)&_sp[0] = Z_platformZ_time((Z_platform_instance_t*)_ctx->userdata);
	m3ApiSuccess();
}

m3ApiRawFunction(callPrintChar) {
	Z_platformZ_printChar((Z_platform_instance_t*)_ctx->userdata, _sp[0]);
	m3ApiSuccess();
}

m3ApiRawFunction(callPrintString) {
	Z_platformZ_printString((Z_platform_instance_t*)_ctx->userdata, _sp[0]);
	m3ApiSuccess();
}

m3ApiRawFunction(callPrintInt) {
	Z_platformZ_printInt((Z_platform_instance_t*)_ctx->userdata, _sp[0]);
	m3ApiSuccess();
}

m3ApiRawFunction(callSetTextColor) {
	Z_platformZ_setTextColor((Z_platform_instance_t*)_ctx->userdata, _sp[0]);
	m3ApiSuccess();
}

m3ApiRawFunction(callSetBackgroundColor) {
	Z_platformZ_setBackgroundColor((Z_platform_instance_t*)_ctx->userdata, _sp[0]);
	m3ApiSuccess();
}

m3ApiRawFunction(callSetCursorPosition) {
	Z_platformZ_setCursorPosition((Z_platform_instance_t*)_ctx->userdata, _sp[0], _sp[1]);
	m3ApiSuccess();
}

m3ApiRawFunction(callSndGes) {
	*(f32*)&_sp[0] = Z_platformZ_sndGes((Z_platform_instance_t*)_ctx->userdata, _sp[1]);
	m3ApiSuccess();
}

m3ApiRawFunction(callPlayNote) {
	Z_platformZ_playNote((Z_platform_instance_t*)_ctx->userdata, _sp[0], _sp[1]);
	m3ApiSuccess();
}

struct {
	const char* name;
	const char* signature;
	M3RawCall function;
} cPlatformFunctions[] = {
	{ "fmod", "f(ff)", callFmod },
	{ "random", "i()", callRandom },
	{ "randomf", "f()", callRandomf },
	{ "randomSeed", "v(i)", callRandomSeed },
	{ "cls", "v(i)", callCls },
	{ "setPixel", "v(iii)", callSetPixel },
	{ "getPidel", "i(ii)", callGetPixel },
	{ "hline", "v(iiii)", callHline },
	{ "rectangle", "v(ffffi)", callRectangle },
	{ "circle", "v(fffi)", callCircle },
	{ "rectangleOutline", "v(ffffi)", callRectangleOutline },
	{ "circleOutline", "v(fffi)", callCircleOutline },
	{ "line", "v(ffffi)", callLine },
	{ "blitSprite", "v(iiiii)", callBlitSprite },
	{ "grabSprite", "v(iiiii)", callGrabSprite },
	{ "isButtonPressed", "i(i)", callIsButtonPressed },
	{ "isButtonTriggered", "i(i)", callIsButtonTriggered},
	{ "time", "f()", callTime },
	{ "printChar", "v(i)", callPrintChar },
	{ "printString", "v(i)", callPrintString },
	{ "printInt", "v(i)", callPrintInt },
	{ "setTextColor", "v(i)", callSetTextColor },
	{ "setBackgroundColor", "v(i)", callSetBackgroundColor },
	{ "setCursorPosition", "v(ii)", callSetCursorPosition },
	{ "playNote", "v(ii)", callPlayNote },
	{ "sndGes", "f(i)", callSndGes }
};

void
linkPlatformFunctions(IM3Runtime runtime, IM3Module cartMod, Z_platform_instance_t* platformInstance) {
	for(int i = 0; i * sizeof(cPlatformFunctions[0]) < sizeof(cPlatformFunctions); ++i) {
		m3_LinkRawFunctionEx(cartMod, "env", cPlatformFunctions[i].name, cPlatformFunctions[i].signature, cPlatformFunctions[i].function, platformInstance);
	}
}

void*
loadUw8(uint32_t* sizeOut, IM3Runtime runtime, const unsigned char* uw8, size_t uw8Size) {
	wasm_rt_memory_t memory;
	memory.data = m3_GetMemory(runtime, NULL, 0);
	memory.max_pages = memory.pages = 4;
	memory.size = 4 * 65536;
	Z_loader_instance_t loader;
	Z_loader_instantiate(&loader, (struct Z_env_instance_t*)&memory);
	
	memcpy(memory.data, uw8, uw8Size);
	*sizeOut = Z_loaderZ_load_uw8(&loader, (uint32_t)uw8Size);
	void* wasm = malloc(*sizeOut);
	memcpy(wasm, memory.data, *sizeOut);
	return wasm;
}

void
initRuntime(Uw8Runtime* runtime, IM3Environment env, void* cart, size_t cartSize) {
	runtime->runtime = m3_NewRuntime(env, 65536, NULL);
	runtime->runtime->memory.maxPages = 4;
	verifyM3(runtime->runtime, ResizeMemory(runtime->runtime, 4));

	runtime->memory_c.data = m3_GetMemory(runtime->runtime, NULL, 0);
	runtime->memory_c.max_pages = 4;
	runtime->memory_c.pages = 4;
	runtime->memory_c.size = 256*1024;
	Z_platform_instantiate(&runtime->platform_c, (struct Z_env_instance_t*)&runtime->memory_c);

	verifyM3(runtime->runtime, m3_ParseModule(env, &runtime->cart, cart, cartSize));
	runtime->cart->memoryImported = true;
	verifyM3(runtime->runtime, m3_LoadModule(runtime->runtime, runtime->cart));
	linkSystemFunctions(runtime->runtime, runtime->cart);
	linkPlatformFunctions(runtime->runtime, runtime->cart, &runtime->platform_c);
	verifyM3(runtime->runtime, m3_CompileModule(runtime->cart));
	verifyM3(runtime->runtime, m3_RunStart(runtime->cart));
}

bool
retro_load_game(const struct retro_game_info *game)
{
log_cb(RETRO_LOG_DEBUG, "retro_load_game()\n");
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
	if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
		return false;
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 1\n");
	gameState->pixels32 = malloc(320*240*4);
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 2\n");
	wasm_rt_init();
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 3\n");
	Z_loader_init_module();
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 4\n");
	Z_platform_init_module();
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 5\n");

	gameState->env = m3_NewEnvironment();
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 6\n");
	IM3Runtime loaderRuntime = m3_NewRuntime(gameState->env, 65536, NULL);
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 7\n");
	loaderRuntime->memory.maxPages = 4;
	verifyM3(loaderRuntime, ResizeMemory(loaderRuntime, 4));
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 8\n");
	uint32_t cartSize;
	void* cartWasm = loadUw8(&cartSize, loaderRuntime, game->data, game->size);

	m3_FreeRuntime(loaderRuntime);
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 9\n");
	initRuntime(&gameState->runtime, gameState->env, cartWasm, cartSize);
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 10\n");
	gameState->memory = m3_GetMemory(gameState->runtime.runtime, NULL, 0);
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 11\n");
	assert(gameState->memory != NULL);
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 12\n");
	gameState->hasUpdFunc = m3_FindFunction(&gameState->updFunc, gameState->runtime.runtime, "upd") == NULL;
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 13\n");
	initRuntime(&audioState->runtime, gameState->env, cartWasm, cartSize);
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 14\n");
	audioState->memory = m3_GetMemory(audioState->runtime.runtime, NULL, 0);
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 15\n");
	audioState->hasSnd = m3_FindFunction(&audioState->snd, audioState->runtime.runtime, "snd") == NULL;
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 16\n");
	memcpy(audioState->registers, audioState->memory + 0x50, 32);
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 17\n");
	audioState->sampleIndex = 0;

	gameState->initialMemory = malloc(1 << 18);
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 18\n");
	memcpy(gameState->initialMemory, gameState->memory, 1 << 18);
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 19\n");

	struct retro_input_descriptor desc[] = {
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "B" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Y" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "X" },

		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "B" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Y" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "X" },

		{ 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
		{ 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
		{ 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
		{ 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
		{ 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A" },
		{ 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "B" },
		{ 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Y" },
		{ 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "X" },

		{ 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
		{ 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
		{ 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
		{ 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
		{ 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A" },
		{ 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "B" },
		{ 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Y" },
		{ 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "X" },

		{ 0 },
	};
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 20\n");
	environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
log_cb(RETRO_LOG_DEBUG, "retro_load_game() 21\n");

	return true;
}

static const uint8_t retro_bind[] = {
	[RETRO_DEVICE_ID_JOYPAD_UP] = 1<<0,
	[RETRO_DEVICE_ID_JOYPAD_DOWN] = 1<<1,
	[RETRO_DEVICE_ID_JOYPAD_LEFT] = 1<<2,
	[RETRO_DEVICE_ID_JOYPAD_RIGHT] = 1<<3,
	[RETRO_DEVICE_ID_JOYPAD_B] = 1<<4,
	[RETRO_DEVICE_ID_JOYPAD_A] = 1<<5,
	[RETRO_DEVICE_ID_JOYPAD_Y] = 1<<6,
	[RETRO_DEVICE_ID_JOYPAD_X] = 1<<7,
};

void
retro_run(void)
{
log_cb(RETRO_LOG_DEBUG, "retro_run()\n");
	input_poll_cb();
log_cb(RETRO_LOG_DEBUG, "retro_run() 1\n");
	for(int p = 0; p < 4; p++) {
		gameState->memory[0x00044+p] = 0;
		for(int i = 0; i <= RETRO_DEVICE_ID_JOYPAD_R3; i++)
			if(input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, i))
				gameState->memory[0x00044+p] ^= retro_bind[i];
	}
log_cb(RETRO_LOG_DEBUG, "retro_run() 2\n");
	if(gameState->hasUpdFunc) {
		verifyM3(gameState->runtime.runtime, m3_CallV(gameState->updFunc));
	}
	memcpy(audioState->registers, gameState->memory + 0x50, 32);
log_cb(RETRO_LOG_DEBUG, "retro_run() 3\n");
	Z_platformZ_endFrame(&gameState->runtime.platform_c);
log_cb(RETRO_LOG_DEBUG, "retro_run() 4\n");
	uint32_t* palette = (uint32_t*)(gameState->memory + 0x13000);
	uint8_t* pixels = gameState->memory + 120;
	for(uint32_t i = 0; i < 320*240; ++i) {
		uint32_t c = palette[pixels[i]];
		gameState->pixels32[i] = (c & 0xff00ff00) | ((c & 0xff) << 16) | ((c >> 16) & 0xff);
	}
log_cb(RETRO_LOG_DEBUG, "retro_run() 5\n");
	video_cb(gameState->pixels32, 320, 240, 320*sizeof(uint32_t));
log_cb(RETRO_LOG_DEBUG, "retro_run() 6\n");
	memcpy(audioState->memory + 0x50, audioState->registers, 32);
	for(int i = 0; i < 44100/60; ++i) {
		float_t left, right;
		if(audioState->hasSnd) {
			m3_CallV(audioState->snd, audioState->sampleIndex++);
			m3_GetResultsV(audioState->snd, &left);
			m3_CallV(audioState->snd, audioState->sampleIndex++);
			m3_GetResultsV(audioState->snd, &right);
		} else {
			left = Z_platformZ_sndGes(&audioState->runtime.platform_c, audioState->sampleIndex++);
			right = Z_platformZ_sndGes(&audioState->runtime.platform_c, audioState->sampleIndex++);
		}
		audio_cb((int16_t)(left * 32767.0f), (int16_t)(right * 32767.0f));
	}
log_cb(RETRO_LOG_DEBUG, "retro_run() 7\n");
	*(uint32_t*)&gameState->memory[0x00040] = gameState->frameNumber++ * 1000 / 60 + 8;
}

void
retro_set_input_poll(retro_input_poll_t cb)
{
	input_poll_cb = cb;
}

void
retro_set_input_state(retro_input_state_t cb)
{
	input_state_cb = cb;
}

void
retro_set_video_refresh(retro_video_refresh_t cb)
{
	video_cb = cb;
}

void
retro_set_environment(retro_environment_t cb)
{
	environ_cb = cb;
}

void
retro_set_audio_sample(retro_audio_sample_t cb)
{
	audio_cb = cb;
}

void
retro_reset(void)
{
log_cb(RETRO_LOG_DEBUG, "retro_reset()\n");
	memcpy(gameState->memory, gameState->initialMemory, 1 << 18);
log_cb(RETRO_LOG_DEBUG, "retro_reset() 1\n");
	audioState->sampleIndex = 0;
log_cb(RETRO_LOG_DEBUG, "retro_reset() 2\n");
	gameState->frameNumber = 0;
log_cb(RETRO_LOG_DEBUG, "retro_reset() 3\n");
}

size_t
retro_serialize_size(void)
{
	return 1 << 18;
}

bool
retro_serialize(void *data, size_t size)
{
	memcpy(data, gameState->memory, 1 << 18);
log_cb(RETRO_LOG_DEBUG, "retro_serialize()\n");
	return true;
}

bool
retro_unserialize(const void *data, size_t size)
{
	memcpy(gameState->memory, data, 1 << 18);
log_cb(RETRO_LOG_DEBUG, "retro_unserialize()\n");
	return true;
}

void
retro_deinit(void) {
log_cb(RETRO_LOG_DEBUG, "retro_deinit()\n");
	m3_FreeRuntime(audioState->runtime.runtime);
	m3_FreeRuntime(gameState->runtime.runtime);
	m3_FreeEnvironment(gameState->env);
	free(gameState->initialMemory);
	free(gameState->pixels32);

	free(audioState);
	audioState = NULL;
	free(gameState);
	gameState = NULL;
}

unsigned
retro_get_region(void) {
	return RETRO_REGION_NTSC;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {}
size_t retro_get_memory_size(unsigned id) { return 0; }
void * retro_get_memory_data(unsigned id) { return NULL; }
void retro_unload_game(void) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {}
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code) {}
bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info) { return false; }
