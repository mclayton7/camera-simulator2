// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Config/CamSimConfig.h"
#include "Metadata/KlvBuilder.h"
#include "Encoder/IFrameSink.h"

// FFmpeg headers — wrap in extern "C" to handle C linkage
THIRD_PARTY_INCLUDES_START
extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/opt.h"
#include "libswscale/swscale.h"
}
THIRD_PARTY_INCLUDES_END

/**
 * FVideoEncoder
 *
 * Encodes raw BGRA8 frame data to H.264 and muxes it as MPEG-TS over UDP
 * multicast.  A second data stream (PID tagged as KLVA) carries MISB ST 0601
 * KLV metadata interleaved with each video frame.
 *
 * Lifecycle:
 *   Open()  – allocate FFmpeg contexts, write MPEG-TS header
 *   EncodeFrame() – convert BGRA→YUV, encode, write video + KLV packets
 *   Close() – flush encoder, write MPEG-TS trailer, free contexts
 *
 * Thread safety: EncodeFrame() is called from a single background task thread
 * (serialised by ACamSimCamera::bEncoderBusy).  Open/Close are called from
 * the game instance subsystem on the game thread before / after gameplay.
 */
class FVideoEncoder : public IFrameSink
{
public:
	explicit FVideoEncoder(const FCamSimConfig& InConfig);
	virtual ~FVideoEncoder() override;

	// IFrameSink interface
	virtual bool Open() override;
	virtual void EncodeFrame(const TArray<FColor>& PixelData,
	                         const FCamSimTelemetry& Telemetry,
	                         uint64 FrameIdx) override;
	virtual void Close() override;
	virtual bool IsOpen() const override { return bIsOpen; }
	virtual uint64 GetSuccessfulFrameCount() const override { return (uint64)SuccessfulFrameCount; }

private:
	const FCamSimConfig& Config;
	bool bIsOpen = false;

	/** Incremented after each successful av_interleaved_write_frame call. */
	TAtomic<uint64> SuccessfulFrameCount { 0 };

	// FFmpeg output context
	AVFormatContext* FmtCtx     = nullptr;

	// Video stream
	AVStream*        VideoStream = nullptr;
	AVCodecContext*  VideoCodecCtx = nullptr;
	AVFrame*         YuvFrame    = nullptr;
	SwsContext*      SwsCtx      = nullptr;

	// KLV data stream (SMPTE 336M / KLVA)
	AVStream*        KlvStream   = nullptr;

	// Scratch packet for av_interleaved_write_frame
	AVPacket*        Pkt         = nullptr;

	// Helpers
	bool OpenVideoStream();
	bool OpenKlvStream();
	void WriteKlvPacket(const FCamSimTelemetry& Telemetry, uint64 FrameIdx);
	void LogFfmpegError(int Err, const TCHAR* Context);
};
