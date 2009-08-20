extern "C" {
#include "driver.h"
#include "fce.h"

void CloseGame();
}

#define LOG_TAG "libnes"
#include <utils/Log.h>
#include "emuengine.h"

#define SOUND_RATE			22050

#define SCREEN_W			256
#define SCREEN_H			240
#define SCREEN_PITCH		320

#define GAMEPAD_A			0x0001
#define GAMEPAD_B			0x0002
#define GAMEPAD_A_TURBO		(GAMEPAD_A << 16)
#define GAMEPAD_B_TURBO		(GAMEPAD_B << 16)

extern "C" void Blit8To16Asm(void *src, void *dst, void *pal, int width);

int soundvol = 100;

static EmuEngine::Callbacks *callbacks;
static unsigned int VPalette[256];

class NesEngine : public EmuEngine {
public:
	NesEngine();
	virtual ~NesEngine();

	virtual bool initialize(Callbacks *cbs);
	virtual void destroy();
	virtual void reset();
	virtual void power();
	virtual void fireLightGun(int x, int y);
	virtual Game *loadRom(const char *file);
	virtual void unloadRom();
	virtual bool saveState(const char *file);
	virtual bool loadState(const char *file);
	virtual void runFrame(bool skip);
	virtual void setOption(const char *name, const char *value);

private:
	uint32 JSreturn;
	bool lightGunEnabled;
	uint32 lightGunEvent;
	uint32 MouseData[3];

	int accurateMode;
};


NesEngine::NesEngine()
		: accurateMode(0)
{
}

NesEngine::~NesEngine()
{
}

bool NesEngine::initialize(EmuEngine::Callbacks *cbs)
{
	callbacks = cbs;
	memset(VPalette, 0, sizeof(VPalette));

	if (!FCEUI_Initialize())
		return false;

	JSreturn = 0;
	lightGunEvent = 0;
	lightGunEnabled = false;
	memset(MouseData, 0, sizeof(MouseData));

	FCEUI_SetInput(0, SI_GAMEPAD, &JSreturn, 0);
	return true;
}

void NesEngine::destroy()
{
	delete this;
}

void NesEngine::reset()
{
	ResetNES();
}

void NesEngine::power()
{
	PowerNES();
}

void NesEngine::fireLightGun(int x, int y)
{
	if (!lightGunEnabled)
		return;

	// skip top 16 scanlines for NTSC
	if (!PAL && y < 16)
		return;

	lightGunEvent = x | (y << 8);
}

NesEngine::Game *NesEngine::loadRom(const char *file)
{
	FCEU_CancelDispMessage();
	FCEUI_SetEmuMode(accurateMode);

	if (!FCEUI_LoadGame(file))
		return NULL;

	static Game game;
	game.soundRate = SOUND_RATE;
	game.soundBits = 16;
	game.soundChannels = 1;
	game.fps = (PAL ? 50 : 60);
	return &game;
}

void NesEngine::unloadRom()
{
	CloseGame();
}

bool NesEngine::saveState(const char *file)
{
	FCEUI_SaveState(file);
	return true;
}

bool NesEngine::loadState(const char *file)
{
	FCEUI_LoadState(file);
	return true;
}

void NesEngine::runFrame(bool skip)
{
	// gamepad
	int states = callbacks->getKeyStates();
	static int turbo = 0;
	if (turbo ^= 1) {
		if (states & GAMEPAD_A_TURBO)
			states |= GAMEPAD_A;
		if (states & GAMEPAD_B_TURBO)
			states |= GAMEPAD_B;
	}
	JSreturn = (states & 0xffff);

	// light gun
	const int gun = lightGunEvent;
	if (gun) {
		MouseData[0] = gun & 0xff;
		MouseData[1] = (gun >> 8) & 0xff;
		MouseData[2] = 1;
	}

	extern int FSkip;
	FSkip = (skip ? 1 : 0);
	FCEUI_Emulate();

	// reset light gun
	if (gun) {
		lightGunEvent = 0;
		MouseData[2] = 0;
	}
}

void NesEngine::setOption(const char *name, const char *value)
{
	if (strcmp(name, "soundEnabled") == 0) {
		bool enabled = (strcmp(value, "true") == 0);
		FCEUI_Sound(enabled ? SOUND_RATE : 0);

	} else if (strcmp(name, "enableLightGun") == 0) {
		lightGunEnabled = (strcmp(value, "true") == 0);
		if (lightGunEnabled)
			FCEUI_SetInput(1, SI_ZAPPER, &MouseData, 1);
		else
			FCEUI_SetInput(1, SI_NONE, NULL, 0);

	} else if (strcmp(name, "accurateRendering") == 0) {
		bool enabled = (strcmp(value, "true") == 0);
		accurateMode = (enabled ? 1 : 0);
	}
}


extern "C"
void FCEUD_Update(uint8 *xbuf, int16 *Buffer, int Count)
{
	if (xbuf != NULL) {
		EmuEngine::Surface surface;
		if (callbacks->lockSurface(&surface)) {
			uint8 *d = (uint8 *) surface.bits;
			uint8 *s = xbuf + 24;
			for (int h = SCREEN_H; --h >= 0; ) {
				Blit8To16Asm(s, d, VPalette, SCREEN_W);
				d += surface.bpr;
				s += SCREEN_PITCH;
			}
			callbacks->unlockSurface(&surface);
		}
	}

	if (Count > 0)
		callbacks->playAudio(Buffer, Count * 2);
}

extern "C"
void FCEUD_SetPalette(uint8 index, uint8 r, uint8 g, uint8 b)
{
	VPalette[index] =
		((r & 0xf8) << 8) |
		((g & 0xfc) << 3) |
		((b & 0xf8) >> 3);
}

extern "C"
void FCEUD_GetPalette(uint8 index, uint8 *r, uint8 *g, uint8 *b)
{
}

extern "C" __attribute__((visibility("default")))
EmuEngine *createEngine()
{
	return new NesEngine;
}
