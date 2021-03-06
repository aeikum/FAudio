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

#include "FAudio_internal.h"

void LinkedList_AddEntry(
	LinkedList **start,
	void* toAdd,
	FAudioMutex lock
) {
	LinkedList *newEntry, *latest;
	newEntry = (LinkedList*) FAudio_malloc(sizeof(LinkedList));
	newEntry->entry = toAdd;
	newEntry->next = NULL;
	FAudio_PlatformLockMutex(lock);
	if (*start == NULL)
	{
		*start = newEntry;
	}
	else
	{
		latest = *start;
		while (latest->next != NULL)
		{
			latest = latest->next;
		}
		latest->next = newEntry;
	}
	FAudio_PlatformUnlockMutex(lock);
}

void LinkedList_PrependEntry(
	LinkedList **start,
	void* toAdd,
	FAudioMutex lock
) {
	LinkedList *newEntry;
	newEntry = (LinkedList*) FAudio_malloc(sizeof(LinkedList));
	newEntry->entry = toAdd;
	FAudio_PlatformLockMutex(lock);
	newEntry->next = *start;
	*start = newEntry;
	FAudio_PlatformUnlockMutex(lock);
}

void LinkedList_RemoveEntry(
	LinkedList **start,
	void* toRemove,
	FAudioMutex lock
) {
	LinkedList *latest, *prev;
	latest = *start;
	prev = latest;
	FAudio_PlatformLockMutex(lock);
	while (latest != NULL)
	{
		if (latest->entry == toRemove)
		{
			if (latest == prev) /* First in list */
			{
				*start = latest->next;
			}
			else
			{
				prev->next = latest->next;
			}
			FAudio_free(latest);
			FAudio_PlatformUnlockMutex(lock);
			return;
		}
		prev = latest;
		latest = latest->next;
	}
	FAudio_PlatformUnlockMutex(lock);
	FAudio_assert(0 && "LinkedList element not found!");
}

/* Resampling */

/* Okay, so here's what all this fixed-point goo is for:
 *
 * Inevitably you're going to run into weird sample rates,
 * both from WaveBank data and from pitch shifting changes.
 *
 * How we deal with this is by calculating a fixed "step"
 * value that steps from sample to sample at the speed needed
 * to get the correct output sample rate, and the offset
 * is stored as separate integer and fraction values.
 *
 * This allows us to do weird fractional steps between samples,
 * while at the same time not letting it drift off into death
 * thanks to floating point madness.
 *
 * Steps are stored in fixed-point with 32 bits for the fraction:
 *
 * 00000000000000000000000000000000 00000000000000000000000000000000
 * ^ Integer block (32)             ^ Fraction block (32)
 *
 * For example, to get 1.5:
 * 00000000000000000000000000000001 10000000000000000000000000000000
 *
 * The Integer block works exactly like you'd expect.
 * The Fraction block is divided by the Integer's "One" value.
 * So, the above Fraction represented visually...
 *   1 << 31
 *   -------
 *   1 << 32
 * ... which, simplified, is...
 *   1 << 0
 *   ------
 *   1 << 1
 * ... in other words, 1 / 2, or 0.5.
 */
#define FIXED_PRECISION		32
#define FIXED_ONE		(1LL << FIXED_PRECISION)

/* Quick way to drop parts */
#define FIXED_FRACTION_MASK	(FIXED_ONE - 1)
#define FIXED_INTEGER_MASK	~FIXED_FRACTION_MASK

/* Helper macros to convert fixed to float */
#define DOUBLE_TO_FIXED(dbl) \
	((uint64_t) (dbl * FIXED_ONE + 0.5))
#define FIXED_TO_DOUBLE(fxd) ( \
	(double) (fxd >> FIXED_PRECISION) + /* Integer part */ \
	((fxd & FIXED_FRACTION_MASK) * (1.0 / FIXED_ONE)) /* Fraction part */ \
)

static void FAudio_INTERNAL_DecodeBuffers(
	FAudioSourceVoice *voice,
	uint64_t *toDecode
) {
	uint32_t end, endRead, decoding, decoded = 0;
	FAudioBuffer *buffer = &voice->src.bufferList->buffer;
	FAudioBufferEntry *toDelete;

	/* This should never go past the max ratio size */
	FAudio_assert(*toDecode <= voice->src.decodeSamples);

	while (decoded < *toDecode && buffer != NULL)
	{
		decoding = (uint32_t) *toDecode - decoded;

		/* Start-of-buffer behavior */
		if (	voice->src.curBufferOffset == buffer->PlayBegin &&
			voice->src.callback != NULL &&
			voice->src.callback->OnBufferStart != NULL	)
		{
			voice->src.callback->OnBufferStart(
				voice->src.callback,
				buffer->pContext
			);
		}

		/* Check for end-of-buffer */
		end = (buffer->LoopCount > 0) ?
			(buffer->LoopBegin + buffer->LoopLength) :
			buffer->PlayBegin + buffer->PlayLength;
		endRead = FAudio_min(
			end - voice->src.curBufferOffset,
			decoding
		);

		/* Decode... */
		voice->src.decode(
			buffer,
			voice->src.curBufferOffset,
			voice->audio->decodeCache + (
				decoded * voice->src.format.nChannels
			),
			endRead,
			&voice->src.format
		);

		voice->src.curBufferOffset += endRead;
		voice->src.totalSamples += endRead;

		/* End-of-buffer behavior */
		if (endRead < decoding)
		{
			if (buffer->LoopCount > 0)
			{
				voice->src.curBufferOffset = buffer->LoopBegin;
				if (buffer->LoopCount < FAUDIO_LOOP_INFINITE)
				{
					buffer->LoopCount -= 1;
				}
				if (	voice->src.callback != NULL &&
					voice->src.callback->OnLoopEnd != NULL	)
				{
					voice->src.callback->OnLoopEnd(
						voice->src.callback,
						buffer->pContext
					);
				}
			}
			else
			{
				/* For EOS we can stop storing fraction offsets */
				if (buffer->Flags & FAUDIO_END_OF_STREAM)
				{
					voice->src.curBufferOffsetDec = 0;
					voice->src.totalSamples = 0;
				}

				/* Change active buffer, delete finished buffer */
				toDelete = voice->src.bufferList;
				voice->src.bufferList = voice->src.bufferList->next;
				if (voice->src.bufferList != NULL)
				{
					buffer = &voice->src.bufferList->buffer;
					voice->src.curBufferOffset = buffer->PlayBegin;
				}
				else
				{
					buffer = NULL;

					/* FIXME: I keep going past the buffer so fuck it */
					FAudio_zero(
						voice->audio->decodeCache + (
							(decoded + endRead) *
							voice->src.format.nChannels
						),
						sizeof(float) * (
							(decoding - endRead) *
							voice->src.format.nChannels
						)
					);
				}

				/* Callbacks */
				if (voice->src.callback != NULL)
				{
					if (voice->src.callback->OnBufferEnd != NULL)
					{
						voice->src.callback->OnBufferEnd(
							voice->src.callback,
							toDelete->buffer.pContext
						);
					}
					if (	toDelete->buffer.Flags & FAUDIO_END_OF_STREAM &&
						voice->src.callback->OnStreamEnd != NULL	)
					{
						voice->src.callback->OnStreamEnd(
							voice->src.callback
						);
					}
				}

				FAudio_free(toDelete);
			}
		}

		/* Finally. */
		decoded += endRead;
	}

	/* ... FIXME: I keep going past the buffer so fuck it */
	if (buffer)
	{
		end = (buffer->LoopCount > 0) ?
			(buffer->LoopBegin + buffer->LoopLength) :
			buffer->PlayBegin + buffer->PlayLength;
		endRead = FAudio_min(
			end - voice->src.curBufferOffset,
			EXTRA_DECODE_PADDING
		);

		voice->src.decode(
			buffer,
			voice->src.curBufferOffset,
			voice->audio->decodeCache + (
				decoded * voice->src.format.nChannels
			),
			endRead,
			&voice->src.format
		);

		if (endRead < EXTRA_DECODE_PADDING)
		{
			FAudio_zero(
				voice->audio->decodeCache + (
					decoded * voice->src.format.nChannels
				),
				sizeof(float) * (
					EXTRA_DECODE_PADDING - endRead *
					voice->src.format.nChannels
				)
			);
		}
	}
	else
	{
		FAudio_zero(
			voice->audio->decodeCache + (
				decoded * voice->src.format.nChannels
			),
			sizeof(float) * (
				EXTRA_DECODE_PADDING *
				voice->src.format.nChannels
			)
		);
	}

	*toDecode = decoded;
}

static void FAudio_INTERNAL_ResamplePCM(
	FAudioSourceVoice *voice,
	float **resampleCache,
	uint64_t toResample
) {
	/* Linear Resampler */
	/* TODO: SSE */
	uint32_t i, j;
	float *dCache = voice->audio->decodeCache;
	uint64_t cur = voice->src.resampleOffset & FIXED_FRACTION_MASK;
	for (i = 0; i < toResample; i += 1)
	{
		for (j = 0; j < voice->src.format.nChannels; j += 1)
		{
			/* lerp, then convert to float value */
			*(*resampleCache)++ = (float) (
				dCache[j] +
				(dCache[j + voice->src.format.nChannels] - dCache[j]) *
				FIXED_TO_DOUBLE(cur)
			);
		}

		/* Increment fraction offset by the stepping value */
		voice->src.resampleOffset += voice->src.resampleStep;
		cur += voice->src.resampleStep;

		/* Only increment the sample offset by integer values.
		 * Sometimes this will be 0 until cur accumulates
		 * enough steps, especially for "slow" rates.
		 */
		dCache += (cur >> FIXED_PRECISION) * voice->src.format.nChannels;

		/* Now that any integer has been added, drop it.
		 * The offset pointer will preserve the total.
		 */
		cur &= FIXED_FRACTION_MASK;
	}
}

static inline void FAudio_INTERNAL_FilterVoice(
	const FAudioFilterParameters *filter,
	FAudioFilterState *filterState,
	float *samples,
	uint32_t numSamples,
	uint16_t numChannels)
{
	uint32_t j, ci;

	/* Apply a digital state-variable filter to the voice.
	 * The difference equations of the filter are:
	 *
	 * Yl(n) = F Yb(n - 1) + Yl(n - 1)
	 * Yh(n) = x(n) - Yl(n) - OneOverQ Yb(n - 1)
	 * Yb(n) = F Yh(n) + Yb(n - 1)
	 * Yn(n) = Yl(n) + Yh(n)
	 *
	 * Please note that FAudioFilterParameters.Frequency is defined as:
	 *
	 * (2 * sin(pi * (desired filter cutoff frequency) / sampleRate))
	 *
	 * - @JohanSmet
	 */

	for (j = 0; j < numSamples; j += 1)
	for (ci = 0; ci < numChannels; ci += 1)
	{
		filterState[ci][FAudioLowPassFilter] = filterState[ci][FAudioLowPassFilter] + (filter->Frequency * filterState[ci][FAudioBandPassFilter]);
		filterState[ci][FAudioHighPassFilter] = samples[j * numChannels + ci] - filterState[ci][FAudioLowPassFilter] - (filter->OneOverQ * filterState[ci][FAudioBandPassFilter]);
		filterState[ci][FAudioBandPassFilter] = (filter->Frequency * filterState[ci][FAudioHighPassFilter]) + filterState[ci][FAudioBandPassFilter];
		filterState[ci][FAudioNotchFilter] = filterState[ci][FAudioHighPassFilter] + filterState[ci][FAudioLowPassFilter];
		samples[j * numChannels + ci] = filterState[ci][filter->Type];
	}
}

static inline float *FAudio_INTERNAL_ProcessEffectChain(
	FAudioVoice *voice,
	uint32_t channels,
	uint32_t sampleRate,
	float *buffer,
	uint32_t samples
) {
	uint32_t i;
	FAPO *fapo;
	FAudioWaveFormatEx srcFmt, dstFmt;
	FAPOLockForProcessBufferParameters srcLockParams, dstLockParams;
	FAPOProcessBufferParameters srcParams, dstParams;

	/* Lock in formats that the APO will expect for processing */
	srcFmt.wBitsPerSample = 32;
	srcFmt.wFormatTag = 3;
	srcFmt.nChannels = channels;
	srcFmt.nSamplesPerSec = sampleRate;
	srcFmt.nBlockAlign = srcFmt.nChannels * (srcFmt.wBitsPerSample / 8);
	srcFmt.nAvgBytesPerSec = srcFmt.nSamplesPerSec * srcFmt.nBlockAlign;
	srcFmt.cbSize = 0;
	srcLockParams.pFormat = &srcFmt;
	srcLockParams.MaxFrameCount = samples;

	FAudio_memcpy(&dstFmt, &srcFmt, sizeof(srcFmt));
	dstLockParams.pFormat = &dstFmt;
	dstLockParams.MaxFrameCount = samples;

	/* Set up the buffer to be written into */
	srcParams.pBuffer = buffer;
	srcParams.BufferFlags = FAPO_BUFFER_VALID;
	srcParams.ValidFrameCount = samples;

	FAudio_memcpy(&dstParams, &srcParams, sizeof(srcParams));

	/* Update parameters, process! */
	for (i = 0; i < voice->effects.count; i += 1)
	{
		fapo = voice->effects.desc[i].pEffect;

		if (!voice->effects.inPlaceProcessing[i])
		{
			dstFmt.nChannels = voice->effects.desc[i].OutputChannels;
			dstFmt.nBlockAlign = dstFmt.nChannels * (dstFmt.wBitsPerSample / 8);
			dstFmt.nAvgBytesPerSec = dstFmt.nSamplesPerSec * dstFmt.nBlockAlign;
			
			if (dstParams.pBuffer == buffer)
			{
				FAudio_INTERNAL_ResizeEffectChainCache(voice->audio, dstFmt.nBlockAlign * samples);
				dstParams.pBuffer = voice->audio->effectChainCache;
			}
			else
			{
				dstParams.pBuffer = buffer;
			}
		}

		if (voice->effects.parameterUpdates[i])
		{
			fapo->SetParameters(
				fapo,
				voice->effects.parameters[i],
				voice->effects.parameterSizes[i]
			);
			voice->effects.parameterUpdates[i] = 0;
		}
		fapo->LockForProcess(
			fapo,
			1,
			&srcLockParams,
			1,
			&dstLockParams
		);
		fapo->Process(
			fapo,
			1,
			&srcParams,
			1,
			&dstParams,
			voice->effects.desc[i].InitialState
		);
		fapo->UnlockForProcess(fapo);

		FAudio_memcpy(&srcFmt, &dstFmt, sizeof(dstFmt));
		FAudio_memcpy(&srcParams, &dstParams, sizeof(dstParams));
	}

	return (float *) dstParams.pBuffer;
}

static void FAudio_INTERNAL_MixSource(FAudioSourceVoice *voice)
{
	/* Iterators */
	uint32_t i, j, co, ci;
	/* Decode/Resample variables */
	uint64_t toDecode;
	uint64_t toResample;
	float *resampleCache;
	/* Output mix variables */
	float *stream;
	uint32_t mixed;
	uint32_t oChan;
	FAudioVoice *out;
	uint32_t outputRate;
	double stepd;
	float *effectOut;

	/* Calculate the resample stepping value */
	if (voice->src.resampleFreqRatio != voice->src.freqRatio)
	{
		FAudio_PlatformLockMutex(voice->sendLock);
		out = (voice->sends.SendCount == 0) ?
			voice->audio->master : /* Barf */
			voice->sends.pSends->pOutputVoice;
		FAudio_PlatformUnlockMutex(voice->sendLock);
		outputRate = (out->type == FAUDIO_VOICE_MASTER) ?
			out->master.inputSampleRate :
			out->mix.inputSampleRate;
		stepd = (
			voice->src.freqRatio *
			(double) voice->src.format.nSamplesPerSec /
			(double) outputRate
		);
		voice->src.resampleStep = DOUBLE_TO_FIXED(stepd);
		voice->src.resampleFreqRatio = voice->src.freqRatio;
	}

	/* Last call for buffer data! */
	if (	voice->src.callback != NULL &&
		voice->src.callback->OnVoiceProcessingPassStart != NULL)
	{
		voice->src.callback->OnVoiceProcessingPassStart(
			voice->src.callback,
			voice->src.decodeSamples * sizeof(int16_t)
		);
	}

	FAudio_PlatformLockMutex(voice->src.bufferLock);

	/* Nothing to do? */
	if (voice->src.bufferList == NULL)
	{
		FAudio_PlatformUnlockMutex(voice->src.bufferLock);
		goto end;
	}

	mixed = 0;
	resampleCache = voice->audio->resampleCache;
	while (mixed < voice->src.resampleSamples && voice->src.bufferList != NULL)
	{

		/* Base decode size, int to fixed... */
		toDecode = (voice->src.resampleSamples - mixed) * voice->src.resampleStep;
		/* ... rounded up based on current offset... */
		toDecode += voice->src.curBufferOffsetDec + FIXED_FRACTION_MASK;
		/* ... fixed to int, truncating extra fraction from rounding. */
		toDecode >>= FIXED_PRECISION;

		/* Decode... */
		FAudio_INTERNAL_DecodeBuffers(voice, &toDecode);

		/* int to fixed... */
		toResample = toDecode << FIXED_PRECISION;
		/* ... round back down based on current offset... */
		toResample -= voice->src.curBufferOffsetDec;
		/* ... undo step size, fixed to int. */
		toResample /= voice->src.resampleStep;
		/* FIXME: I feel like this should be an assert but I suck */
		toResample = FAudio_min(toResample, voice->src.resampleSamples - mixed);

		/* Resample... */
		if (voice->src.resampleStep == FIXED_ONE)
		{
			/* Actually, just copy directly... */
			FAudio_memcpy(
				resampleCache,
				voice->audio->decodeCache,
				(size_t) toResample * voice->src.format.nChannels * sizeof(float)
			);
			resampleCache += toResample * voice->src.format.nChannels;
		}
		else
		{
			FAudio_INTERNAL_ResamplePCM(voice, &resampleCache, toResample);
		}

		/* Update buffer offsets */
		if (voice->src.bufferList != NULL)
		{
			/* Increment fixed offset by resample size, int to fixed... */
			voice->src.curBufferOffsetDec += toResample * voice->src.resampleStep;
			/* ... chop off any ints we got from the above increment */
			voice->src.curBufferOffsetDec &= FIXED_FRACTION_MASK;
		}
		else
		{
			voice->src.curBufferOffsetDec = 0;
			voice->src.curBufferOffset = 0;
		}

		/* Finally. */
		mixed += (uint32_t) toResample;
	}
	FAudio_PlatformUnlockMutex(voice->src.bufferLock);
	if (mixed == 0)
	{
		goto end;
	}

	FAudio_PlatformLockMutex(voice->sendLock);

	/* Nowhere to send it? Just skip resampling...*/
	if (voice->sends.SendCount == 0)
	{
		FAudio_PlatformUnlockMutex(voice->sendLock);
		goto end;
	}

	/* Filters */
	if (voice->flags & FAUDIO_VOICE_USEFILTER)
	{
		FAudio_PlatformLockMutex(voice->filterLock);
		FAudio_INTERNAL_FilterVoice(
			&voice->filter,
			voice->filterState,
			voice->audio->resampleCache,
			mixed,
			voice->src.format.nChannels
		);
		FAudio_PlatformUnlockMutex(voice->filterLock);
	}

	/* Process effect chain */
	effectOut = voice->audio->resampleCache;

	FAudio_PlatformLockMutex(voice->effectLock);
	if (voice->effects.count > 0)
	{
		effectOut = FAudio_INTERNAL_ProcessEffectChain(
			voice,
			voice->src.format.nChannels,
			voice->src.format.nSamplesPerSec,
			voice->audio->resampleCache,
			mixed
		);
	}
	FAudio_PlatformUnlockMutex(voice->effectLock);

	/* Send float cache to sends */
	FAudio_PlatformLockMutex(voice->volumeLock);
	for (i = 0; i < voice->sends.SendCount; i += 1)
	{
		out = voice->sends.pSends[i].pOutputVoice;
		if (out->type == FAUDIO_VOICE_MASTER)
		{
			stream = out->master.output;
			oChan = out->master.inputChannels;
		}
		else
		{
			stream = out->mix.inputCache;
			oChan = out->mix.inputChannels;
		}

		/* TODO: SSE */
		for (j = 0; j < mixed; j += 1)
		for (co = 0; co < oChan; co += 1)
		for (ci = 0; ci < voice->outputChannels; ci += 1)
		{
			/* Include source/channel volumes in the mix! */
			stream[j * oChan + co] += (
				effectOut[j * voice->outputChannels + ci] *
				voice->channelVolume[ci] *
				voice->volume *
				voice->sendCoefficients[i][
					co * voice->outputChannels + ci
				]
			);
			stream[j * oChan + co] = FAudio_clamp(
				stream[j * oChan + co],
				-FAUDIO_MAX_VOLUME_LEVEL,
				FAUDIO_MAX_VOLUME_LEVEL
			);
		}
	}
	FAudio_PlatformUnlockMutex(voice->volumeLock);

	FAudio_PlatformUnlockMutex(voice->sendLock);

	/* Done, finally. */
end:
	if (	voice->src.callback != NULL &&
		voice->src.callback->OnVoiceProcessingPassEnd != NULL)
	{
		voice->src.callback->OnVoiceProcessingPassEnd(
			voice->src.callback
		);
	}
}

static void FAudio_INTERNAL_MixSubmix(FAudioSubmixVoice *voice)
{
	uint32_t i, j, co, ci;
	float *stream;
	uint32_t oChan;
	FAudioVoice *out;
	uint32_t resampled;
	float *effectOut;

	FAudio_PlatformLockMutex(voice->sendLock);

	/* Nothing to do? */
	if (voice->sends.SendCount == 0)
	{
		goto end;
	}

	/* Resample (if necessary) */
	resampled = FAudio_PlatformResample(
		voice->mix.resampler,
		voice->mix.inputCache,
		voice->mix.inputSamples,
		voice->audio->resampleCache,
		voice->mix.outputSamples * voice->mix.inputChannels
	);

	/* Submix overall volume is applied _before_ effects/filters, blech! */
	if (voice->volume != 1.0f)
	for (i = 0; i < resampled; i += 1)
	{
		/* TODO: SSE */
		voice->audio->resampleCache[i] *= voice->volume;
		voice->audio->resampleCache[i] = FAudio_clamp(
			voice->audio->resampleCache[i],
			-FAUDIO_MAX_VOLUME_LEVEL,
			FAUDIO_MAX_VOLUME_LEVEL
		);
	}
	resampled /= voice->mix.inputChannels;

	/* Filters */
	if (voice->flags & FAUDIO_VOICE_USEFILTER)
	{
		FAudio_PlatformLockMutex(voice->filterLock);
		FAudio_INTERNAL_FilterVoice(
			&voice->filter,
			voice->filterState,
			voice->audio->resampleCache,
			resampled,
			voice->mix.inputChannels
		);
		FAudio_PlatformUnlockMutex(voice->filterLock);
	}

	/* Process effect chain */
	effectOut = voice->audio->resampleCache;

	FAudio_PlatformLockMutex(voice->effectLock);
	if (voice->effects.count > 0)
	{
		effectOut = FAudio_INTERNAL_ProcessEffectChain(
			voice,
			voice->mix.inputChannels,
			voice->mix.inputSampleRate,
			voice->audio->resampleCache,
			resampled
		);
	}
	FAudio_PlatformUnlockMutex(voice->effectLock);

	/* Send float cache to sends */
	FAudio_PlatformLockMutex(voice->volumeLock);
	for (i = 0; i < voice->sends.SendCount; i += 1)
	{
		out = voice->sends.pSends[i].pOutputVoice;
		if (out->type == FAUDIO_VOICE_MASTER)
		{
			stream = out->master.output;
			oChan = out->master.inputChannels;
		}
		else
		{
			stream = out->mix.inputCache;
			oChan = out->mix.inputChannels;
		}

		/* TODO: SSE */
		for (j = 0; j < resampled; j += 1)
		for (co = 0; co < oChan; co += 1)
		for (ci = 0; ci < voice->outputChannels; ci += 1)
		{
			stream[j * oChan + co] += (
				effectOut[j * voice->outputChannels + ci] *
				voice->channelVolume[ci] *
				voice->sendCoefficients[i][
					co * voice->outputChannels + ci
				]
			);
			stream[j * oChan + co] = FAudio_clamp(
				stream[j * oChan + co],
				-FAUDIO_MAX_VOLUME_LEVEL,
				FAUDIO_MAX_VOLUME_LEVEL
			);
		}
	}
	FAudio_PlatformUnlockMutex(voice->volumeLock);

	/* Zero this at the end, for the next update */
end:
	FAudio_PlatformUnlockMutex(voice->sendLock);
	FAudio_zero(
		voice->mix.inputCache,
		sizeof(float) * voice->mix.inputSamples
	);
}

void FAudio_INTERNAL_UpdateEngine(FAudio *audio, float *output)
{
	uint32_t i, totalSamples;
	LinkedList *list;
	FAudioSourceVoice *source;
	FAudioSubmixVoice *submix;
	FAudioEngineCallback *callback;

	if (!audio->active)
	{
		return;
	}

	/* ProcessingPassStart callbacks */
	FAudio_PlatformLockMutex(audio->callbackLock);
	list = audio->callbacks;
	while (list != NULL)
	{
		callback = (FAudioEngineCallback*) list->entry;
		if (callback->OnProcessingPassStart != NULL)
		{
			callback->OnProcessingPassStart(
				callback
			);
		}
		list = list->next;
	}
	FAudio_PlatformUnlockMutex(audio->callbackLock);

	/* Writes to master will directly write to output */
	audio->master->master.output = output;

	/* Mix sources */
	FAudio_PlatformLockMutex(audio->sourceLock);
	list = audio->sources;
	while (list != NULL)
	{
		source = (FAudioSourceVoice*) list->entry;
		if (source->src.active)
		{
			FAudio_INTERNAL_MixSource(source);
		}
		list = list->next;
	}
	FAudio_PlatformUnlockMutex(audio->sourceLock);

	/* Mix submixes, ordered by processing stage */
	FAudio_PlatformLockMutex(audio->submixLock);
	for (i = 0; i <= audio->submixStages; i += 1)
	{
		list = audio->submixes;
		while (list != NULL)
		{
			submix = (FAudioSubmixVoice*) list->entry;
			if (submix->mix.processingStage == i)
			{
				FAudio_INTERNAL_MixSubmix(submix);
			}
			list = list->next;
		}
	}
	FAudio_PlatformUnlockMutex(audio->submixLock);

	/* Apply master volume */
	totalSamples = audio->updateSize * audio->master->master.inputChannels;
	if (audio->master->volume != 1.0f)
	for (i = 0; i < totalSamples; i += 1)
	{
		/* TODO: SSE */
		output[i] *= audio->master->volume;
		output[i] = FAudio_clamp(
			output[i],
			-FAUDIO_MAX_VOLUME_LEVEL,
			FAUDIO_MAX_VOLUME_LEVEL
		);
	}

	/* Process master effect chain */
	FAudio_PlatformLockMutex(audio->master->effectLock);
	if (audio->master->effects.count > 0)
	{
		float *effectOut = FAudio_INTERNAL_ProcessEffectChain(
			audio->master,
			audio->master->master.inputChannels,
			audio->master->master.inputSampleRate,
			output,
			audio->updateSize
		);

		if (effectOut != output)
		{
			FAudio_memcpy(
				output,
				effectOut,
				audio->updateSize * audio->master->outputChannels * sizeof(float)
			);
		}
	}
	FAudio_PlatformUnlockMutex(audio->master->effectLock);

	/* OnProcessingPassEnd callbacks */
	FAudio_PlatformLockMutex(audio->callbackLock);
	list = audio->callbacks;
	while (list != NULL)
	{
		callback = (FAudioEngineCallback*) list->entry;
		if (callback->OnProcessingPassEnd != NULL)
		{
			callback->OnProcessingPassEnd(
				callback
			);
		}
		list = list->next;
	}
	FAudio_PlatformUnlockMutex(audio->callbackLock);
}

void FAudio_INTERNAL_ResizeDecodeCache(FAudio *audio, uint32_t samples)
{
	if (samples > audio->decodeSamples)
	{
		audio->decodeSamples = samples;
		audio->decodeCache = (float*) FAudio_realloc(
			audio->decodeCache,
			sizeof(float) * audio->decodeSamples
		);
	}
}

void FAudio_INTERNAL_ResizeResampleCache(FAudio *audio, uint32_t samples)
{
	if (samples > audio->resampleSamples)
	{
		audio->resampleSamples = samples;
		audio->resampleCache = (float*) FAudio_realloc(
			audio->resampleCache,
			sizeof(float) * audio->resampleSamples
		);
	}
}

void FAudio_INTERNAL_ResizeEffectChainCache(FAudio *audio, uint32_t samples)
{
	if (samples > audio->effectChainSamples)
	{
		audio->effectChainSamples = samples;
		audio->effectChainCache = (float*)FAudio_realloc(
			audio->effectChainCache,
			sizeof(float) * audio->effectChainSamples
		);
	}
}

static const float MATRIX_DEFAULTS[8][8][64] =
{
	#include "matrix_defaults.inl"
};

void FAudio_INTERNAL_SetDefaultMatrix(
	float *matrix,
	uint32_t srcChannels,
	uint32_t dstChannels
) {
	FAudio_assert(srcChannels > 0 && srcChannels < 9);
	FAudio_assert(dstChannels > 0 && dstChannels < 9);
	FAudio_memcpy(
		matrix,
		MATRIX_DEFAULTS[srcChannels - 1][dstChannels - 1],
		srcChannels * dstChannels * sizeof(float)
	);
}

void FAudio_INTERNAL_AllocEffectChain(
	FAudioVoice *voice,
	const FAudioEffectChain *pEffectChain
) {
	uint32_t i;

	voice->effects.count = pEffectChain->EffectCount;
	if (voice->effects.count == 0)
	{
		return;
	}

	for (i = 0; i < pEffectChain->EffectCount; i += 1)
	{
		pEffectChain->pEffectDescriptors[i].pEffect->AddRef(pEffectChain->pEffectDescriptors[i].pEffect);
	}

	voice->effects.desc = (FAudioEffectDescriptor*) FAudio_malloc(
		voice->effects.count * sizeof(FAudioEffectDescriptor)
	);
	FAudio_memcpy(
		voice->effects.desc,
		pEffectChain->pEffectDescriptors,
		voice->effects.count * sizeof(FAudioEffectDescriptor)
	);
	#define ALLOC_EFFECT_PROPERTY(prop, type) \
		voice->effects.prop = (type*) FAudio_malloc( \
			voice->effects.count * sizeof(type) \
		); \
		FAudio_zero( \
			voice->effects.prop, \
			voice->effects.count * sizeof(type) \
		);
	ALLOC_EFFECT_PROPERTY(parameters, void*)
	ALLOC_EFFECT_PROPERTY(parameterSizes, uint32_t)
	ALLOC_EFFECT_PROPERTY(parameterUpdates, uint8_t)
	ALLOC_EFFECT_PROPERTY(inPlaceProcessing, uint8_t)
	#undef ALLOC_EFFECT_PROPERTY
}

void FAudio_INTERNAL_FreeEffectChain(FAudioVoice *voice)
{
	uint32_t i;

	if (voice->effects.count == 0)
	{
		return;
	}

	for (i = 0; i < voice->effects.count; i += 1)
	{
		voice->effects.desc[i].pEffect->Release(voice->effects.desc[i].pEffect);
	}

	FAudio_free(voice->effects.desc);
	FAudio_free(voice->effects.parameters);
	FAudio_free(voice->effects.parameterSizes);
	FAudio_free(voice->effects.parameterUpdates);
	FAudio_free(voice->effects.inPlaceProcessing);
}

/* PCM Decoding */

void (*FAudio_INTERNAL_Convert_U8_To_F32)(
	const uint8_t *restrict src,
	float *restrict dst,
	uint32_t len
);
void (*FAudio_INTERNAL_Convert_S16_To_F32)(
	const int16_t *restrict src,
	float *restrict dst,
	uint32_t len
);

void FAudio_INTERNAL_DecodePCM8(
	FAudioBuffer *buffer,
	uint32_t curOffset,
	float *decodeCache,
	uint32_t samples,
	FAudioWaveFormatEx *format
) {
	FAudio_INTERNAL_Convert_U8_To_F32(
		((uint8_t*) buffer->pAudioData) + (
			curOffset * format->nChannels
		),
		decodeCache,
		samples * format->nChannels
	);
}

void FAudio_INTERNAL_DecodePCM16(
	FAudioBuffer *buffer,
	uint32_t curOffset,
	float *decodeCache,
	uint32_t samples,
	FAudioWaveFormatEx *format
) {
	FAudio_INTERNAL_Convert_S16_To_F32(
		((int16_t*) buffer->pAudioData) + (
			curOffset * format->nChannels
		),
		decodeCache,
		samples * format->nChannels
	);
}

void FAudio_INTERNAL_DecodePCM32F(
	FAudioBuffer *buffer,
	uint32_t curOffset,
	float *decodeCache,
	uint32_t samples,
	FAudioWaveFormatEx *format
) {
	FAudio_memcpy(
		decodeCache,
		((float*) buffer->pAudioData) + (
			curOffset * format->nChannels
		),
		sizeof(float) * samples * format->nChannels
	);
}

/* MSADPCM Decoding */

static inline int16_t FAudio_INTERNAL_ParseNibble(
	uint8_t nibble,
	uint8_t predictor,
	int16_t *delta,
	int16_t *sample1,
	int16_t *sample2
) {
	static const int32_t AdaptionTable[16] =
	{
		230, 230, 230, 230, 307, 409, 512, 614,
		768, 614, 512, 409, 307, 230, 230, 230
	};
	static const int32_t AdaptCoeff_1[7] =
	{
		256, 512, 0, 192, 240, 460, 392
	};
	static const int32_t AdaptCoeff_2[7] =
	{
		0, -256, 0, 64, 0, -208, -232
	};

	int8_t signedNibble;
	int32_t sampleInt;
	int16_t sample;

	signedNibble = (int8_t) nibble;
	if (signedNibble & 0x08)
	{
		signedNibble -= 0x10;
	}

	sampleInt = (
		(*sample1 * AdaptCoeff_1[predictor]) +
		(*sample2 * AdaptCoeff_2[predictor])
	) / 256;
	sampleInt += signedNibble * (*delta);
	sample = FAudio_clamp(sampleInt, -32768, 32767);

	*sample2 = *sample1;
	*sample1 = sample;
	*delta = (int16_t) (AdaptionTable[nibble] * (int32_t) (*delta) / 256);
	if (*delta < 16)
	{
		*delta = 16;
	}
	return sample;
}

#define READ(item, type) \
	item = *((type*) *buf); \
	*buf += sizeof(type);

static inline void FAudio_INTERNAL_DecodeMonoMSADPCMBlock(
	uint8_t **buf,
	int16_t *blockCache,
	uint32_t align
) {
	uint32_t i;

	/* Temp storage for ADPCM blocks */
	uint8_t predictor;
	int16_t delta;
	int16_t sample1;
	int16_t sample2;

	/* Preamble */
	READ(predictor, uint8_t)
	READ(delta, int16_t)
	READ(sample1, int16_t)
	READ(sample2, int16_t)
	align -= 7;

	/* Samples */
	*blockCache++ = sample2;
	*blockCache++ = sample1;
	for (i = 0; i < align; i += 1, *buf += 1)
	{
		*blockCache++ = FAudio_INTERNAL_ParseNibble(
			*(*buf) >> 4,
			predictor,
			&delta,
			&sample1,
			&sample2
		);
		*blockCache++ = FAudio_INTERNAL_ParseNibble(
			*(*buf) & 0x0F,
			predictor,
			&delta,
			&sample1,
			&sample2
		);
	}
}

static inline void FAudio_INTERNAL_DecodeStereoMSADPCMBlock(
	uint8_t **buf,
	int16_t *blockCache,
	uint32_t align
) {
	uint32_t i;

	/* Temp storage for ADPCM blocks */
	uint8_t l_predictor;
	uint8_t r_predictor;
	int16_t l_delta;
	int16_t r_delta;
	int16_t l_sample1;
	int16_t r_sample1;
	int16_t l_sample2;
	int16_t r_sample2;

	/* Preamble */
	READ(l_predictor, uint8_t)
	READ(r_predictor, uint8_t)
	READ(l_delta, int16_t)
	READ(r_delta, int16_t)
	READ(l_sample1, int16_t)
	READ(r_sample1, int16_t)
	READ(l_sample2, int16_t)
	READ(r_sample2, int16_t)
	align -= 14;

	/* Samples */
	*blockCache++ = l_sample2;
	*blockCache++ = r_sample2;
	*blockCache++ = l_sample1;
	*blockCache++ = r_sample1;
	for (i = 0; i < align; i += 1, *buf += 1)
	{
		*blockCache++ = FAudio_INTERNAL_ParseNibble(
			*(*buf) >> 4,
			l_predictor,
			&l_delta,
			&l_sample1,
			&l_sample2
		);
		*blockCache++ = FAudio_INTERNAL_ParseNibble(
			*(*buf) & 0x0F,
			r_predictor,
			&r_delta,
			&r_sample1,
			&r_sample2
		);
	}
}

#undef READ

void FAudio_INTERNAL_DecodeMonoMSADPCM(
	FAudioBuffer *buffer,
	uint32_t curOffset,
	float *decodeCache,
	uint32_t samples,
	FAudioWaveFormatEx *format
) {
	/* Loop variables */
	uint32_t copy;

	/* Read pointers */
	uint8_t *buf;
	int32_t midOffset;

	/* PCM block cache */
	int16_t blockCache[512]; /* Max block size */

	/* Block size */
	uint32_t bsize = (format->nBlockAlign - 6) * 2;

	/* Where are we starting? */
	buf = (uint8_t*) buffer->pAudioData + (
		(curOffset / bsize) *
		format->nBlockAlign
	);

	/* Are we starting in the middle? */
	midOffset = (curOffset % bsize);

	/* Read in each block directly to the decode cache */
	while (samples > 0)
	{
		copy = FAudio_min(samples, bsize - midOffset);
		FAudio_INTERNAL_DecodeMonoMSADPCMBlock(
			&buf,
			blockCache,
			format->nBlockAlign
		);
		FAudio_INTERNAL_Convert_S16_To_F32(
			blockCache + midOffset,
			decodeCache,
			copy
		);
		decodeCache += copy;
		samples -= copy;
		midOffset = 0;
	}
}

void FAudio_INTERNAL_DecodeStereoMSADPCM(
	FAudioBuffer *buffer,
	uint32_t curOffset,
	float *decodeCache,
	uint32_t samples,
	FAudioWaveFormatEx *format
) {
	/* Loop variables */
	uint32_t copy;

	/* Read pointers */
	uint8_t *buf;
	int32_t midOffset;

	/* PCM block cache */
	int16_t blockCache[1024]; /* Max block size */

	/* Align, block size */
	uint32_t bsize = ((format->nBlockAlign / 2) - 6) * 2;

	/* Where are we starting? */
	buf = (uint8_t*) buffer->pAudioData + (
		(curOffset / bsize) *
		format->nBlockAlign
	);

	/* Are we starting in the middle? */
	midOffset = (curOffset % bsize);

	/* Read in each block directly to the decode cache */
	while (samples > 0)
	{
		copy = FAudio_min(samples, bsize - midOffset);
		FAudio_INTERNAL_DecodeStereoMSADPCMBlock(
			&buf,
			blockCache,
			format->nBlockAlign
		);
		FAudio_INTERNAL_Convert_S16_To_F32(
			blockCache + (midOffset * 2),
			decodeCache,
			copy * 2
		);
		decodeCache += copy * 2;
		samples -= copy;
		midOffset = 0;
	}
}

/* Type Converters */

/* The SSE/NEON converters are based on SDL_audiotypecvt:
 * https://hg.libsdl.org/SDL/file/default/src/audio/SDL_audiotypecvt.c
 * The SSE/NEON detection comes from MojoAL:
 * https://hg.icculus.org/icculus/mojoAL/file/default/mojoal.c
 */

#if defined(__x86_64__)
#define NEED_SCALAR_CONVERTER_FALLBACKS 0  /* x86_64 guarantees SSE2. */
#elif __MACOSX__
#define NEED_SCALAR_CONVERTER_FALLBACKS 0  /* Mac OS X/Intel guarantees SSE2. */
#elif defined(__ARM_ARCH) && (__ARM_ARCH >= 8)
#define NEED_SCALAR_CONVERTER_FALLBACKS 0  /* ARMv8+ promise NEON. */
#elif defined(__APPLE__) && defined(__ARM_ARCH) && (__ARM_ARCH >= 7)
#define NEED_SCALAR_CONVERTER_FALLBACKS 0  /* All Apple ARMv7 chips promise NEON support. */
#else
#define NEED_SCALAR_CONVERTER_FALLBACKS 1
#endif

/* Some platforms fail to define __ARM_NEON__, others need it or arm_neon.h will fail. */
#if (defined(__ARM_ARCH) || defined(_M_ARM))
#  if !NEED_SCALAR_CONVERTER_FALLBACKS && !defined(__ARM_NEON__)
#    define __ARM_NEON__ 1
#  endif
#endif

#ifdef __ARM_NEON__
#include <arm_neon.h>
#define HAVE_NEON_INTRINSICS 1
#endif

#ifdef __SSE2__
#include <emmintrin.h>
#define HAVE_SSE2_INTRINSICS 1
#endif

#define DIVBY128 0.0078125f
#define DIVBY32768 0.000030517578125f

#if NEED_SCALAR_CONVERTER_FALLBACKS
void FAudio_INTERNAL_Convert_U8_To_F32_Scalar(
	const uint8_t *restrict src,
	float *restrict dst,
	uint32_t len
) {
	uint32_t i;
	for (i = 0; i < len; i += 1)
	{
		*dst++ = (*src++ * DIVBY128) - 1.0f;
	}
}

void FAudio_INTERNAL_Convert_S16_To_F32_Scalar(
	const int16_t *restrict src,
	float *restrict dst,
	uint32_t len
) {
	uint32_t i;
	for (i = 0; i < len; i += 1)
	{
		*dst++ = *src++ * DIVBY32768;
	}
}
#endif /* NEED_SCALAR_CONVERTER_FALLBACKS */

#if HAVE_SSE2_INTRINSICS
void FAudio_INTERNAL_Convert_U8_To_F32_SSE2(
	const uint8_t *restrict src,
	float *restrict dst,
	uint32_t len
) {
    int i;
    src += len - 1;
    dst += len - 1;

    /* Get dst aligned to 16 bytes (since buffer is growing, we don't have to worry about overreading from src) */
    for (i = len; i && (((size_t) (dst-15)) & 15); --i, --src, --dst) {
        *dst = (((float) *src) * DIVBY128) - 1.0f;
    }

    src -= 15; dst -= 15;  /* adjust to read SSE blocks from the start. */
    FAudio_assert(!i || ((((size_t) dst) & 15) == 0));

    /* Make sure src is aligned too. */
    if ((((size_t) src) & 15) == 0) {
        /* Aligned! Do SSE blocks as long as we have 16 bytes available. */
        const __m128i *mmsrc = (const __m128i *) src;
        const __m128i zero = _mm_setzero_si128();
        const __m128 divby128 = _mm_set1_ps(DIVBY128);
        const __m128 minus1 = _mm_set1_ps(-1.0f);
        while (i >= 16) {   /* 16 * 8-bit */
            const __m128i bytes = _mm_load_si128(mmsrc);  /* get 16 uint8 into an XMM register. */
            /* treat as int16, shift left to clear every other sint16, then back right with zero-extend. Now uint16. */
            const __m128i shorts1 = _mm_srli_epi16(_mm_slli_epi16(bytes, 8), 8);
            /* right-shift-zero-extend gets us uint16 with the other set of values. */
            const __m128i shorts2 = _mm_srli_epi16(bytes, 8);
            /* unpack against zero to make these int32, convert to float, multiply, add. Whew! */
            /* Note that AVX2 can do floating point multiply+add in one instruction, fwiw. SSE2 cannot. */
            const __m128 floats1 = _mm_add_ps(_mm_mul_ps(_mm_cvtepi32_ps(_mm_unpacklo_epi16(shorts1, zero)), divby128), minus1);
            const __m128 floats2 = _mm_add_ps(_mm_mul_ps(_mm_cvtepi32_ps(_mm_unpacklo_epi16(shorts2, zero)), divby128), minus1);
            const __m128 floats3 = _mm_add_ps(_mm_mul_ps(_mm_cvtepi32_ps(_mm_unpackhi_epi16(shorts1, zero)), divby128), minus1);
            const __m128 floats4 = _mm_add_ps(_mm_mul_ps(_mm_cvtepi32_ps(_mm_unpackhi_epi16(shorts2, zero)), divby128), minus1);
            /* Interleave back into correct order, store. */
            _mm_store_ps(dst, _mm_unpacklo_ps(floats1, floats2));
            _mm_store_ps(dst+4, _mm_unpackhi_ps(floats1, floats2));
            _mm_store_ps(dst+8, _mm_unpacklo_ps(floats3, floats4));
            _mm_store_ps(dst+12, _mm_unpackhi_ps(floats3, floats4));
            i -= 16; mmsrc--; dst -= 16;
        }

        src = (const uint8_t *) mmsrc;
    }

    src += 15; dst += 15;  /* adjust for any scalar finishing. */

    /* Finish off any leftovers with scalar operations. */
    while (i) {
        *dst = (((float) *src) * DIVBY128) - 1.0f;
        i--; src--; dst--;
    }
}

void FAudio_INTERNAL_Convert_S16_To_F32_SSE2(
	const int16_t *restrict src,
	float *restrict dst,
	uint32_t len
) {
    int i;
    src += len - 1;
    dst += len - 1;

    /* Get dst aligned to 16 bytes (since buffer is growing, we don't have to worry about overreading from src) */
    for (i = len; i && (((size_t) (dst-7)) & 15); --i, --src, --dst) {
        *dst = ((float) *src) * DIVBY32768;
    }

    src -= 7; dst -= 7;  /* adjust to read SSE blocks from the start. */
    FAudio_assert(!i || ((((size_t) dst) & 15) == 0));

    /* Make sure src is aligned too. */
    if ((((size_t) src) & 15) == 0) {
        /* Aligned! Do SSE blocks as long as we have 16 bytes available. */
        const __m128 divby32768 = _mm_set1_ps(DIVBY32768);
        while (i >= 8) {   /* 8 * 16-bit */
            const __m128i ints = _mm_load_si128((__m128i const *) src);  /* get 8 sint16 into an XMM register. */
            /* treat as int32, shift left to clear every other sint16, then back right with sign-extend. Now sint32. */
            const __m128i a = _mm_srai_epi32(_mm_slli_epi32(ints, 16), 16);
            /* right-shift-sign-extend gets us sint32 with the other set of values. */
            const __m128i b = _mm_srai_epi32(ints, 16);
            /* Interleave these back into the right order, convert to float, multiply, store. */
            _mm_store_ps(dst, _mm_mul_ps(_mm_cvtepi32_ps(_mm_unpacklo_epi32(a, b)), divby32768));
            _mm_store_ps(dst+4, _mm_mul_ps(_mm_cvtepi32_ps(_mm_unpackhi_epi32(a, b)), divby32768));
            i -= 8; src -= 8; dst -= 8;
        }
    }

    src += 7; dst += 7;  /* adjust for any scalar finishing. */

    /* Finish off any leftovers with scalar operations. */
    while (i) {
        *dst = ((float) *src) * DIVBY32768;
        i--; src--; dst--;
    }
}
#endif /* HAVE_SSE2_INTRINSICS */

#if HAVE_NEON_INTRINSICS
void FAudio_INTERNAL_Convert_U8_To_F32_NEON(
	const uint8_t *restrict src,
	float *restrict dst,
	uint32_t len
) {
    int i;
    src += len - 1;
    dst += len - 1;

    /* Get dst aligned to 16 bytes (since buffer is growing, we don't have to worry about overreading from src) */
    for (i = len; i && (((size_t) (dst-15)) & 15); --i, --src, --dst) {
        *dst = (((float) *src) * DIVBY128) - 1.0f;
    }

    src -= 15; dst -= 15;  /* adjust to read NEON blocks from the start. */
    FAudio_assert(!i || ((((size_t) dst) & 15) == 0));

    /* Make sure src is aligned too. */
    if ((((size_t) src) & 15) == 0) {
        /* Aligned! Do NEON blocks as long as we have 16 bytes available. */
        const uint8_t *mmsrc = (const uint8_t *) src;
        const float32x4_t divby128 = vdupq_n_f32(DIVBY128);
        const float32x4_t one = vdupq_n_f32(1.0f);
        while (i >= 16) {   /* 16 * 8-bit */
            const uint8x16_t bytes = vld1q_u8(mmsrc);  /* get 16 uint8 into a NEON register. */
            const uint16x8_t uint16hi = vmovl_u8(vget_high_u8(bytes));  /* convert top 8 bytes to 8 uint16 */
            const uint16x8_t uint16lo = vmovl_u8(vget_low_u8(bytes));   /* convert bottom 8 bytes to 8 uint16 */
            /* split uint16 to two uint32, then convert to float, then multiply to normalize, subtract to adjust for sign, store. */
            vst1q_f32(dst, vmlsq_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(uint16hi))), divby128, one));
            vst1q_f32(dst+4, vmlsq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(uint16hi))), divby128, one));
            vst1q_f32(dst+8, vmlsq_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(uint16lo))), divby128, one));
            vst1q_f32(dst+12, vmlsq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(uint16lo))), divby128, one));
            i -= 16; mmsrc -= 16; dst -= 16;
        }

        src = (const uint8_t *) mmsrc;
    }

    src += 15; dst += 15;  /* adjust for any scalar finishing. */

    /* Finish off any leftovers with scalar operations. */
    while (i) {
        *dst = (((float) *src) * DIVBY128) - 1.0f;
        i--; src--; dst--;
    }
}

void FAudio_INTERNAL_Convert_S16_To_F32_NEON(
	const int16_t *restrict src,
	float *restrict dst,
	uint32_t len
) {
    int i;
    src += len - 1;
    dst += len - 1;

    /* Get dst aligned to 16 bytes (since buffer is growing, we don't have to worry about overreading from src) */
    for (i = len; i && (((size_t) (dst-7)) & 15); --i, --src, --dst) {
        *dst = ((float) *src) * DIVBY32768;
    }

    src -= 7; dst -= 7;  /* adjust to read NEON blocks from the start. */
    FAudio_assert(!i || ((((size_t) dst) & 15) == 0));

    /* Make sure src is aligned too. */
    if ((((size_t) src) & 15) == 0) {
        /* Aligned! Do NEON blocks as long as we have 16 bytes available. */
        const float32x4_t divby32768 = vdupq_n_f32(DIVBY32768);
        while (i >= 8) {   /* 8 * 16-bit */
            const int16x8_t ints = vld1q_s16((int16_t const *) src);  /* get 8 sint16 into a NEON register. */
            /* split int16 to two int32, then convert to float, then multiply to normalize, store. */
            vst1q_f32(dst, vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(ints))), divby32768));
            vst1q_f32(dst+4, vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(ints))), divby32768));
            i -= 8; src -= 8; dst -= 8;
        }
    }

    src += 7; dst += 7;  /* adjust for any scalar finishing. */

    /* Finish off any leftovers with scalar operations. */
    while (i) {
        *dst = ((float) *src) * DIVBY32768;
        i--; src--; dst--;
    }
}
#endif /* HAVE_NEON_INTRINSICS */

void FAudio_INTERNAL_InitConverterFunctions(uint8_t hasSSE2, uint8_t hasNEON)
{
#if HAVE_SSE2_INTRINSICS
	if (hasSSE2)
	{
		FAudio_INTERNAL_Convert_U8_To_F32 = FAudio_INTERNAL_Convert_U8_To_F32_SSE2;
		FAudio_INTERNAL_Convert_S16_To_F32 = FAudio_INTERNAL_Convert_S16_To_F32_SSE2;
		return;
	}
#endif
#if HAVE_NEON_INTRINSICS
	if (hasNEON)
	{
		FAudio_INTERNAL_Convert_U8_To_F32 = FAudio_INTERNAL_Convert_U8_To_F32_NEON;
		FAudio_INTERNAL_Convert_S16_To_F32 = FAudio_INTERNAL_Convert_S16_To_F32_NEON;
		return;
	}
#endif
#if NEED_SCALAR_CONVERTER_FALLBACKS
	FAudio_INTERNAL_Convert_U8_To_F32 = FAudio_INTERNAL_Convert_U8_To_F32_Scalar;
	FAudio_INTERNAL_Convert_S16_To_F32 = FAudio_INTERNAL_Convert_S16_To_F32_Scalar;
#else
	FAudio_assert(0 && "Need converter functions!");
#endif
}
