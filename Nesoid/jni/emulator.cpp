#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>

#define LOG_TAG "libemu"
#include <utils/Log.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/jni.h>
#include "emuengine.h"
#include "audioplayer.h"
#include "ticks.h"

static pthread_mutex_t emuStateMutex;
static pthread_cond_t emuStateCond;
static int emuState;

static bool resumeRequested;
static unsigned int keyStates;
static EmuEngine *engine;
static EmuEngine::Game *currentGame;
static bool autoFrameSkip;
static int maxFrameSkips;
static bool soundEnabled;
static AudioPlayer *audioPlayer;
static unsigned short *screen16;

static JNIEnv *jEnv;
static jobject renderSurface;
static int surfaceWidth, surfaceHeight;
static jintArray jImage;
static jmethodID jSendImageMethod;

enum {
	EMUSTATE_RUNNING,
	EMUSTATE_PAUSED,
	EMUSTATE_REQUEST_PAUSE,
	EMUSTATE_REQUEST_RUN,
	EMUSTATE_QUIT
};

class EngineCallbacks : public EmuEngine::Callbacks {
public:
	virtual bool lockSurface(EmuEngine::Surface *surface) {
		surface->bits = screen16;
		surface->bpr = surfaceWidth * 2;
		surface->w = surfaceWidth;
		surface->h = surfaceHeight;
		return true;
	}

	virtual void unlockSurface(const EmuEngine::Surface *surface) {
		jsize size = jEnv->GetArrayLength(jImage);
		jint *image = jEnv->GetIntArrayElements(jImage, 0);
		for (int i = 0; i < size; i++) {
			unsigned short pix = screen16[i];
			image[i] = (pix & 0xf800) << 8 |
					(pix & 0x07e0) << 5 |
					(pix & 0x1f) << 3;
		}
		jEnv->ReleaseIntArrayElements(jImage, image, 0);
		jEnv->CallVoidMethod(renderSurface, jSendImageMethod, jImage);
	}

	virtual void playAudio(void *data, int size) {
		audioPlayer->play(data, size);
	}

	virtual unsigned int getKeyStates() {
		return keyStates;
	}
};


static EmuEngine *loadEmuEngine(const char *dir, const char *lib)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/lib%s.so", dir, lib);

	void *handle = dlopen(path, RTLD_NOW);
	if (handle == NULL) {
		LOGD("Cannot load %s: %s", path, dlerror());
		return NULL;
	}

	EmuEngine *(*createEngine)() = (EmuEngine *(*)())
			dlsym(handle, "createEngine");
	if (createEngine == NULL) {
		dlclose(handle);
		return NULL;
	}
	return createEngine();
}

static AudioPlayer *loadAudioPlayer(const char *libdir)
{
	static const char *const so_names[] = {
		"emusound",
		"emusound2",
	};

	void *handle = NULL;
	for (int i = 0; i < NELEM(so_names); i++) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/lib%s.so", libdir, so_names[i]);
		handle = dlopen(path, RTLD_NOW);
		if (handle != NULL)
			break;

		LOGD("Cannot load %s: %s", path, dlerror());
	}
	if (handle == NULL)
		return NULL;

	AudioPlayer *(*createPlayer)() = (AudioPlayer *(*)())
			dlsym(handle, "createPlayer");
	if (createPlayer == NULL) {
		dlclose(handle);
		return NULL;
	}
	return createPlayer();
}

static void showFPS()
{
	static int frames;
	static unsigned int last;
	unsigned int now = ticksGetTicks();

	frames++;
	if (now - last >= 1000) {
		LOGD("fps: %d", frames * 1000 / (now - last));
		last = now;
		frames = 0;
	}
}

static void pauseEmulator()
{
	pthread_mutex_lock(&emuStateMutex);
	if (emuState == EMUSTATE_RUNNING) {
		emuState = EMUSTATE_REQUEST_PAUSE;
		while (emuState == EMUSTATE_REQUEST_PAUSE)
			pthread_cond_wait(&emuStateCond, &emuStateMutex);
	}
	pthread_mutex_unlock(&emuStateMutex);
}

static void resumeEmulator()
{
	if (!resumeRequested || renderSurface == NULL || currentGame == NULL)
		return;

	pthread_mutex_lock(&emuStateMutex);
	if (emuState == EMUSTATE_PAUSED) {
		emuState = EMUSTATE_REQUEST_RUN;
		pthread_cond_signal(&emuStateCond);

		while (emuState == EMUSTATE_REQUEST_RUN)
			pthread_cond_wait(&emuStateCond, &emuStateMutex);
	}
	pthread_mutex_unlock(&emuStateMutex);
}

static void unloadROM()
{
	if (currentGame == NULL)
		return;

	pauseEmulator();

	if (audioPlayer != NULL)
		audioPlayer->stop();
	engine->unloadRom();
	currentGame = NULL;
}

static void runEmulator()
{
	const bool soundOn = (soundEnabled && audioPlayer);
	if (soundOn)
		audioPlayer->start();

	const int fps = currentGame->fps;
	const unsigned int frameTime = 1000 / fps;
	const unsigned int initialTicks = ticksGetTicks();
	unsigned int lastTicks = initialTicks;
	unsigned int virtualFrameCount = 0;
	int frameSkipCounter = 0;

	while (emuState == EMUSTATE_RUNNING) {
		unsigned int now = ticksGetTicks();
		unsigned int realFrameCount = (now - initialTicks) * fps / 1000;

		virtualFrameCount++;
		if (realFrameCount >= virtualFrameCount) {
			if (realFrameCount > virtualFrameCount &&
					autoFrameSkip && frameSkipCounter < maxFrameSkips) {
				frameSkipCounter++;
			} else {
				virtualFrameCount = realFrameCount;
				if (autoFrameSkip)
					frameSkipCounter = 0;
			}
		} else {
			unsigned int delta = now - lastTicks;
			if (delta < frameTime)
				usleep((frameTime - delta) * 1000);
		}
		if (!autoFrameSkip) {
			if (++frameSkipCounter > maxFrameSkips)
				frameSkipCounter = 0;
		}

		lastTicks = now;
		engine->runFrame(frameSkipCounter > 0);
		//showFPS();
	}

	if (soundOn)
		audioPlayer->pause();
}

static void *emuThreadProc(void *arg)
{
	while (true) {
		pthread_mutex_lock(&emuStateMutex);
		while (emuState == EMUSTATE_PAUSED)
			pthread_cond_wait(&emuStateCond, &emuStateMutex);
		if (emuState == EMUSTATE_QUIT) {
			pthread_mutex_unlock(&emuStateMutex);
			break;
		}
		if (emuState == EMUSTATE_REQUEST_RUN) {
			emuState = EMUSTATE_RUNNING;
			pthread_cond_signal(&emuStateCond);
		}
		pthread_mutex_unlock(&emuStateMutex);

		runEmulator();

		pthread_mutex_lock(&emuStateMutex);
		if (emuState == EMUSTATE_REQUEST_PAUSE) {
			emuState = EMUSTATE_PAUSED;
			pthread_cond_signal(&emuStateCond);
		}
		pthread_mutex_unlock(&emuStateMutex);
	}
	return NULL;
}

static jboolean
Emulator_initialize(JNIEnv *env, jobject self, jstring jdir, jstring jlib)
{
	static EngineCallbacks cbs;

	const char *dir = env->GetStringUTFChars(jdir, NULL);
	const char *lib = env->GetStringUTFChars(jlib, NULL);
	jboolean rv = JNI_FALSE;

	engine = loadEmuEngine(dir, lib);
	if (engine == NULL || !engine->initialize(&cbs)) {
		LOGE("Cannot load emulator engine");
		goto bail;
	}
	audioPlayer = loadAudioPlayer(dir);
	LOGW_IF(audioPlayer == NULL, "Cannot initialize sound module");

	ticksInitialize();

	emuState = EMUSTATE_PAUSED;
	renderSurface = NULL;
	currentGame = NULL;
	resumeRequested = false;
	autoFrameSkip = true;
	maxFrameSkips = 2;
	soundEnabled = false;

	rv = JNI_TRUE;
bail:
	env->ReleaseStringUTFChars(jdir, dir);
	env->ReleaseStringUTFChars(jlib, lib);
	return rv;
}

static void Emulator_cleanUp(JNIEnv *env, jobject self)
{
	unloadROM();

	pthread_mutex_lock(&emuStateMutex);
	emuState = EMUSTATE_QUIT;
	pthread_cond_signal(&emuStateCond);
	pthread_mutex_unlock(&emuStateMutex);

	if (audioPlayer != NULL) {
		audioPlayer->destroy();
		audioPlayer = NULL;
	}
	if (engine != NULL) {
		engine->destroy();
		engine = NULL;
	}
}

static void
Emulator_setRenderSurface(JNIEnv *env, jobject self,
		jobject surface, int width, int height)
{
	pauseEmulator();

	if (renderSurface != NULL) {
		delete[] screen16;
		screen16 = NULL;
		env->DeleteGlobalRef(jImage);
		jImage = NULL;
		env->DeleteGlobalRef(renderSurface);
		renderSurface = NULL;
	}

	if (surface != NULL) {
		surfaceWidth = width;
		surfaceHeight = height;

		screen16 = new unsigned short[width * height];
		jImage = env->NewIntArray(width * height);
		jImage = (jintArray) env->NewGlobalRef(jImage);

		renderSurface = env->NewGlobalRef(surface);
		jclass cls = env->GetObjectClass(surface);
		jSendImageMethod = env->GetMethodID(cls, "onImageUpdate", "([I)V");

		resumeEmulator();
	}
}

static void
Emulator_setKeyStates(JNIEnv *env, jobject self, jint states)
{
	keyStates = states;
}

static void
Emulator_fireLightGun(JNIEnv *env, jobject self, jint x, jint y)
{
	engine->fireLightGun(x, y);
}

static void
Emulator_setOption(JNIEnv *env, jobject self, jstring jname, jstring jvalue)
{
	const char *name = env->GetStringUTFChars(jname, NULL);
	const char *value = env->GetStringUTFChars(jvalue, NULL);

	if (strcmp(name, "autoFrameSkip") == 0) {
		autoFrameSkip = (strcmp(value, "false") != 0);

	} else if (strcmp(name, "maxFrameSkips") == 0) {
		maxFrameSkips = atoi(value);
		if (maxFrameSkips < 2)
			maxFrameSkips = 2;
		else if (maxFrameSkips > 99)
			maxFrameSkips = 99;

	} else {
		if (strcmp(name, "soundEnabled") == 0)
			soundEnabled = (strcmp(value, "false") != 0);
		engine->setOption(name, value);
	}

	env->ReleaseStringUTFChars(jname, name);
	env->ReleaseStringUTFChars(jvalue, value);
}

static void Emulator_reset(JNIEnv *env, jobject self)
{
	pauseEmulator();
	engine->reset();
	resumeEmulator();
}

static void Emulator_power(JNIEnv *env, jobject self)
{
	pauseEmulator();
	engine->power();
	resumeEmulator();
}

static jboolean Emulator_loadROM(JNIEnv *env, jobject self, jstring jfile)
{
	unloadROM();

	const char *file = env->GetStringUTFChars(jfile, NULL);
	jboolean rv = JNI_FALSE;

	currentGame = engine->loadRom(file);
	if (currentGame == NULL)
		goto error;

	if (audioPlayer != NULL) {
		audioPlayer->init(
			currentGame->soundRate,
			currentGame->soundBits,
			currentGame->soundChannels);
	}

	resumeEmulator();
	rv = JNI_TRUE;
error:
	env->ReleaseStringUTFChars(jfile, file);
	return rv;
}

static void Emulator_unloadROM(JNIEnv *env, jobject self)
{
	unloadROM();
}

static void Emulator_pause(JNIEnv *env, jobject self)
{
	resumeRequested = false;
	pauseEmulator();
}

static void Emulator_resume(JNIEnv *env, jobject self)
{
	resumeRequested = true;
	resumeEmulator();
}

static jboolean Emulator_saveState(JNIEnv *env, jobject self, jstring jfile)
{
	const char *file = env->GetStringUTFChars(jfile, NULL);

	pauseEmulator();
	jboolean rv = engine->saveState(file);
	resumeEmulator();

	env->ReleaseStringUTFChars(jfile, file);
	return rv;
}

static jboolean Emulator_loadState(JNIEnv *env, jobject self, jstring jfile)
{
	const char *file = env->GetStringUTFChars(jfile, NULL);

	pauseEmulator();
	jboolean rv = engine->loadState(file);
	resumeEmulator();

	env->ReleaseStringUTFChars(jfile, file);
	return rv;
}

static void Emulator_run(JNIEnv *env, jobject self)
{
	jEnv = env;

	pthread_mutex_init(&emuStateMutex, NULL);
	pthread_cond_init(&emuStateCond, NULL);

	while (true) {
		pthread_mutex_lock(&emuStateMutex);
		while (emuState == EMUSTATE_PAUSED)
			pthread_cond_wait(&emuStateCond, &emuStateMutex);
		if (emuState == EMUSTATE_QUIT) {
			pthread_mutex_unlock(&emuStateMutex);
			break;
		}
		if (emuState == EMUSTATE_REQUEST_RUN) {
			emuState = EMUSTATE_RUNNING;
			pthread_cond_signal(&emuStateCond);
		}
		pthread_mutex_unlock(&emuStateMutex);

		runEmulator();

		pthread_mutex_lock(&emuStateMutex);
		if (emuState == EMUSTATE_REQUEST_PAUSE) {
			emuState = EMUSTATE_PAUSED;
			pthread_cond_signal(&emuStateCond);
		}
		pthread_mutex_unlock(&emuStateMutex);
	}
}

int register_Emulator(JNIEnv *env)
{
	static const JNINativeMethod methods[] = {
		{ "setRenderSurface", "(Lcom/androidemu/EmulatorView;II)V",
				(void *) Emulator_setRenderSurface },
		{ "setKeyStates", "(I)V",
				(void *) Emulator_setKeyStates },
		{ "fireLightGun", "(II)V",
				(void *) Emulator_fireLightGun },
		{ "setOption", "(Ljava/lang/String;Ljava/lang/String;)V",
				(void *) Emulator_setOption },

		{ "initialize", "(Ljava/lang/String;Ljava/lang/String;)Z",
				(void *) Emulator_initialize },
		{ "cleanUp", "()V", (void *) Emulator_cleanUp },
		{ "reset", "()V", (void *) Emulator_reset },
		{ "power", "()V", (void *) Emulator_power },
		{ "loadROM", "(Ljava/lang/String;)Z", (void *) Emulator_loadROM },
		{ "unloadROM", "()V", (void *) Emulator_unloadROM },
		{ "pause", "()V", (void *) Emulator_pause },
		{ "resume", "()V", (void *) Emulator_resume },
		{ "run", "()V", (void *) Emulator_run },
		{ "saveState", "(Ljava/lang/String;)Z", (void *) Emulator_saveState },
		{ "loadState", "(Ljava/lang/String;)Z", (void *) Emulator_loadState },
	};

	return jniRegisterNativeMethods(env, "com/androidemu/Emulator",
			methods, NELEM(methods));
}
