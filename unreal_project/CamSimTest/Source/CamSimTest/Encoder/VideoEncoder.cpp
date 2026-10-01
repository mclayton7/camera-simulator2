// Copyright CamSim Contributors. All Rights Reserved.

#include "Encoder/VideoEncoder.h"
#include "Encoder/Nv12.h"
#include "CamSimTest.h"

// ---------------------------------------------------------------------------
// Helper: configure an AVStream as a STANAG 4609 KLV metadata stream.
// Used by both OpenKlvStream (primary multicast output) and
// OpenRecordingContext (local .ts recording). Keeps the four required field
// assignments in one place so a future STANAG compliance fix only changes one
// site.
// ---------------------------------------------------------------------------
static void ConfigureKlvStream(struct AVStream* S)
{
	S->codecpar->codec_type = AVMEDIA_TYPE_DATA;
	S->codecpar->codec_id   = AV_CODEC_ID_SMPTE_KLV;
	S->codecpar->codec_tag  = MKTAG('K','L','V','A');
	S->time_base            = AVRational{1, 90000};
}

// -------------------------------------------------------------------------
// Constructor / Destructor
// -------------------------------------------------------------------------

FVideoEncoder::FVideoEncoder(const FCamSimConfig& InConfig)
	: Config(InConfig)
{
}

FVideoEncoder::~FVideoEncoder()  // NOLINT(modernize-use-override) — defined in .h
{
	if (bIsOpen)
	{
		Close();
	}
}

// -------------------------------------------------------------------------
// Open – allocate FFmpeg contexts and write MPEG-TS header
// -------------------------------------------------------------------------

bool FVideoEncoder::Open()
{
	if (bIsOpen) return true;

	EncodedFrameCount = 0;

	// Build UDP URL:  udp://239.x.x.x:5004?pkt_size=1316&ttl=4
	FString UdpUrl = FString::Printf(
		TEXT("udp://%s:%d?pkt_size=1316&ttl=4"),
		*Config.MulticastAddr, Config.MulticastPort);

	auto UrlAnsiCast = StringCast<ANSICHAR>(*UdpUrl);
	const char* UrlAnsi = UrlAnsiCast.Get();

	int Ret = avformat_alloc_output_context2(
		&FmtCtx, nullptr, "mpegts", UrlAnsi);
	if (Ret < 0 || !FmtCtx)
	{
		LogFfmpegError(Ret, TEXT("avformat_alloc_output_context2"));
		return false;
	}

	// Allocate re-usable packets — one for video, one for KLV. Reused across
	// every frame so we don't pay the av_packet_alloc/free cost per tick.
	Pkt    = av_packet_alloc();
	KlvPkt = av_packet_alloc();
	if (!Pkt || !KlvPkt)
	{
		UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: av_packet_alloc failed"));
		return false;
	}

	if (!OpenVideoStream()) return false;
	if (!OpenKlvStream())   return false;

	// Open optional local recording context (Phase 12E)
	if (!Config.Recording.VideoRecordPath.IsEmpty())
	{
		if (OpenRecordingContext())
		{
			UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: local recording enabled -> %s"),
				*Config.Recording.VideoRecordPath);
		}
		else
		{
			UE_LOG(LogCamSim, Warning, TEXT("FVideoEncoder: local recording failed to open, continuing without"));
		}
	}

	// Open UDP output
	Ret = avio_open(&FmtCtx->pb, UrlAnsi, AVIO_FLAG_WRITE);
	if (Ret < 0)
	{
		LogFfmpegError(Ret, TEXT("avio_open"));
		return false;
	}

	// Write MPEG-TS header
	Ret = avformat_write_header(FmtCtx, nullptr);
	if (Ret < 0)
	{
		LogFfmpegError(Ret, TEXT("avformat_write_header"));
		return false;
	}

	bIsOpen = true;

	const TCHAR* CodecLabel = Config.VideoCodec.ToLower().Contains(TEXT("265"))
		? TEXT("H.265") : TEXT("H.264");
	const float LogEffectiveFps = FMath::Clamp(Config.FrameRate, 1.0f, 120.0f);
	UE_LOG(LogCamSim, Log,
		TEXT("FVideoEncoder: %s %dx%d @ %.0ffps  bitrate=%d bps  preset=%s  tune=%s  -> %s"),
		CodecLabel, Config.CaptureWidth, Config.CaptureHeight, LogEffectiveFps,
		Config.VideoBitrate, *Config.H264Preset, *Config.H264Tune, *UdpUrl);
	return true;
}

// -------------------------------------------------------------------------
// OpenVideoStream
// -------------------------------------------------------------------------

TArray<const AVCodec*> FVideoEncoder::SelectVideoCodecs(bool& bOutWantH265)
{
	using EPref = FCamSimConfig::EEncoderPreference;
	const EPref EncoderPref   = Config.EncoderPref;
	const FString CodecPref   = Config.VideoCodec.ToLower().TrimStartAndEnd();
	bOutWantH265              = (CodecPref == TEXT("h265") || CodecPref == TEXT("hevc"));

	const char* NvencName        = bOutWantH265 ? "hevc_nvenc"        : "h264_nvenc";
	const char* VideoToolboxName = bOutWantH265 ? "hevc_videotoolbox" : "h264_videotoolbox";
	const char* SoftwareName     = bOutWantH265 ? "libx265"           : "libx264";
	const AVCodecID FallbackId   = bOutWantH265 ? AV_CODEC_ID_HEVC    : AV_CODEC_ID_H264;

	TArray<const AVCodec*> Candidates;

	// Hardware encoders. An explicit request uses only that encoder and fails
	// if it is missing. Auto tries NVENC but not VideoToolbox: VT can't hold
	// rc_max_rate on noisy scenes (IR grain), so it is opt-in.
	auto AddHardware = [&](EPref Pref, const char* Name) -> bool
	{
		if (EncoderPref != Pref && EncoderPref != EPref::Auto) return true;
		if (const AVCodec* Codec = avcodec_find_encoder_by_name(Name))
		{
			UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: found hardware encoder %s"),
				ANSI_TO_TCHAR(Name));
			Candidates.Add(Codec);
			return true;
		}
		if (EncoderPref == Pref)
		{
			UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: %s requested but not available"),
				ANSI_TO_TCHAR(Name));
			return false;
		}
		UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: %s not available"), ANSI_TO_TCHAR(Name));
		return true;
	};
	if (!AddHardware(EPref::Nvenc, NvencName) ||
	    (EncoderPref == EPref::VideoToolbox && !AddHardware(EPref::VideoToolbox, VideoToolboxName)))
	{
		return {};
	}
	if (EncoderPref == EPref::Nvenc || EncoderPref == EPref::VideoToolbox)
	{
		return Candidates;
	}

	// Software fallback (and the only choice for explicit libx264/libx265).
	if (const AVCodec* Codec = avcodec_find_encoder_by_name(SoftwareName)) Candidates.AddUnique(Codec);
	if (const AVCodec* Codec = avcodec_find_encoder(FallbackId))           Candidates.AddUnique(Codec);
	if (Candidates.IsEmpty())
	{
		UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: %s encoder not found"),
			bOutWantH265 ? TEXT("H.265/HEVC") : TEXT("H.264"));
	}
	return Candidates;
}

void FVideoEncoder::ApplyEncoderOptions(bool bWantH265)
{
	if (bUsingNvenc)
	{
		av_opt_set(VideoCodecCtx->priv_data, "preset", "p4",  0);
		av_opt_set(VideoCodecCtx->priv_data, "tune",   "ll",  0); // low latency
		av_opt_set(VideoCodecCtx->priv_data, "rc",     "cbr", 0);
		av_opt_set(VideoCodecCtx->priv_data, "gpu",    "0",   0);
	}
	else if (bUsingVideoToolbox)
	{
		// Low-latency session; preset/tune are x264 concepts with no VT analogue.
		av_opt_set_int(VideoCodecCtx->priv_data, "realtime",   1, 0);
		av_opt_set_int(VideoCodecCtx->priv_data, "prio_speed", 1, 0); // macOS 13+, ignored otherwise
		// VT's default QP ceiling stops it from degrading far enough to hold
		// rc_max_rate on busy scenes; allow the full H.264 range.
		VideoCodecCtx->qmax = 51;
	}
	else if (bWantH265)
	{
		av_opt_set(VideoCodecCtx->priv_data, "preset", TCHAR_TO_ANSI(*Config.H264Preset), 0);
		// libx265 uses x265-params for tune instead of a top-level tune key.
		FString X265Params = FString::Printf(TEXT("log-level=warning"));
		if (!Config.H264Tune.IsEmpty())
		{
			X265Params += FString::Printf(TEXT(":tune=%s"), *Config.H264Tune);
		}
		av_opt_set(VideoCodecCtx->priv_data, "x265-params", TCHAR_TO_ANSI(*X265Params), 0);
	}
	else
	{
		av_opt_set(VideoCodecCtx->priv_data, "preset", TCHAR_TO_ANSI(*Config.H264Preset), 0);
		av_opt_set(VideoCodecCtx->priv_data, "tune",   TCHAR_TO_ANSI(*Config.H264Tune),   0);
		// Signal CBR HRD so downstream links and decoders see a constant rate.
		av_opt_set(VideoCodecCtx->priv_data, "nal-hrd", "cbr", 0);
	}
}

bool FVideoEncoder::TryOpenVideoCodec(const AVCodec* Codec, bool bWantH265)
{
	bUsingNvenc        = (FCStringAnsi::Strstr(Codec->name, "nvenc") != nullptr);
	bUsingVideoToolbox = (FCStringAnsi::Strstr(Codec->name, "videotoolbox") != nullptr);
	UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: opening encoder %s (codec=%s)"),
		ANSI_TO_TCHAR(Codec->name), bWantH265 ? TEXT("H.265") : TEXT("H.264"));

	VideoCodecCtx = avcodec_alloc_context3(Codec);
	if (!VideoCodecCtx)
	{
		UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: avcodec_alloc_context3 failed"));
		return false;
	}

	VideoCodecCtx->width       = Config.CaptureWidth;
	VideoCodecCtx->height      = Config.CaptureHeight;
	VideoCodecCtx->pix_fmt     = AV_PIX_FMT_YUV420P;
	const float EffectiveFps = FMath::Clamp(Config.FrameRate, 1.0f, 120.0f);
	VideoCodecCtx->time_base    = AVRational{1, (int)FMath::RoundToInt(EffectiveFps)};
	VideoCodecCtx->framerate    = AVRational{(int)FMath::RoundToInt(EffectiveFps), 1};
	VideoCodecCtx->bit_rate     = Config.VideoBitrate;
	// Cap the peak rate so complex frames can't burst past the link budget:
	// over any window of T seconds the stream stays within
	// bitrate * (T + VbvBufferSec). Honoured by libx264, libx265 and NVENC.
	// VideoToolbox ignores rc_buffer_size and applies rc_max_rate as a
	// per-second data-rate limit instead.
	VideoCodecCtx->rc_max_rate    = Config.VideoBitrate;
	VideoCodecCtx->rc_buffer_size = static_cast<int>(Config.VideoBitrate * VbvBufferSec);
	VideoCodecCtx->gop_size     = (int)FMath::RoundToInt(EffectiveFps);
	VideoCodecCtx->max_b_frames = 0; // zero-latency

	// Explicitly signal the color space so all decoders (VLC, ffplay, hardware) agree,
	// matching the BT.709 limited-range values the GPU sensor graph already applies.
	// Transfer: the GPU sensor graph (ROADMAP 3B) applies the BT.709 OETF itself.
	ColorTrc = AVCOL_TRC_BT709;
	VideoCodecCtx->color_range     = AVCOL_RANGE_MPEG;        // limited (16-235/16-240)
	VideoCodecCtx->color_primaries = AVCOL_PRI_BT709;
	VideoCodecCtx->color_trc       = ColorTrc;
	VideoCodecCtx->colorspace      = AVCOL_SPC_BT709;

	if (FmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
		VideoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	ApplyEncoderOptions(bWantH265);

	const int Ret = avcodec_open2(VideoCodecCtx, Codec, nullptr);
	if (Ret < 0)
	{
		LogFfmpegError(Ret, TEXT("avcodec_open2"));
		avcodec_free_context(&VideoCodecCtx);
		return false;
	}
	return true;
}

bool FVideoEncoder::OpenVideoStream()
{
	bool bWantH265 = false;
	const TArray<const AVCodec*> Candidates = SelectVideoCodecs(bWantH265);
	if (Candidates.IsEmpty()) return false;

	UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: libavcodec %d.%d.%d"),
		LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR, LIBAVCODEC_VERSION_MICRO);

	VideoStream = avformat_new_stream(FmtCtx, nullptr);
	if (!VideoStream)
	{
		UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: could not create video stream"));
		return false;
	}
	VideoStream->id = 0;

	// A hardware encoder can be built in yet fail to open (no NVIDIA GPU, a
	// Mac VM without a media engine), so fall through to the next candidate.
	const AVCodec* Opened = nullptr;
	for (const AVCodec* Codec : Candidates)
	{
		if (TryOpenVideoCodec(Codec, bWantH265))
		{
			Opened = Codec;
			break;
		}
		UE_LOG(LogCamSim, Warning, TEXT("FVideoEncoder: could not open %s"),
			ANSI_TO_TCHAR(Codec->name));
	}
	if (!Opened)
	{
		UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: no video encoder could be opened"));
		return false;
	}
	UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: using encoder %s"), ANSI_TO_TCHAR(Opened->name));

	int Ret = avcodec_parameters_from_context(VideoStream->codecpar, VideoCodecCtx);
	if (Ret < 0)
	{
		LogFfmpegError(Ret, TEXT("avcodec_parameters_from_context"));
		return false;
	}

	VideoStream->time_base = VideoCodecCtx->time_base;

	// Allocate the YUV420P frame EncodeFrame de-interleaves NV12 into.
	YuvFrame = av_frame_alloc();
	YuvFrame->format = AV_PIX_FMT_YUV420P;
	YuvFrame->width  = Config.CaptureWidth;
	YuvFrame->height = Config.CaptureHeight;
	av_frame_get_buffer(YuvFrame, 0);

	// Stamp color properties on the YUV frame to match the VUI.
	YuvFrame->color_range     = AVCOL_RANGE_MPEG;
	YuvFrame->colorspace      = AVCOL_SPC_BT709;
	YuvFrame->color_primaries = AVCOL_PRI_BT709;
	YuvFrame->color_trc       = ColorTrc;

	return true;
}

// -------------------------------------------------------------------------
// OpenKlvStream
// -------------------------------------------------------------------------

bool FVideoEncoder::OpenKlvStream()
{
	KlvStream = avformat_new_stream(FmtCtx, nullptr);
	if (!KlvStream)
	{
		UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: could not create KLV stream"));
		return false;
	}
	KlvStream->id = 1;

	// STANAG 4609 compliance (Phase 26C):
	// - codec_type AVMEDIA_TYPE_DATA → PMT stream_type 0x06/0x15 (metadata PES)
	// - codec_id AV_CODEC_ID_SMPTE_KLV → FFmpeg adds registration descriptor "KLVA"
	// - codec_tag 'KLVA' → STANAG 4609 Edition 3 §4.3.1 KLV metadata stream identifier
	// - time_base 90kHz → STANAG 4609 §4.2 MPEG-TS clock reference (ISO/IEC 13818-1)
	// - KLV PTS synced to video PTS via av_rescale_q in WriteKlvPacket
	// - TS packet size 1316 bytes set via pkt_size option in OpenOutputContext
	// - PAT at PID 0x0000 (FFmpeg default) per STANAG 4609 §4.1
	// - PMT contains video (stream_type 0x1B for H.264 / 0x24 for H.265) + KLV
	ConfigureKlvStream(KlvStream);

	return true;
}

// -------------------------------------------------------------------------
// OpenRecordingContext — local .ts file recording (Phase 12E)
// -------------------------------------------------------------------------

bool FVideoEncoder::OpenRecordingContext()
{
	auto PathAnsi = StringCast<ANSICHAR>(*Config.Recording.VideoRecordPath);
	const char* PathStr = PathAnsi.Get();

	int Ret = avformat_alloc_output_context2(&RecordFmtCtx, nullptr, "mpegts", PathStr);
	if (Ret < 0 || !RecordFmtCtx)
	{
		LogFfmpegError(Ret, TEXT("recording avformat_alloc_output_context2"));
		return false;
	}

	// Video stream — copy codec params from the primary stream
	RecordVideoStream = avformat_new_stream(RecordFmtCtx, nullptr);
	if (!RecordVideoStream) return false;
	RecordVideoStream->id = 0;
	avcodec_parameters_from_context(RecordVideoStream->codecpar, VideoCodecCtx);
	RecordVideoStream->time_base = VideoCodecCtx->time_base;

	// KLV data stream — mirror the primary KLV stream
	RecordKlvStream = avformat_new_stream(RecordFmtCtx, nullptr);
	if (!RecordKlvStream) return false;
	RecordKlvStream->id = 1;
	ConfigureKlvStream(RecordKlvStream);

	Ret = avio_open(&RecordFmtCtx->pb, PathStr, AVIO_FLAG_WRITE);
	if (Ret < 0)
	{
		LogFfmpegError(Ret, TEXT("recording avio_open"));
		avformat_free_context(RecordFmtCtx);
		RecordFmtCtx = nullptr;
		return false;
	}

	Ret = avformat_write_header(RecordFmtCtx, nullptr);
	if (Ret < 0)
	{
		LogFfmpegError(Ret, TEXT("recording avformat_write_header"));
		avio_closep(&RecordFmtCtx->pb);
		avformat_free_context(RecordFmtCtx);
		RecordFmtCtx = nullptr;
		return false;
	}

	bRecording = true;
	return true;
}

// -------------------------------------------------------------------------
// EncodeFrame – called from background task thread
// -------------------------------------------------------------------------

void FVideoEncoder::EncodeFrame(
	const FSensorFrame& Frame,
	const FCamSimTelemetry& Telemetry,
	uint64 FrameIdx)
{
	if (!bIsOpen || Frame.IsEmpty()) return;

	// Confirm the encoder is receiving frames (first 3 only to avoid spam).
	if (FrameIdx < 3)
	{
		UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: encoding frame %llu (%d NV12 bytes)"), FrameIdx,
			Frame.Nv12.Num());
	}

	av_frame_make_writable(YuvFrame);

	// GPU sensor graph (the only sensor path): already BT.709 limited-range
	// YUV, so EncodeFrame only de-interleaves NV12 into planar YUV420P.
	if (Frame.Nv12.Num() != CamSimNv12::NumBytes(Config.CaptureWidth, Config.CaptureHeight))
	{
		UE_LOG(LogCamSim, Warning, TEXT("FVideoEncoder: NV12 frame %llu has %d bytes, expected %d — skipped"),
			FrameIdx, Frame.Nv12.Num(), CamSimNv12::NumBytes(Config.CaptureWidth, Config.CaptureHeight));
		return;
	}
	CamSimNv12::SplitToYuv420p(Frame.Nv12.GetData(), Config.CaptureWidth, Config.CaptureHeight,
		YuvFrame->data[0], YuvFrame->linesize[0], YuvFrame->data[1], YuvFrame->linesize[1],
		YuvFrame->data[2], YuvFrame->linesize[2]);

	// Monotonic PTS — one tick per encoded frame so the MPEG-TS stream has
	// uniform frame spacing.  Wall-clock PTS caused stutter when frames were
	// dropped (the gap in wall time produced uneven PTS deltas in the mux).
	YuvFrame->pts = EncodedFrameCount++;

	// Send frame to encoder — Phase 13C: stop encoding on persistent failure
	int Ret = avcodec_send_frame(VideoCodecCtx, YuvFrame);
	if (Ret < 0)
	{
		LogFfmpegError(Ret, TEXT("avcodec_send_frame"));
		// AVERROR(EAGAIN) is transient (encoder buffer full); any other error is fatal
		if (Ret != AVERROR(EAGAIN))
		{
			UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: fatal encode error — closing encoder"));
			Close();
		}
		return;
	}

	// Receive and write encoded packets
	while (Ret >= 0)
	{
		Ret = avcodec_receive_packet(VideoCodecCtx, Pkt);
		if (Ret == AVERROR(EAGAIN) || Ret == AVERROR_EOF) break;
		if (Ret < 0)
		{
			LogFfmpegError(Ret, TEXT("avcodec_receive_packet"));
			break;
		}

		WriteVideoPacket();
	}

	// Write KLV metadata packet for this frame
	WriteKlvPacket(Telemetry, FrameIdx);

	// Mark frame as successfully output (read by watchdog on game thread)
	++SuccessfulFrameCount;
}

// -------------------------------------------------------------------------
// WriteKlvPacket
// -------------------------------------------------------------------------

void FVideoEncoder::WriteVideoPacket()
{
	Pkt->stream_index = VideoStream->index;
	av_packet_rescale_ts(Pkt, VideoCodecCtx->time_base, VideoStream->time_base);

	// Duplicate to local recording before writing (write_frame takes ownership of timing)
	if (bRecording && RecordFmtCtx)
	{
		AVPacket* RecPkt = av_packet_clone(Pkt);
		if (RecPkt)
		{
			RecPkt->stream_index = RecordVideoStream->index;
			av_interleaved_write_frame(RecordFmtCtx, RecPkt);
			av_packet_free(&RecPkt);
		}
	}

	av_interleaved_write_frame(FmtCtx, Pkt);
	av_packet_unref(Pkt);
}

void FVideoEncoder::WriteKlvPacket(const FCamSimTelemetry& Telemetry, uint64 FrameIdx)
{
	if (!KlvPkt) return;

	// Reuse the scratch TArray across frames; BuildMisbST0601Into() appends
	// into it after Reset(), preserving the allocated capacity.
	KlvScratch.Reset();
	FKlvBuilder::BuildMisbST0601Into(Telemetry, KlvScratch);
	if (KlvScratch.Num() == 0) return;

	// av_packet_unref clears any previous payload without freeing the AVPacket
	// itself; av_new_packet then allocates a buf of the right size.
	av_packet_unref(KlvPkt);
	av_new_packet(KlvPkt, KlvScratch.Num());
	FMemory::Memcpy(KlvPkt->data, KlvScratch.GetData(), KlvScratch.Num());

	// KLV PTS derived from video PTS via av_rescale_q (Phase 6)
	const int64 VideoPts = YuvFrame ? YuvFrame->pts : static_cast<int64>(FrameIdx);
	const int64 KlvPts = av_rescale_q(VideoPts, VideoCodecCtx->time_base, KlvStream->time_base);
	KlvPkt->pts          = KlvPts;
	KlvPkt->dts          = KlvPts;
	KlvPkt->duration     = av_rescale_q(1, VideoCodecCtx->time_base, KlvStream->time_base);
	KlvPkt->stream_index = KlvStream->index;

	// Duplicate KLV to local recording — the clone is the one short-lived
	// AVPacket alloc per frame; Unref'ing KlvPkt after the main write would
	// invalidate the buffer the clone shared, so clone first, send both.
	if (bRecording && RecordFmtCtx)
	{
		AVPacket* RecPkt = av_packet_clone(KlvPkt);
		if (RecPkt)
		{
			RecPkt->stream_index = RecordKlvStream->index;
			av_interleaved_write_frame(RecordFmtCtx, RecPkt);
			av_packet_free(&RecPkt);
		}
	}

	av_interleaved_write_frame(FmtCtx, KlvPkt);
	// Keep KlvPkt alive; av_packet_unref on the next call releases this buf.
}

// -------------------------------------------------------------------------
// Close
// -------------------------------------------------------------------------

void FVideoEncoder::Close()
{
	if (!bIsOpen) return;
	bIsOpen = false;

	// Flush encoder
	avcodec_send_frame(VideoCodecCtx, nullptr);
	while (true)
	{
		int Ret = avcodec_receive_packet(VideoCodecCtx, Pkt);
		if (Ret == AVERROR_EOF || Ret < 0) break;
		// Encoders with a pipeline delay (VideoToolbox) still hold frames here.
		WriteVideoPacket();
	}

	av_write_trailer(FmtCtx);

	// Close local recording context (Phase 12E)
	if (bRecording && RecordFmtCtx)
	{
		av_write_trailer(RecordFmtCtx);
		if (RecordFmtCtx->pb) avio_closep(&RecordFmtCtx->pb);
		avformat_free_context(RecordFmtCtx);
		RecordFmtCtx = nullptr;
		RecordVideoStream = nullptr;
		RecordKlvStream = nullptr;
		bRecording = false;
		UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: local recording closed"));
	}

	// Free resources
	if (YuvFrame)      { av_frame_free(&YuvFrame); }
	if (Pkt)           { av_packet_free(&Pkt); }
	if (KlvPkt)        { av_packet_free(&KlvPkt); }
	if (VideoCodecCtx) { avcodec_free_context(&VideoCodecCtx); }
	if (FmtCtx)
	{
		if (FmtCtx->pb) avio_closep(&FmtCtx->pb);
		avformat_free_context(FmtCtx);
		FmtCtx = nullptr;
	}

	UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: closed"));
}

// -------------------------------------------------------------------------
// LogFfmpegError
// -------------------------------------------------------------------------

void FVideoEncoder::LogFfmpegError(int Err, const TCHAR* Context)
{
	char ErrBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
	av_strerror(Err, ErrBuf, sizeof(ErrBuf));
	FString ErrStr(ErrBuf);
	UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: %s - %s"), Context, *ErrStr);
}
