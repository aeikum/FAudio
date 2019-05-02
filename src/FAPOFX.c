/* FAudio - XAudio Reimplementation for FNA
 *
 * Copyright (c) 2011-2018 Ethan Lee, Luigi Auriemma, and the MonoGame Team
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

#include "FAPOFX.h"
#include "FAudio_internal.h"

#if FAUDIO_ABI_VERSION >= 1
uint32_t FAPOFX_CreateFX(
#else
uint32_t FAPOFX_CreateFXBADABI(
#endif
	const FAudioGUID *clsid,
	FAPO **pEffect
) {
	return FAPOFX_CreateFX8WithCustomAllocatorEXT(
		clsid,
		pEffect,
		NULL,
		0,
		FAudio_malloc,
		FAudio_free,
		FAudio_realloc
	);
}

uint32_t FAPOFX_CreateFX8(
	const FAudioGUID *clsid,
	FAPO **pEffect,
	const void *pInitData,
	uint32_t InitDataByteSize
) {
	return FAPOFX_CreateFX8WithCustomAllocatorEXT(
		clsid,
		pEffect,
		pInitData,
		InitDataByteSize,
		FAudio_malloc,
		FAudio_free,
		FAudio_realloc
	);
}

#if FAUDIO_ABI_VERSION >= 1
uint32_t FAPOFX_CreateFXWithCustomAllocatorEXT(
#else
uint32_t FAPOFX_CreateFXWithCustomAllocatorEXTBADABI(
#endif
	const FAudioGUID *clsid,
	FAPO **pEffect,
	FAudioMallocFunc customMalloc,
	FAudioFreeFunc customFree,
	FAudioReallocFunc customRealloc
) {
	return FAPOFX_CreateFX8WithCustomAllocatorEXT(
		clsid,
		pEffect,
		NULL,
		0,
		customMalloc,
		customFree,
		customRealloc
	);
}

uint32_t FAPOFX_CreateFX8WithCustomAllocatorEXT(
	const FAudioGUID *clsid,
	FAPO **pEffect,
	const void *pInitData,
	uint32_t InitDataByteSize,
	FAudioMallocFunc customMalloc,
	FAudioFreeFunc customFree,
	FAudioReallocFunc customRealloc
) {
	#define CHECK_AND_RETURN(effect) \
		if (FAudio_memcmp(clsid, &FAPOFX_CLSID_FX##effect, sizeof(FAudioGUID)) == 0) \
		{ \
			return FAPOFXCreate##effect( \
				pEffect, \
				pInitData, \
				InitDataByteSize, \
				customMalloc, \
				customFree, \
				customRealloc, \
				0 \
			); \
		} \
		else if (FAudio_memcmp(clsid, &FAPOFX_CLSID_FX##effect##_LEGACY, sizeof(FAudioGUID)) == 0) \
		{ \
			return FAPOFXCreate##effect( \
				pEffect, \
				pInitData, \
				InitDataByteSize, \
				customMalloc, \
				customFree, \
				customRealloc, \
				1 \
			); \
		}
	CHECK_AND_RETURN(EQ)
	CHECK_AND_RETURN(MasteringLimiter)
	CHECK_AND_RETURN(Reverb)
	CHECK_AND_RETURN(Echo)
	#undef CHECK_AND_RETURN
	return -1;
}

/* ABI Compatibility Garbage */

#if FAUDIO_ABI_VERSION == 1
uint32_t FAPOFX_CreateFXBADABI(
	const FAudioGUID *clsid,
	FAPO **pEffect
) {
	return FAPOFX_CreateFX8WithCustomAllocatorEXT(
		clsid,
		pEffect,
		NULL,
		0,
		FAudio_malloc,
		FAudio_free,
		FAudio_realloc
	);
}
uint32_t FAPOFX_CreateFXWithCustomAllocatorEXTBADABI(
	const FAudioGUID *clsid,
	FAPO **pEffect,
	FAudioMallocFunc customMalloc,
	FAudioFreeFunc customFree,
	FAudioReallocFunc customRealloc
) {
	return FAPOFX_CreateFX8WithCustomAllocatorEXT(
		clsid,
		pEffect,
		NULL,
		0,
		customMalloc,
		customFree,
		customRealloc
	);
}
#elif FAUDIO_ABI_VERSION == 0
uint32_t FAPOFX_CreateFX(
	const FAudioGUID *clsid,
	FAPO **pEffect,
	const void *pInitData,
	uint32_t InitDataByteSize
) {
	return FAPOFX_CreateFX8WithCustomAllocatorEXT(
		clsid,
		pEffect,
		pInitData,
		InitDataByteSize,
		FAudio_malloc,
		FAudio_free,
		FAudio_realloc
	);
}
uint32_t FAPOFX_CreateFXWithCustomAllocatorEXT(
	const FAudioGUID *clsid,
	FAPO **pEffect,
	const void *pInitData,
	uint32_t InitDataByteSize,
	FAudioMallocFunc customMalloc,
	FAudioFreeFunc customFree,
	FAudioReallocFunc customRealloc
) {
	return FAPOFX_CreateFX8WithCustomAllocatorEXT(
		clsid,
		pEffect,
		pInitData,
		InitDataByteSize,
		customMalloc,
		customFree,
		customRealloc
	);
}
#endif

/* vim: set noexpandtab shiftwidth=8 tabstop=8: */
