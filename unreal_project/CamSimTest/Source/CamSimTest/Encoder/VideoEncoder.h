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
}
THIRD_PARTY_INCLUDES_END

/**
 * FVideoEncoder
 *
 * Encodes NV12 frame data (the GPU sensor graph's output, already BT.709
 * limited-range YUV) to H.264 and muxes it as MPEG-TS over UDP multicast.
 * A second data stream (PID tagged as KLVA) carries MISB ST 0601 KLV
 * metadata interleaved with each video frame.
 *
 * Lifecycle:
 *   Open()  – allocate FFmpeg contexts, write MPEG-TS header
 *   EncodeFrame() – de-interleave NV12, encode, write video + KLV packets
 *   Close() – flush encoder, write MPEG-TS trailer, free contexts
 *
 * Thread safety: EncodeFrame() is called from a single background task thread
 * (serialised by FEncoderThread — single consumer).  Open/Close are called from
 * the game instance subsystem on the game thread before / after gameplay.
 */
class FVideoEncoder : public IFrameSink
{
public:
	explicit FVideoEncoder(const FCamSimConfig& InConfig);
	virtual ~FVideoEncoder() override;

	// IFrameSink interface
	virtual bool Open() override;
	virtual void EncodeFrame(const FSensorFrame& Frame,
	                         const FCamSimTelemetry& Telemetry,
	                         uint64 FrameIdx) override;
	virtual void Close() override;
	virtual bool IsOpen() const override { return bIsOpen; }
	virtual uint64 GetSuccessfulFrameCount() const override { return (uint64)SuccessfulFrameCount; }

	/** VBV buffer, in seconds of VideoBitrate. Bounds bursts on the UDP link. */
	static constexpr double VbvBufferSec = 0.5;

private:
	const FCamSimConfig& Config;
	bool bIsOpen = false;
	bool bUsingNvenc = false;
	bool bUsingVideoToolbox = false;

	/** Incremented after each successful av_interleaved_write_frame call. */
	TAtomic<uint64> SuccessfulFrameCount { 0 };

	// FFmpeg output context
	AVFormatContext* FmtCtx     = nullptr;

	// Video stream
	AVStream*        VideoStream = nullptr;
	AVCodecContext*  VideoCodecCtx = nullptr;
	AVFrame*         YuvFrame    = nullptr;
	/** Stream transfer tag: the GPU sensor graph applies the BT.709 OETF itself. */
	AVColorTransferCharacteristic ColorTrc = AVCOL_TRC_BT709;

	// KLV data stream (SMPTE 336M / KLVA)
	AVStream*        KlvStream   = nullptr;

	// Scratch packet for av_interleaved_write_frame
	AVPacket*        Pkt         = nullptr;

	// KLV scratch — reused across frames so we don't av_packet_alloc +
	// TArray<uint8> allocate + free every single frame. Lifecycle mirrors Pkt.
	AVPacket*        KlvPkt      = nullptr;
	TArray<uint8>    KlvScratch;

	// Local recording (Phase 12E) — optional second output to a local .ts file
	AVFormatContext* RecordFmtCtx      = nullptr;
	AVStream*        RecordVideoStream = nullptr;
	AVStream*        RecordKlvStream   = nullptr;
	bool             bRecording        = false;

	// Monotonic count of successfully encoded frames — used as PTS base so
	// the output stream has uniform frame spacing regardless of wall-clock
	// jitter or dropped frames on the capture side.
	int64 EncodedFrameCount = 0;

	// Helpers
	bool OpenVideoStream();
	bool OpenKlvStream();
	bool OpenRecordingContext();
	/** Rescales Pkt to the stream time base and writes it to the stream and recording. */
	void WriteVideoPacket();
	void WriteKlvPacket(const FCamSimTelemetry& Telemetry, uint64 FrameIdx);
	void LogFfmpegError(int Err, const TCHAR* Context);

	// OpenVideoStream sub-helpers (split for readability).
	// Lists the H.264 or H.265 encoders to try, in order, honouring
	// Config.Encoder (auto/nvenc/videotoolbox/libx*). Empty on failure (logged).
	TArray<const AVCodec*> SelectVideoCodecs(bool& bOutWantH265);
	// Allocates and opens VideoCodecCtx for one encoder. On failure the context
	// is freed so the next candidate can be tried.
	bool TryOpenVideoCodec(const AVCodec* Codec, bool bWantH265);
	// Writes preset/tune/rc/profile options onto VideoCodecCtx->priv_data.
	void ApplyEncoderOptions(bool bWantH265);
};
