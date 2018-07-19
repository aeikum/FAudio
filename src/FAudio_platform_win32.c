/* FAudio - XAudio Reimplementation for FNA
 *
 * Copyright (c) 2011-2018 Ethan Lee, Luigi Auriemma, and the MonoGame Team
 * Copyright (c) 2018 Andrew Eikum for CodeWeavers
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from
 * the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 * claim that you wrote the original software. If you use this software in a
 * product, an acknowledgment in the product documentation would be
 * appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not be
 * misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * Ethan "flibitijibibo" Lee <flibitijibibo@flibitijibibo.com>
 *
 */

#include "FAudio_internal.h"

#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "wine/heap.h"

#define COBJMACROS
#include "mmsystem.h"
#include "xaudio2.h"
#include "xaudio2fx.h"
#include "xapo.h"
#include "devpkey.h"
#include "mmdeviceapi.h"
#include "audioclient.h"

/* Internal Types */

typedef struct FAudioPlatformDevice
{
	FAudio *audio;
	IAudioClient *aclient;
	IAudioRenderClient *render;
	FAudioWaveFormatExtensible format;
	UINT32 period_frames;
	HANDLE mmevt;
	HANDLE thread;
	BOOL stop_engine;
} FAudioPlatformDevice;

/* Globals */

static LONG platform_ref;

static CRITICAL_SECTION platform_lock;
static CRITICAL_SECTION_DEBUG platform_lock_debug =
{
	0, 0, &platform_lock,
	{ &platform_lock_debug.ProcessLocksList, &platform_lock_debug.ProcessLocksList },
	  0, 0, { (DWORD_PTR)(__FILE__ ": platform_lock") }
};
static CRITICAL_SECTION platform_lock = { &platform_lock_debug, -1, 0, 0, 0, 0 };

static IMMDeviceEnumerator *mmdevenum;
static UINT mmdevcount;
static WCHAR **mmdevids;

LinkedList *devlist = NULL;
FAudioMutex devlock = &platform_lock;

/* Mixer Thread */

static DWORD WINAPI FAudio_INTERNAL_MixerThread(void *user)
{
	FAudioPlatformDevice *device = user;
	UINT32 pad, nframes, offs;
	BYTE *buf;
	HRESULT hr;

	while (1)
	{
		WaitForSingleObject(device->mmevt, INFINITE);

		TRACE("engine tick\n");

		if (device->stop_engine)
			return 0;

		hr = IAudioClient_GetCurrentPadding(device->aclient, &pad);
		if (FAILED(hr))
		{
			WARN("GetCurrentPadding failed: %08x\n", hr);
			continue;
		}

		nframes = device->period_frames * 3 - pad;
		nframes -= nframes % device->period_frames;

		hr = IAudioRenderClient_GetBuffer(device->render, nframes, &buf);
		if (FAILED(hr))
		{
			WARN("GetBuffer failed: %08x\n", hr);
			continue;
		}

		offs = 0;
		while (offs < nframes)
		{
			FAudio_INTERNAL_UpdateEngine(device->audio,
					(float*)(buf + offs * device->format.Format.nBlockAlign));
			offs += device->period_frames;
		}

		hr = IAudioRenderClient_ReleaseBuffer(device->render, nframes, 0);
		if (FAILED(hr))
			WARN("GetBuffer failed: %08x\n", hr);
	}
}

/* Platform Functions */

void FAudio_PlatformAddRef()
{
	FAudio_INTERNAL_InitSIMDFunctions(
		/* TODO */
		1, /* SSE2 */
		0  /* NEON */
	);

	FAudio_PlatformLockMutex(devlock);

	InterlockedIncrement(&platform_ref);

	if (!mmdevids)
	{
		IMMDeviceCollection *devcoll;
		HRESULT hr;

		if (!mmdevenum)
		{
			hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL,
					CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator, (void**)&mmdevenum);
			if (FAILED(hr))
			{
				FAudio_PlatformUnlockMutex(devlock);
				return;
			}
		}

		hr = IMMDeviceEnumerator_EnumAudioEndpoints(mmdevenum, eRender,
				DEVICE_STATE_ACTIVE, &devcoll);
		if (FAILED(hr))
		{
			FAudio_PlatformUnlockMutex(devlock);
			return;
		}

		hr = IMMDeviceCollection_GetCount(devcoll, &mmdevcount);
		if (FAILED(hr))
		{
			IMMDeviceCollection_Release(devcoll);
			FAudio_PlatformUnlockMutex(devlock);
			return;
		}

		if (mmdevcount > 0)
		{
			UINT i, count = 1;
			IMMDevice *dev, *def_dev;

			/* make sure that device 0 is the default device */
			IMMDeviceEnumerator_GetDefaultAudioEndpoint(mmdevenum, eRender, eConsole, &def_dev);

			mmdevids = FAudio_malloc(sizeof(WCHAR *) * mmdevcount);

			for (i = 0; i < mmdevcount; ++i)
			{
				hr = IMMDeviceCollection_Item(devcoll, i, &dev);
				if (SUCCEEDED(hr))
				{
					UINT idx;

					if (dev == def_dev)
						idx = 0;
					else
					{
						idx = count;
						++count;
					}

					hr = IMMDevice_GetId(dev, &mmdevids[idx]);
					if (FAILED(hr))
					{
						WARN("GetId failed: %08x\n", hr);
						FAudio_free(mmdevids);
						mmdevids = NULL;
						IMMDevice_Release(dev);
						FAudio_PlatformUnlockMutex(devlock);
						return;
					}

					IMMDevice_Release(dev);
				}
				else
				{
					WARN("Item failed: %08x\n", hr);
					FAudio_free(mmdevids);
					mmdevids = NULL;
					IMMDeviceCollection_Release(devcoll);
					FAudio_PlatformUnlockMutex(devlock);
					return;
				}
			}
		}

		IMMDeviceCollection_Release(devcoll);
	}

	FAudio_PlatformUnlockMutex(devlock);
}

void FAudio_PlatformRelease()
{
	LONG r;
	FAudio_PlatformLockMutex(devlock);

	r = InterlockedDecrement(&platform_ref);
	if (!r)
	{
		unsigned i;
		FAudio_free(mmdevids);
		mmdevids = NULL;
		for (i = 0; i < mmdevcount; ++i)
			CoTaskMemFree(mmdevids[i]);
		HeapFree(GetProcessHeap(), 0, mmdevids);
		IMMDeviceEnumerator_Release(mmdevenum);
		mmdevenum = NULL;
	}

	FAudio_PlatformUnlockMutex(devlock);
}

static DWORD get_channel_mask(unsigned int channels)
{
	switch (channels)
	{
		case 0:
			return 0;
		case 1:
			return KSAUDIO_SPEAKER_MONO;
		case 2:
			return KSAUDIO_SPEAKER_STEREO;
		case 3:
			return KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY;
		case 4:
			return KSAUDIO_SPEAKER_QUAD;	/* not _SURROUND */
		case 5:
			return KSAUDIO_SPEAKER_QUAD | SPEAKER_LOW_FREQUENCY;
		case 6:
			return KSAUDIO_SPEAKER_5POINT1; /* not 5POINT1_SURROUND */
		case 7:
			return KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER;
		case 8:
			return KSAUDIO_SPEAKER_7POINT1_SURROUND; /* Vista deprecates 7POINT1 */
	}
	FIXME("Unknown speaker configuration: %u\n", channels);
	return 0;
}

static int format_is_float32(WAVEFORMATEX *fmt)
{
	if (	fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (
		fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
		IsEqualGUID(&((FAudioWaveFormatExtensible *)fmt)->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)))
	{
		return (fmt->wBitsPerSample == 32);
	}
	return 0;
}

void FAudio_PlatformInit(FAudio *audio, uint32_t deviceIndex)
{
	FAudioPlatformDevice *device;
	IMMDevice *dev;
	HRESULT hr;
	REFERENCE_TIME period, bufdur;
	WAVEFORMATEX *fmt;

	if (!mmdevids)
		return;

	hr = IMMDeviceEnumerator_GetDevice(mmdevenum, mmdevids[deviceIndex], &dev);
	if (FAILED(hr))
	{
		WARN("GetDevice failed: %08x\n", hr);
		return;
	}

	device = FAudio_malloc(sizeof(*device));
	FAudio_zero(device, sizeof(*device));

	hr = IMMDevice_Activate(dev, &IID_IAudioClient,
			CLSCTX_INPROC_SERVER, NULL, (void**)&device->aclient);
	if (FAILED(hr))
	{
		WARN("Activate(IAudioClient) failed: %08x\n", hr);
		IMMDevice_Release(dev);
		FAudio_free(device);
		return;
	}

	IMMDevice_Release(dev);

	device->audio = audio;

	device->format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	device->format.Format.nChannels = audio->master->master.inputChannels;
	device->format.Format.nSamplesPerSec = audio->master->master.inputSampleRate;
	device->format.Format.wBitsPerSample = 32;
	device->format.Format.nBlockAlign = device->format.Format.nChannels * device->format.Format.wBitsPerSample / 8;
	device->format.Format.nAvgBytesPerSec = device->format.Format.nBlockAlign * device->format.Format.nSamplesPerSec;
	device->format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	device->format.dwChannelMask = get_channel_mask(device->format.Format.nChannels);
	device->format.Samples.wValidBitsPerSample = 32;
	memcpy(&device->format.SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, sizeof(GUID));

	hr = IAudioClient_IsFormatSupported(device->aclient,
			AUDCLNT_SHAREMODE_SHARED, (WAVEFORMATEX*)&device->format.Format, &fmt);
	if (hr == S_FALSE)
	{
		if (!format_is_float32(fmt))
		{
			FIXME("Mix format must be 32-bit float\n");
			IAudioClient_Release(device->aclient);
			FAudio_free(device);
			return;
		}
		if (sizeof(WAVEFORMATEX) + fmt->cbSize > sizeof(WAVEFORMATEXTENSIBLE))
		{
			FIXME("Mix format doesn't fit into WAVEFORMATEXTENSIBLE!\n");
			IAudioClient_Release(device->aclient);
			FAudio_free(device);
			return;
		}
		memcpy(&device->format, fmt, sizeof(WAVEFORMATEX) + fmt->cbSize);
	}

	hr = IAudioClient_GetDevicePeriod(device->aclient, &period, NULL);
	if (FAILED(hr))
	{
		WARN("GetDevicePeriod failed: %08x\n", hr);
		IAudioClient_Release(device->aclient);
		FAudio_free(device);
		return;
	}

	/* 3 periods or 0.1 seconds */
	bufdur = max(3 * period, 1000000);

	hr = IAudioClient_Initialize(device->aclient, AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK, bufdur,
			0, (WAVEFORMATEX*)&device->format.Format, NULL);
	if (FAILED(hr))
	{
		WARN("Initialize failed: %08x\n", hr);
		IAudioClient_Release(device->aclient);
		FAudio_free(device);
		return;
	}

	device->mmevt = CreateEventW(NULL, FALSE, FALSE, NULL);

	hr = IAudioClient_SetEventHandle(device->aclient, device->mmevt);
	if (FAILED(hr))
	{
		WARN("Initialize failed: %08x\n", hr);
		IAudioClient_Release(device->aclient);
		CloseHandle(device->mmevt);
		FAudio_free(device);
		return;
	}

	hr = IAudioClient_GetService(device->aclient, &IID_IAudioRenderClient,
			(void**)&device->render);
	if (FAILED(hr))
	{
		WARN("GetService(IAudioRenderClient) failed: %08x\n", hr);
		IAudioRenderClient_Release(device->render);
		IAudioClient_Release(device->aclient);
		CloseHandle(device->mmevt);
		FAudio_free(device);
		return;
	}

	device->period_frames = MulDiv(period, device->format.Format.nSamplesPerSec, 10000000);
	audio->updateSize = device->period_frames;
	audio->mixFormat = &device->format;
	audio->master->master.inputChannels = device->format.Format.nChannels;
	audio->master->master.inputSampleRate = device->format.Format.nSamplesPerSec;

	device->thread = CreateThread(NULL, 0, &FAudio_INTERNAL_MixerThread, device, 0, NULL);
	hr = IAudioClient_Start(device->aclient);
	if (FAILED(hr))
		WARN("Start(IAudioClient) failed: %08x\n", hr);

	LinkedList_AddEntry(&devlist, device, devlock);
}

void FAudio_PlatformQuit(FAudio *audio)
{
	FAudioPlatformDevice *device;
	LinkedList *dev;
	uint8_t found = 0;

	dev = devlist;
	while (dev != NULL)
	{
		device = (FAudioPlatformDevice*) dev->entry;
		if (device->audio == audio)
		{
			found = 1;
			break;
		}
	}

	if (found)
	{
		device->stop_engine = TRUE;
		SetEvent(device->mmevt);
		WaitForSingleObject(device->thread, INFINITE);
		CloseHandle(device->thread);
		LinkedList_RemoveEntry(&devlist, device, devlock);
		IAudioRenderClient_Release(device->render);
		IAudioClient_Release(device->aclient);
		CloseHandle(device->mmevt);
		FAudio_free(device);
		return;
	}
	dev = dev->next;
}

void FAudio_PlatformStart(FAudio *audio)
{
	FAudioPlatformDevice *device;
	LinkedList *dev;

	dev = devlist;
	while (dev != NULL)
	{
		device = (FAudioPlatformDevice*) dev->entry;
		if (device->audio == audio)
		{
			IAudioClient_Start(device->aclient);
			return;
		}
		dev = dev->next;
	}
}

void FAudio_PlatformStop(FAudio *audio)
{
	FAudioPlatformDevice *device;
	LinkedList *dev;

	dev = devlist;
	while (dev != NULL)
	{
		device = (FAudioPlatformDevice*) dev->entry;
		if (device->audio == audio)
		{
			IAudioClient_Stop(device->aclient);
			return;
		}
		dev = dev->next;
	}
}

uint32_t FAudio_PlatformGetDeviceCount()
{
	return mmdevcount;
}

void FAudio_PlatformGetDeviceDetails(
	uint32_t index,
	FAudioDeviceDetails *details
) {
	const char *name;
	size_t len, i;

	FAudio_zero(details, sizeof(FAudioDeviceDetails));
	if (index > FAudio_PlatformGetDeviceCount())
	{
		return;
	}

	/* FIXME: wchar_t is an asshole */
	lstrcpyW((WCHAR*)details->DeviceID, mmdevids[index]);

	details->Role = (index == 0 ? GlobalDefaultDevice : NotDefaultDevice);

	name = "Should Get Friendly Name"; /* XXX TODO */
	len = FAudio_min(FAudio_strlen(name), 0xFF); /* XXX Nul term? */
	for (i = 0; i < len; i += 1)
	{
		details->DisplayName[i] = name[i];
	}

	/* TODO create device and query */
	details->OutputFormat.dwChannelMask = SPEAKER_STEREO;
	details->OutputFormat.Samples.wValidBitsPerSample = 32;
	details->OutputFormat.Format.wBitsPerSample = 32;
	details->OutputFormat.Format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
	details->OutputFormat.Format.nChannels = 2;
	details->OutputFormat.Format.nSamplesPerSec = 48000;
	details->OutputFormat.Format.nBlockAlign = (
		details->OutputFormat.Format.nChannels *
		(details->OutputFormat.Format.wBitsPerSample / 8)
	);
	details->OutputFormat.Format.nAvgBytesPerSec = (
		details->OutputFormat.Format.nSamplesPerSec *
		details->OutputFormat.Format.nBlockAlign
	);
}

FAudioPlatformFixedRateSRC FAudio_PlatformInitFixedRateSRC(
	uint32_t channels,
	uint32_t inputRate,
	uint32_t outputRate
) {
	/* TODO - base on openal?! */
	return NULL;
}

void FAudio_PlatformCloseFixedRateSRC(FAudioPlatformFixedRateSRC resampler)
{
	/* TODO */
}

uint32_t FAudio_PlatformResample(
	FAudioPlatformFixedRateSRC resampler,
	float *input,
	uint32_t inLen,
	float *output,
	uint32_t outLen
) {
	/* TODO */
	return 0;
}

/* Threading */

FAudioThread FAudio_PlatformCreateThread(
	FAudioThreadFunc func,
	const char *name,
	void* data
) {
	/* TODO */
	return NULL;
}

void FAudio_PlatformWaitThread(FAudioThread thread, int32_t *retval)
{
	/* TODO */
}

void FAudio_PlatformThreadPriority(FAudioThreadPriority priority)
{
	/* TODO */
}

FAudioMutex FAudio_PlatformCreateMutex()
{
	CRITICAL_SECTION *r = FAudio_malloc(sizeof(CRITICAL_SECTION));
	InitializeCriticalSection(r);
	return (FAudioMutex)r;
}

void FAudio_PlatformDestroyMutex(FAudioMutex mutex)
{
	DeleteCriticalSection(mutex);
	FAudio_free(mutex);
}

void FAudio_PlatformLockMutex(FAudioMutex mutex)
{
	EnterCriticalSection(mutex);
}

void FAudio_PlatformUnlockMutex(FAudioMutex mutex)
{
	LeaveCriticalSection(mutex);
}

void FAudio_sleep(uint32_t ms)
{
	Sleep(ms);
}

/* stdlib Functions */

void* FAudio_malloc(size_t size)
{
	return heap_alloc(size);
}

void* FAudio_realloc(void* ptr, size_t size)
{
	return heap_realloc(ptr, size);
}

void FAudio_free(void *ptr)
{
	heap_free(ptr);
}

uint32_t FAudio_timems()
{
	return GetTickCount();
}

/* FAudio I/O */

FAudioIOStream* FAudio_fopen(const char *path)
{
	return NULL;
}

FAudioIOStream* FAudio_memopen(void *mem, int len)
{
	return NULL;
}

uint8_t* FAudio_memptr(FAudioIOStream *io, size_t offset)
{
	return NULL;
}

void FAudio_close(FAudioIOStream *io)
{
}
