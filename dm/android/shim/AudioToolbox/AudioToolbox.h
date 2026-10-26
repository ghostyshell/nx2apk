// The slice of AudioToolbox the host uses, on Android: an output AudioUnit with a render callback (hle_audio.c) is an AAudio stream whose data callback calls it,
// with the same pull contract (the callback fills exactly the frames asked for). AudioConverter (AAC to PCM16 for movie sound, hle_movie.c) is MediaCodec's AAC decoder
// (aconv_mc.c).
#pragma once
#include "../darwin_compat.h"
#include <aaudio/AAudio.h>

typedef int32_t OSStatus;
typedef uint32_t UInt32;
typedef int32_t SInt32;
typedef double Float64;
typedef unsigned char Boolean;
#define noErr 0

typedef struct { Float64 mSampleRate; UInt32 mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket, mBytesPerFrame, mChannelsPerFrame, mBitsPerChannel, mReserved; } AudioStreamBasicDescription;
typedef struct { int64_t mStartOffset; UInt32 mVariableFramesInPacket, mDataByteSize; } AudioStreamPacketDescription;
typedef struct { UInt32 mNumberChannels, mDataByteSize; void *mData; } AudioBuffer;
typedef struct { UInt32 mNumberBuffers; AudioBuffer mBuffers[1]; } AudioBufferList;
typedef struct { Float64 mSampleTime; uint64_t mHostTime; } AudioTimeStamp;
typedef UInt32 AudioUnitRenderActionFlags;
typedef OSStatus (*AURenderCallback)(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *bl);
typedef struct { AURenderCallback inputProc; void *inputProcRefCon; } AURenderCallbackStruct;
typedef struct { UInt32 componentType, componentSubType, componentManufacturer, componentFlags, componentFlagsMask; } AudioComponentDescription;
typedef struct dm_aucomp *AudioComponent;
typedef struct dm_au *AudioUnit;

enum { kAudioUnitType_Output = 'auou', kAudioUnitSubType_RemoteIO = 'rioc', kAudioUnitSubType_DefaultOutput = 'def ', kAudioUnitManufacturer_Apple = 'appl' };
enum { kAudioFormatLinearPCM = 'lpcm', kAudioFormatMPEG4AAC = 'aac ', kAudioFormatMPEG = 'mpeg' };
enum { kLinearPCMFormatFlagIsFloat = 1, kLinearPCMFormatFlagIsSignedInteger = 4, kLinearPCMFormatFlagIsPacked = 8 };
enum { kAudioUnitProperty_StreamFormat = 8, kAudioUnitProperty_SetRenderCallback = 23, kAudioDevicePropertyBufferFrameSize = 'fsiz' };
enum { kAudioUnitScope_Global = 0, kAudioUnitScope_Input = 1, kAudioUnitScope_Output = 2 };

AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d);
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioUnit *out);
OSStatus AudioComponentInstanceDispose(AudioUnit u);
OSStatus AudioUnitSetProperty(AudioUnit u, UInt32 id, UInt32 scope, UInt32 elem, const void *data, UInt32 size);
OSStatus AudioUnitInitialize(AudioUnit u);
OSStatus AudioUnitUninitialize(AudioUnit u);
OSStatus AudioOutputUnitStart(AudioUnit u);
OSStatus AudioOutputUnitStop(AudioUnit u);

typedef struct dm_aconv *AudioConverterRef;
typedef OSStatus (*AudioConverterComplexInputDataProc)(AudioConverterRef c, UInt32 *packets, AudioBufferList *data, AudioStreamPacketDescription **desc, void *user);
enum { kAudioConverterDecompressionMagicCookie = 'dmgc' };
OSStatus AudioConverterNew(const AudioStreamBasicDescription *in, const AudioStreamBasicDescription *out, AudioConverterRef *c);
OSStatus AudioConverterSetProperty(AudioConverterRef c, UInt32 id, UInt32 size, const void *data);
OSStatus AudioConverterFillComplexBuffer(AudioConverterRef c, AudioConverterComplexInputDataProc p, void *u, UInt32 *frames, AudioBufferList *out, AudioStreamPacketDescription *d);
OSStatus AudioConverterDispose(AudioConverterRef c);
