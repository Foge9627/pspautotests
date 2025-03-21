#include <common.h>

#include <pspsdk.h>
#include <pspkernel.h>
#include <pspatrac3.h>
#include <pspaudio.h>
#include <pspctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <psputility.h>

#include "atrac.h"
#include "shared.h"

// We log too much.
extern unsigned int CHECKPOINT_OUTPUT_DIRECT;
extern unsigned int HAS_DISPLAY;

// NOTE: If you just run the binary plain in psplink (and started usbhostfs from the root), the output text files will be under host0:/
// which effectively is the root of the pspautotests tree.

u32 min(u32 a, u32 b) {
	u32 ret = a > b ? b : a;
	return ret;
}

// Double buffering seems to be enough.
#define DEC_BUFFERS 2

bool RunAtracTest(Atrac3File &file, AtracTestMode mode, int requestedBufSize, int minRemain, int loopCount, bool enablePlayback) {
	schedf("============================================================\n");
	schedf("AtracTest: 'unk', mode %s, buffer size %08x, min remain %d:\n", AtracTestModeToString(mode), requestedBufSize, minRemain);

    SceCtrlData pad;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

	// Pretty small streaming buffer, so even sample.at3 will need to wrap around more than once.
	// Curious size: 0x4100 results in no wrapping of the buffer.
	// const int blk_size = 0x4200;
	int buf_size = 0;

	// doesn't matter, as long as it's bigger than a frame size (0x800 bytes, or the return value of sceAtracGetMaxSample).
	// We double buffer this, otherwise we get playback glitches.
	int decode_size = 32 * 1024;

	u32 writePtr;
	u32 readFileOffset;

	// We start by just reading the header.
	// sample_long.at3 streams just as well as sample.at3.
	// However, some offsets become different, which is good for testing. We should make both work.
	int file_size;
	int at3_size;
	int load_bytes;
	u8 *at3_data = 0;
	u8 *decode_data = 0;

	{
		file.Seek(0, SEEK_SET);
		u32 header[4];
		file.Read(&header, ARRAY_SIZE(header) * sizeof(u32));
		file.Seek(0, SEEK_SET);

		file_size = header[1];
		schedf("filesize (according to header) = 0x%08x\n", file_size);

		hexDump16((char *)header);

		at3_size = file.Size();

		schedf("at3size = 0x%08x\n", at3_size);

		switch (mode & ATRAC_TEST_MODE_MASK) {
		case ATRAC_TEST_STREAM:
			buf_size = requestedBufSize;
			load_bytes = requestedBufSize;
			schedf("Streaming. buf_size: %08x\n", buf_size);
			break;
		case ATRAC_TEST_HALFWAY:
			schedf("Halfway: Creating a buffer that fits the full %08x bytes, but partially filled.\n", at3_size);
			buf_size = at3_size;
			load_bytes = std::min(requestedBufSize, at3_size);
			break;
		case ATRAC_TEST_HALFWAY_STREAM:
			if (requestedBufSize >= at3_size) {
				schedf("UNINTENDED: Creating a buffer that fits the full %08x bytes, but partially filled.\n", at3_size);
				buf_size = at3_size;
				load_bytes = requestedBufSize;
			} else {
				schedf("Creating a streaming buffer that's partially filled.\n");
				// GTA: Vice City Stories does this.
				buf_size = requestedBufSize;
				load_bytes = 0x800;
			}
			break;
		case ATRAC_TEST_FULL:
			schedf("Full: Creating a buffer that fits all the %08x bytes.\n", at3_size);
			buf_size = at3_size;
			load_bytes = at3_size;
			break;
		}

		at3_data = (u8 *)malloc(buf_size);
		decode_data = (u8 *)malloc(decode_size * DEC_BUFFERS);

		memset(at3_data, 0, buf_size);
		memset(decode_data, 0, decode_size);

		file.Read(at3_data, load_bytes);

		if (mode & ATRAC_TEST_CORRUPT) {
			schedf("Corrupting the data.\n");
			int corruptionLocation = load_bytes - 0x200;
			if (corruptionLocation > 0x4000) {
				corruptionLocation = 0x4000;
			}
			memset(at3_data + corruptionLocation, 0xFF, 0x200);
		}
	}

	int id = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
	int id2 = sceUtilityLoadModule(PSP_MODULE_AV_ATRAC3PLUS);

	if ((id >= 0 || (u32) id == 0x80020139UL) && (id2 >= 0 || (u32) id2 == 0x80020139UL)) {
		schedf("Audio modules: OK\n");
	} else {
		schedf("Audio modules: Failed %08x %08x\n", id, id2);
	}

	schedf("Header: %.*s\n", 4, (char *)at3_data);
	schedf("at3_size: %d bufSize: %08x. minRemain: %d.\n", at3_size, buf_size, minRemain);

	int atracID = -1337;
	// set first block of data
	switch (mode & ATRAC_TEST_MODE_MASK) {
	case ATRAC_TEST_STREAM:
	case ATRAC_TEST_FULL:
		atracID = sceAtracSetDataAndGetID(at3_data, load_bytes);
		if (atracID < 0) {
			schedf("sceAtracSetDataAndGetID: Failed %08x\n", atracID);
			return 1;
		} else {
			schedf("sceAtracSetDataAndGetID: OK, size=%08x (file_size: %08x)\n", load_bytes, at3_size);
		}
		break;
	case ATRAC_TEST_HALFWAY:
	case ATRAC_TEST_HALFWAY_STREAM:
		atracID = sceAtracSetHalfwayBufferAndGetID((u8 *)at3_data, load_bytes, buf_size);
		if (atracID < 0) {
			schedf("sceAtracSetHalfwayBufferAndGetID: Failed %08x\n", atracID);
			return 1;
		} else {
			schedf("sceAtracSetHalfwayBufferAndGetID: OK, load=%08x buf=%08x file_size: %08x\n", load_bytes, buf_size, at3_size);
		}
		break;
	default:
		schedf("bad mode\n");
		return 1;
	}

	hexDump16((char *)at3_data);

	LogAtracContext(atracID, at3_data, NULL, true);

	int result;
	int endSample, loopStart, loopEnd;
	result = sceAtracGetSoundSample(atracID, &endSample, &loopStart, &loopEnd);
	schedf("%08x=sceAtracGetSoundSample: endSample:%08x, loopStart:%08x, loopEnd:%08x\n", result, endSample, loopStart, loopEnd);

	int bitrate;
	result = sceAtracGetBitrate(atracID, &bitrate);
	schedf("%08x=sceAtracGetBitrate: %d\n", result, bitrate);

	// TODO: Need some mono test data

	u32 channelNum;
	result = sceAtracGetChannel(atracID, &channelNum);
	schedf("%08x=sceAtracGetChannel: %d\n", result, channelNum);

	u32 outputChannel;
	result = sceAtracGetOutputChannel(atracID, &outputChannel);
	schedf("%08x=sceAtracGetOutputChannel: %d\n", result, outputChannel);

	int loopNum = 0xcccccccc;
	u32 loopStatus = 0xcccccccc;
	result = sceAtracGetLoopStatus(atracID, &loopNum, &loopStatus);
	schedf("%08x=sceAtracGetLoopStatus(%d, %d)\n", result, loopNum, loopStatus);

	if (loopCount >= -1) {
		// Override the loop number dynamically. It's also read from the file.
		result = sceAtracSetLoopNum(atracID, loopCount);
		schedf("%08x=sceAtracSetLoopNum(%d)\n", result, loopCount);
	}

	int maxSamples = 0;
	result = sceAtracGetMaxSample(atracID, &maxSamples);
	schedf("%08x=sceAtracGetMaxSample: %d (%08x)\n", result, maxSamples, maxSamples);

	int nextSamples = 0;
	result = sceAtracGetNextSample(atracID, &nextSamples);
	schedf("%08x=sceAtracGetNextSample: %d (%08x)\n", result, nextSamples, nextSamples);

	int audioChannel = -1;
	if (enablePlayback) {
		audioChannel = sceAudioChReserve(0, maxSamples, PSP_AUDIO_FORMAT_STEREO);
	}

	u8 *secondBuffer = NULL;
	u32 secondFilePosition;
	u32 secondDataByte;
	result = sceAtracGetSecondBufferInfo(atracID, &secondFilePosition, &secondDataByte);
	schedf("%08x=sceAtracGetSecondBufferInfo: %u, %u\n", result, (unsigned int)secondFilePosition, (unsigned int)secondDataByte);
	if (result == 0) {
		schedf("Filling up a second data buffer according to specifications.\n");
		// A second buffer is needed.
		secondBuffer = (u8 *)malloc(secondDataByte);
		memset(secondBuffer, 0, secondDataByte);
		file.Seek(secondFilePosition, SEEK_SET);
		if (file.Read(secondBuffer, secondDataByte) != (int)secondDataByte) {
			schedf("File read for second buffer failed.\n");
		}
		file.Seek(0, SEEK_SET);
		sceAtracSetSecondBuffer(atracID, secondBuffer, secondDataByte);
	} else {
		sceAtracSetSecondBuffer(atracID, 0, 0);
	}

	int remainFrame = 0xcccccccc;
	result = sceAtracGetRemainFrame(atracID, &remainFrame);
	schedf("sceAtracGetRemainFrame(): %d\n\n", remainFrame);

	// Do an early query just to see what happens here.
	u32 bytesToRead;
	result = sceAtracGetStreamDataInfo(atracID, (u8**)&writePtr, &bytesToRead, &readFileOffset);
	schedf("%i=sceAtracGetStreamDataInfo: %d (offset), %d, %d (%08x %08x %08x)\n", result,
		 (const u8 *)writePtr - at3_data, bytesToRead, readFileOffset,
		 (const u8 *)writePtr - at3_data, bytesToRead, readFileOffset);

	if (sizeof(SceAtracIdInfo) != 128) {
		schedf("bad size %d\n", sizeof(SceAtracIdInfo));
	}

	if (mode & ATRAC_TEST_RESET_POSITION_EARLY) {
		schedf("==========================\n");
		const int seekSamplePos = 0x3000;
		schedf("Option enabled: Resetting the buffer position to %d.\n", seekSamplePos);

		AtracResetBufferInfo resetInfo;
		memset(&resetInfo, 0xcc, sizeof(resetInfo));

		result = sceAtracGetBufferInfoForResetting(atracID, seekSamplePos, &resetInfo);
		LogResetBuffer(result, seekSamplePos, resetInfo, (const u8 *)at3_data);

		int bytesToWrite = resetInfo.first.writableBytes;
		if (!(mode & ATRAC_TEST_RESET_POSITION_RELOAD_ALL)) {
			if (bytesToWrite > requestedBufSize) {
				bytesToWrite = requestedBufSize;
			}
		}

		schedf("Performing actions to reset the buffer - fread of %d/%d bytes from %d in file, to offset %d\n", bytesToWrite, resetInfo.first.writableBytes, resetInfo.first.filePos, resetInfo.first.writePos - at3_data);
		file.Seek(resetInfo.first.filePos, SEEK_SET);
		int writtenBytes1 = file.Read(resetInfo.first.writePos, bytesToWrite);
		int writtenBytes2 = 0;

		LogAtracContext(atracID, at3_data, secondBuffer, true);

		result = sceAtracResetPlayPosition(atracID, seekSamplePos, writtenBytes1, writtenBytes2);
		schedf("%08x=sceAtracResetPlayPosition(%d, %d, %d)\n", result, seekSamplePos, writtenBytes1, writtenBytes2);
	}

	bool first = true;
	int count = 0;
	int end = 3;
	int samples = 0;

	int decIndex = 0;

	bool quit = false;

	while (!(quit && end == 0)) {
		sceCtrlPeekBufferPositive(&pad, 1);
		if (pad.Buttons & PSP_CTRL_START) {
			schedf("Cancelled using the start button");
			break;
		}
		if (quit) {
			// Simple mechanism to try to decode a couple of extra frames at the end, so we
			// can test the errors.
			end--;
		}

		LogAtracContext(atracID, at3_data, secondBuffer, first);

		u8 *dec_frame = decode_data + decode_size * decIndex;

		result = sceAtracGetLoopStatus(atracID, &loopNum, &loopStatus);
		u32 nextDecodePosition = 0;
		result = sceAtracGetNextDecodePosition(atracID, &nextDecodePosition);
		u32 result2 = sceAtracGetNextSample(atracID, &nextSamples);
		schedf("%08x=sceAtracGetNextDecodePosition: pos=%d (%08x) (loopnum %d status %d)\n", result,
			 nextDecodePosition, nextDecodePosition, loopNum, loopStatus);
		schedf("%08x=sceAtracGetNextSample: samples=%d (%80x)", result2, nextSamples, nextSamples);

		// decode
		int finish = 0;
		result = sceAtracDecodeData(atracID, (u16 *)(dec_frame), &samples, &finish, &remainFrame);
		if (finish) {
			schedf("Finish flag hit, stopping soon. result=%08x\n", result);
			quit = true;
		} else {
			schedf("(no finish flag) result=%08x\n", result);
		}
		if (result) {
			schedf("%08x=sceAtracDecodeData error: samples: %08x, finish: %08x, remainFrame: %d\n",
				result, samples, finish, remainFrame);
			quit = true;
		} else {
			schedf("%08x=sceAtracDecodeData: samples: %08x, finish: %08x, remainFrame: %d\n",
				result, samples, finish, remainFrame);
		}

		// de-glitch the first frame, which is usually shorter
		if (first && samples < maxSamples) {
			schedf("Deglitching first frame\n");
			memmove(dec_frame + (maxSamples - samples) * 4, dec_frame, samples * 4);
			memset(dec_frame, 0, (maxSamples - samples) * 4);
		}

		if (enablePlayback) {
			// output sound. 0x8000 is the volume, not the block size, that's specified in sceAudioChReserve.
			sceAudioOutputBlocking(audioChannel, 0x8000, dec_frame);
		}

		if (count == 0) {
			// LogResetBufferInfo(atracID, at3_data);
		}

		schedf("========\n");

		result = sceAtracGetStreamDataInfo(atracID, (u8**)&writePtr, &bytesToRead, &readFileOffset);
		schedf("%i=sceAtracGetStreamDataInfo: %d (off), %d, %d (%08x %08x %08x)\n", result,
			(const u8 *)writePtr - at3_data, bytesToRead, readFileOffset,
			(const u8 *)writePtr - at3_data, bytesToRead, readFileOffset);

		// When not needing data, remainFrame is negative.
		if (remainFrame >= 0 && remainFrame < minRemain) {
			// get stream data info
			if (bytesToRead > 0 && (mode & ATRAC_TEST_DONT_REFILL) == 0) {
				int filePos = file.Tell();
				// In halfway buffer mode, restrict the read size, for a more realistic simulation.
				if ((mode & ATRAC_TEST_MODE_MASK) == ATRAC_TEST_HALFWAY) {
					bytesToRead = min(bytesToRead, requestedBufSize);
				}
				if ((int)readFileOffset != filePos) {
					schedf("Calling fread (%d) (!!!! predicted to be at %d, is at %d). Seeking.\n", bytesToRead, readFileOffset, filePos);
					file.Seek(readFileOffset, SEEK_SET);
				} else {
					schedf("Calling fread (%d) (at file offset %d)\n", bytesToRead, readFileOffset);
				}

				int bytesRead = file.Read((u8*)writePtr, bytesToRead);
				if (bytesRead != (int)bytesToRead) {
					schedf("!!!! fread error: %d != %d\n", bytesRead, bytesToRead);
					return 1;
				}
				LogAtracContext(atracID, at3_data, secondBuffer, first);

				result = sceAtracAddStreamData(atracID, bytesToRead);
				if (result) {
					schedf("%08x=sceAtracAddStreamData(%d) error\n", result, bytesToRead);
					return 1;
				}
				schedf("%08x=sceAtracAddStreamData: %08x\n", result, bytesToRead);

				schedf("========\n");

				// Let's get better information by adding another test here.
				result = sceAtracGetStreamDataInfo(atracID, (u8**)&writePtr, &bytesToRead, &readFileOffset);
				schedf("%i=sceAtracGetStreamDataInfo: %d (off), %d, %d (%08x %08x %08x)\n", result,
					(const u8 *)writePtr - at3_data, bytesToRead, readFileOffset,
					(const u8 *)writePtr - at3_data, bytesToRead, readFileOffset);
			}
		}

		int internalError;
		result = sceAtracGetInternalErrorInfo(atracID, &internalError);
		if (internalError != 0 || result != 0) {
			schedf("%08x=sceAtracGetInternalErrorInfo(): %08x\n", result, internalError);
		}

		first = false;

		decIndex++;
		if (decIndex == DEC_BUFFERS) {
			decIndex = 0;
		}

		if (count % 10 == 0) {
			// This doesn't seem to change with position.
			// LogResetBufferInfo(atracID, (const u8 *)at3_data);
		}

		count++;
	}

	loopNum = 0xcccccccc;
	loopStatus = 0xcccccccc;

	// TODO: Figure out how loop status works.
	result = sceAtracGetLoopStatus(atracID, &loopNum, &loopStatus);
	schedf("(end) %08x=sceAtracGetLoopStatus(%d, %08x)\n", result, loopNum, loopStatus);

	result = sceAtracGetNextSample(atracID, &nextSamples);
	schedf("(end) %08x=sceAtracGetNextSample: %d (%08x)\n", result, nextSamples, nextSamples);

	LogResetBufferInfo(atracID, (const u8 *)at3_data);
	LogAtracContext(atracID, at3_data, secondBuffer, true);

	if (end) {
		schedf("reached end of file\n");
	}

	free(at3_data);

	if (enablePlayback) {
		sceAudioChRelease(audioChannel);
	}

	result = sceAtracReleaseAtracID(atracID);
	schedf("sceAtracReleaseAtracID: %08X\n\n", result);

	if (secondBuffer) {
		free(secondBuffer);
	}
	schedf("Done! req=%d\n", requestedBufSize);
	return true;
}

extern "C" int main(int argc, char *argv[]) {
	CHECKPOINT_OUTPUT_DIRECT = 1;
	HAS_DISPLAY = 0;  // don't waste time logging to the screen.

	Atrac3File file("sample.at3");
	schedf("file size: %d\n", file.Size());
	if (!file.Size()) {
		return 1;
	}
	// ignore return values for now.
	RunAtracTest(file, (AtracTestMode)(ATRAC_TEST_HALFWAY_STREAM | ATRAC_TEST_RESET_POSITION_EARLY), 32 * 1024, 10, 0, false);
	RunAtracTest(file, (AtracTestMode)(ATRAC_TEST_HALFWAY_STREAM), 29 * 1024, 10, 0, false);
	RunAtracTest(file, (AtracTestMode)(ATRAC_TEST_HALFWAY), 0x4300, 10, 0, false);

	Atrac3File looped;
	CreateLoopedAtracFrom(file, looped, 2048 + 2048 * 10 + 256, 249548, 1);  // These parameters work. The key is setting the loop start to 2048 or more.
	Atrac3File looped2;
	CreateLoopedAtracFrom(file, looped2, 2048 + 2048 * 10 + 256, 100000, 1);  // These parameters work. The key is setting the loop start to 2048 or more.

	RunAtracTest(looped, (AtracTestMode)(ATRAC_TEST_STREAM), 0x4000, 32, 1, false);
	RunAtracTest(looped2, (AtracTestMode)(ATRAC_TEST_STREAM), 0x4000, 32, 2, false);
	RunAtracTest(file, (AtracTestMode)(ATRAC_TEST_STREAM), 0x4500, 10, 0, false);
	RunAtracTest(file, (AtracTestMode)(ATRAC_TEST_STREAM | ATRAC_TEST_CORRUPT), 0x3700, 10, 0, false);
	RunAtracTest(file, (AtracTestMode)(ATRAC_TEST_STREAM | ATRAC_TEST_DONT_REFILL), 0x4300, 10, 0, false);
	return 0;
}
