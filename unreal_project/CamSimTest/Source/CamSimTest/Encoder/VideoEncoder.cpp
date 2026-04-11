// Copyright CamSim Contributors. All Rights Reserved.

#include "Encoder/VideoEncoder.h"
#include "CamSimTest.h"

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

	// Allocate re-usable packet
	Pkt = av_packet_alloc();
	if (!Pkt)
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

	// Phase 21E.1 — ROVER PID override
	if (Config.Streaming.bRoverCompat)
	{
		VideoStream->id = Config.Streaming.RoverVideoPid;
		KlvStream->id   = Config.Streaming.RoverKlvPid;
		UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: ROVER compat — video PID=0x%04X klv PID=0x%04X"),
			Config.Streaming.RoverVideoPid, Config.Streaming.RoverKlvPid);
	}

	// Write MPEG-TS header
	AVDictionary* MuxOpts = nullptr;
	if (Config.Streaming.bRoverCompat)
	{
		av_dict_set(&MuxOpts, "mpegts_pmt_start_pid", "4096", 0);
	}
	Ret = avformat_write_header(FmtCtx, Config.Streaming.bRoverCompat ? &MuxOpts : nullptr);
	if (MuxOpts) av_dict_free(&MuxOpts);
	if (Ret < 0)
	{
		LogFfmpegError(Ret, TEXT("avformat_write_header"));
		return false;
	}

	bIsOpen = true;
	const TCHAR* CodecLabel = Config.VideoCodec.ToLower().Contains(TEXT("265"))
		? TEXT("H.265") : TEXT("H.264");
	const float LogEffectiveFps = (Config.Performance.OutputFrameRateHz > 0.0f)
		? FMath::Clamp(Config.Performance.OutputFrameRateHz, 1.0f, 120.0f)
		: Config.FrameRate;
	UE_LOG(LogCamSim, Log,
		TEXT("FVideoEncoder: %s %dx%d @ %.0ffps  bitrate=%d bps  preset=%s  tune=%s  -> %s"),
		CodecLabel, Config.CaptureWidth, Config.CaptureHeight, LogEffectiveFps,
		Config.VideoBitrate, *Config.H264Preset, *Config.H264Tune, *UdpUrl);
	return true;
}

// -------------------------------------------------------------------------
// OpenVideoStream
// -------------------------------------------------------------------------

bool FVideoEncoder::OpenVideoStream()
{
	const AVCodec* Codec = nullptr;
	const FString EncoderPref = Config.Encoder.ToLower().TrimStartAndEnd();
	const FString CodecPref   = Config.VideoCodec.ToLower().TrimStartAndEnd();
	const bool bWantH265      = (CodecPref == TEXT("h265") || CodecPref == TEXT("hevc"));

	if (bWantH265)
	{
		// H.265/HEVC encoder selection
		if (EncoderPref == TEXT("nvenc") || EncoderPref == TEXT("auto"))
		{
			Codec = avcodec_find_encoder_by_name("hevc_nvenc");
			if (Codec)
			{
				UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: found NVENC encoder hevc_nvenc"));
			}
			else if (EncoderPref == TEXT("nvenc"))
			{
				UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: NVENC requested but hevc_nvenc not available"));
				return false;
			}
			else
			{
				UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: hevc_nvenc not available, trying libx265"));
			}
		}
		if (!Codec)
		{
			Codec = avcodec_find_encoder_by_name("libx265");
		}
		if (!Codec)
		{
			Codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
		}
		if (!Codec)
		{
			UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: H.265/HEVC encoder not found"));
			return false;
		}
	}
	else
	{
		// H.264 encoder selection (default)
		if (EncoderPref == TEXT("nvenc") || EncoderPref == TEXT("auto"))
		{
			Codec = avcodec_find_encoder_by_name("h264_nvenc");
			if (Codec)
			{
				UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: found NVENC encoder h264_nvenc"));
			}
			else if (EncoderPref == TEXT("nvenc"))
			{
				UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: NVENC requested but h264_nvenc not available"));
				return false;
			}
			else
			{
				UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: NVENC not available, falling back to libx264"));
			}
		}
		if (!Codec)
		{
			Codec = avcodec_find_encoder_by_name("libx264");
		}
		if (!Codec)
		{
			Codec = avcodec_find_encoder(AV_CODEC_ID_H264);
		}
		if (!Codec)
		{
			UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: H.264 encoder not found"));
			return false;
		}
	}

	bUsingNvenc = (FCStringAnsi::Strstr(Codec->name, "nvenc") != nullptr);
	UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: using encoder %s (codec=%s)"),
		ANSI_TO_TCHAR(Codec->name), bWantH265 ? TEXT("H.265") : TEXT("H.264"));

	// Log library versions — helps diagnose system-vs-ThirdParty lib conflicts
	UE_LOG(LogCamSim, Log,
		TEXT("FVideoEncoder: libswscale %d.%d.%d  libavcodec %d.%d.%d"),
		LIBSWSCALE_VERSION_MAJOR, LIBSWSCALE_VERSION_MINOR, LIBSWSCALE_VERSION_MICRO,
		LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR, LIBAVCODEC_VERSION_MICRO);

	VideoStream = avformat_new_stream(FmtCtx, nullptr);
	if (!VideoStream)
	{
		UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: could not create video stream"));
		return false;
	}
	VideoStream->id = 0;

	VideoCodecCtx = avcodec_alloc_context3(Codec);
	if (!VideoCodecCtx)
	{
		UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: avcodec_alloc_context3 failed"));
		return false;
	}

	VideoCodecCtx->width       = Config.CaptureWidth;
	VideoCodecCtx->height      = Config.CaptureHeight;
	VideoCodecCtx->pix_fmt     = AV_PIX_FMT_YUV420P;
	const float EffectiveFps = (Config.Performance.OutputFrameRateHz > 0.0f)
		? FMath::Clamp(Config.Performance.OutputFrameRateHz, 1.0f, 120.0f)
		: Config.FrameRate;
	VideoCodecCtx->time_base   = AVRational{1, (int)FMath::RoundToInt(EffectiveFps)};
	VideoCodecCtx->framerate   = AVRational{(int)FMath::RoundToInt(EffectiveFps), 1};
	VideoCodecCtx->bit_rate    = Config.VideoBitrate;
	VideoCodecCtx->gop_size    = (int)FMath::RoundToInt(EffectiveFps);
	VideoCodecCtx->max_b_frames = 0; // zero-latency

	// Explicitly signal the color space so all decoders (VLC, ffplay, hardware) agree.
	// sws_scale converts sRGB→YUV using BT.709 limited-range coefficients (see below),
	// so we stamp the matching values into the H.264 VUI / SPS here.
	VideoCodecCtx->color_range      = AVCOL_RANGE_MPEG;        // limited (16-235/16-240)
	VideoCodecCtx->color_primaries  = AVCOL_PRI_BT709;
	VideoCodecCtx->color_trc        = AVCOL_TRC_IEC61966_2_1;  // sRGB gamma (matches SCS_FinalColorLDR)
	VideoCodecCtx->colorspace       = AVCOL_SPC_BT709;

	if (FmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
		VideoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	// Encoder-specific options
	if (bUsingNvenc)
	{
		av_opt_set(VideoCodecCtx->priv_data, "preset", "p4",  0);
		av_opt_set(VideoCodecCtx->priv_data, "tune",   "ll",  0); // low latency
		av_opt_set(VideoCodecCtx->priv_data, "rc",     "cbr", 0);
		av_opt_set(VideoCodecCtx->priv_data, "gpu",    "0",   0);
	}
	else if (bWantH265)
	{
		// libx265 options
		av_opt_set(VideoCodecCtx->priv_data, "preset", TCHAR_TO_ANSI(*Config.H264Preset), 0);
		// libx265 uses x265-params for tune instead of top-level tune
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
	}

	// Phase 21E.1 — ROVER Baseline profile override
	if (Config.Streaming.bRoverCompat && !bUsingNvenc && !bWantH265)
	{
		VideoCodecCtx->profile = FF_PROFILE_H264_CONSTRAINED_BASELINE;
		av_opt_set(VideoCodecCtx->priv_data, "profile", "baseline", 0);
		UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: ROVER Baseline profile set"));
	}

	int Ret = avcodec_open2(VideoCodecCtx, Codec, nullptr);
	if (Ret < 0)
	{
		LogFfmpegError(Ret, TEXT("avcodec_open2 (H.264)"));
		return false;
	}

	Ret = avcodec_parameters_from_context(VideoStream->codecpar, VideoCodecCtx);
	if (Ret < 0)
	{
		LogFfmpegError(Ret, TEXT("avcodec_parameters_from_context"));
		return false;
	}

	VideoStream->time_base = VideoCodecCtx->time_base;

	// Allocate YUV frame for sws_scale output
	YuvFrame = av_frame_alloc();
	YuvFrame->format = AV_PIX_FMT_YUV420P;
	YuvFrame->width  = Config.CaptureWidth;
	YuvFrame->height = Config.CaptureHeight;
	av_frame_get_buffer(YuvFrame, 0);

	// Create sws context: BGRA → YUV420P (BT.709, limited range)
	SwsCtx = sws_getContext(
		Config.CaptureWidth, Config.CaptureHeight, AV_PIX_FMT_BGRA,
		Config.CaptureWidth, Config.CaptureHeight, AV_PIX_FMT_YUV420P,
		SWS_BILINEAR, nullptr, nullptr, nullptr);

	if (!SwsCtx)
	{
		UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: sws_getContext failed"));
		return false;
	}

	// CRITICAL: sws_getContext defaults to srcRange=0 (limited-range 16-235 RGB),
	// but GPU readback delivers full-range 0-255 RGB.  Without correcting this,
	// values below 16 or above 235 cause chroma overflow → pink/green banding.
	const int* BT709Coeffs = sws_getCoefficients(SWS_CS_ITU709);
	int ScsRet = sws_setColorspaceDetails(
		SwsCtx,
		BT709Coeffs, /*srcRange=*/1,   // full-range RGB in (0-255)
		BT709Coeffs, /*dstRange=*/0,   // limited-range YUV out (16-235/16-240)
		0, 1 << 16, 1 << 16);

	if (ScsRet < 0)
	{
		UE_LOG(LogCamSim, Error,
			TEXT("FVideoEncoder: sws_setColorspaceDetails FAILED (ret=%d). "
			     "Pink/green artifacts are likely."), ScsRet);
	}

	// Verify the color parameters actually took effect
	{
		int *InvTbl = nullptr, *Tbl = nullptr;
		int VerifySrcRange = -1, VerifyDstRange = -1;
		int Bri = 0, Con = 0, Sat = 0;
		sws_getColorspaceDetails(SwsCtx, &InvTbl, &VerifySrcRange,
		                         &Tbl, &VerifyDstRange, &Bri, &Con, &Sat);

		UE_LOG(LogCamSim, Log,
			TEXT("FVideoEncoder: sws colorspace verified → srcRange=%d dstRange=%d "
			     "(expected srcRange=1 dstRange=0)"),
			VerifySrcRange, VerifyDstRange);

		if (VerifySrcRange != 1 || VerifyDstRange != 0)
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("FVideoEncoder: sws color params NOT applied correctly! "
				     "srcRange=%d (want 1) dstRange=%d (want 0). "
				     "Retrying with context reinit..."),
				VerifySrcRange, VerifyDstRange);

			// Fallback: recreate the context using the explicit init path
			sws_freeContext(SwsCtx);
			SwsCtx = sws_alloc_context();
			if (SwsCtx)
			{
				av_opt_set_int(SwsCtx, "srcw",       Config.CaptureWidth,   0);
				av_opt_set_int(SwsCtx, "srch",       Config.CaptureHeight,  0);
				av_opt_set_int(SwsCtx, "src_format",  AV_PIX_FMT_BGRA,     0);
				av_opt_set_int(SwsCtx, "dstw",       Config.CaptureWidth,   0);
				av_opt_set_int(SwsCtx, "dsth",       Config.CaptureHeight,  0);
				av_opt_set_int(SwsCtx, "dst_format",  AV_PIX_FMT_YUV420P,  0);
				av_opt_set_int(SwsCtx, "sws_flags",   SWS_BILINEAR,        0);

				int InitRet = sws_init_context(SwsCtx, nullptr, nullptr);
				if (InitRet < 0)
				{
					UE_LOG(LogCamSim, Error,
						TEXT("FVideoEncoder: sws_init_context fallback failed (ret=%d)"), InitRet);
					sws_freeContext(SwsCtx);
					SwsCtx = nullptr;
					return false;
				}

				// Re-apply color space details on the freshly initialized context
				ScsRet = sws_setColorspaceDetails(
					SwsCtx,
					BT709Coeffs, /*srcRange=*/1,
					BT709Coeffs, /*dstRange=*/0,
					0, 1 << 16, 1 << 16);

				sws_getColorspaceDetails(SwsCtx, &InvTbl, &VerifySrcRange,
				                         &Tbl, &VerifyDstRange, &Bri, &Con, &Sat);
				UE_LOG(LogCamSim, Log,
					TEXT("FVideoEncoder: sws fallback → srcRange=%d dstRange=%d ret=%d"),
					VerifySrcRange, VerifyDstRange, ScsRet);
			}
		}

		// Final determination of whether color space settings were applied
		bSwsColorSpaceApplied = (VerifySrcRange == 1 && VerifyDstRange == 0);
		if (!bSwsColorSpaceApplied)
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("FVideoEncoder: sws color space NOT active — EncodeFrame will "
				     "pre-compress BGRA to limited range (16-235) to avoid pink/green"));
		}
	}

	// Stamp color properties on the YUV frame to match VUI and sws output
	YuvFrame->color_range     = AVCOL_RANGE_MPEG;
	YuvFrame->colorspace      = AVCOL_SPC_BT709;
	YuvFrame->color_primaries = AVCOL_PRI_BT709;
	YuvFrame->color_trc       = AVCOL_TRC_IEC61966_2_1;

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
	KlvStream->codecpar->codec_type = AVMEDIA_TYPE_DATA;
	KlvStream->codecpar->codec_id   = AV_CODEC_ID_SMPTE_KLV;
	KlvStream->codecpar->codec_tag  = MKTAG('K','L','V','A');
	KlvStream->time_base            = AVRational{1, 90000};

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
	RecordKlvStream->codecpar->codec_type = AVMEDIA_TYPE_DATA;
	RecordKlvStream->codecpar->codec_id   = AV_CODEC_ID_SMPTE_KLV;
	RecordKlvStream->codecpar->codec_tag  = MKTAG('K','L','V','A');
	RecordKlvStream->time_base            = AVRational{1, 90000};

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
	const TArray<FColor>& PixelData,
	const FCamSimTelemetry& Telemetry,
	uint64 FrameIdx)
{
	if (!bIsOpen || PixelData.Num() == 0) return;

	// Confirm the encoder is receiving frames (first 3 only to avoid spam).
	if (FrameIdx < 3)
	{
		UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: encoding frame %llu (%d pixels)"), FrameIdx, PixelData.Num());
	}

	// Convert BGRA → YUV420P
	av_frame_make_writable(YuvFrame);

	// IR/NVG fast path: frame is already grayscale after sensor post-process.
	// Y = R channel (all channels equal after ApplyIR/ApplyNVG), Cb=Cr=128.
	// Skips sws_scale entirely, saving 2-3ms per frame.
	const bool bGrayscaleMode = (Telemetry.SensorMode == 1 || Telemetry.SensorMode == 2);
	if (bGrayscaleMode)
	{
		const int32 W = Config.CaptureWidth;
		const int32 H = Config.CaptureHeight;
		const FColor* RESTRICT Src = PixelData.GetData();

		// Y plane: BT.601 luma from BGRA (limited range for MPEG compliance)
		for (int32 y = 0; y < H; ++y)
		{
			uint8* RESTRICT YRow = YuvFrame->data[0] + y * YuvFrame->linesize[0];
			const FColor* RESTRICT SrcRow = Src + y * W;
			for (int32 x = 0; x < W; ++x)
			{
				// For grayscale, R≈G≈B. Use R directly, map to limited range.
				YRow[x] = static_cast<uint8>(16 + (SrcRow[x].R * 219 + 127) / 255);
			}
		}
		// U/V planes: neutral chroma (128)
		const int32 HalfW = W / 2;
		const int32 HalfH = H / 2;
		FMemory::Memset(YuvFrame->data[1], 128, YuvFrame->linesize[1] * HalfH);
		FMemory::Memset(YuvFrame->data[2], 128, YuvFrame->linesize[2] * HalfH);
	}
	else
	{
	// Full color path: BGRA → YUV420P via sws_scale
	const uint8* SrcPtr = reinterpret_cast<const uint8*>(PixelData.GetData());
	TArray<uint8> CompressedBuf;

	// If sws color space details didn't take, sws_scale treats input as
	// limited-range RGB [16-235].  Pre-compress full-range [0-255] → [16-235]
	// to prevent chroma overflow that causes pink/green banding.
	if (!bSwsColorSpaceApplied)
	{
		const int32 NumBytes = PixelData.Num() * 4;
		CompressedBuf.SetNumUninitialized(NumBytes);
		for (int32 i = 0; i < NumBytes; ++i)
		{
			// Map [0,255] → [16,235]: out = 16 + in * 219/255
			CompressedBuf[i] = static_cast<uint8>(16 + (SrcPtr[i] * 219 + 127) / 255);
		}
		SrcPtr = CompressedBuf.GetData();
	}

	const uint8* SrcData[1] = { SrcPtr };
	int SrcLinesize[1]      = { Config.CaptureWidth * 4 };  // 4 bytes per BGRA pixel

	sws_scale(SwsCtx,
		SrcData, SrcLinesize,
		0, Config.CaptureHeight,
		YuvFrame->data, YuvFrame->linesize);
	} // end color path

	// Diagnostic: dump sample pixel values for first frame to verify conversion
	if (FrameIdx == 0 && PixelData.Num() > 0)
	{
		// Sample a pixel from near the center of the frame
		const int32 CX = Config.CaptureWidth / 2;
		const int32 CY = Config.CaptureHeight / 2;
		const int32 Idx = CY * Config.CaptureWidth + CX;
		const FColor& P = PixelData[Idx];
		const uint8 Y_val  = YuvFrame->data[0][CY * YuvFrame->linesize[0] + CX];
		const uint8 Cb_val = YuvFrame->data[1][(CY/2) * YuvFrame->linesize[1] + (CX/2)];
		const uint8 Cr_val = YuvFrame->data[2][(CY/2) * YuvFrame->linesize[2] + (CX/2)];
		UE_LOG(LogCamSim, Log,
			TEXT("FVideoEncoder: frame 0 center pixel (%d,%d): "
			     "BGRA=[B=%u G=%u R=%u A=%u] → YCbCr=[Y=%u Cb=%u Cr=%u]  "
			     "(linesize Y=%d U=%d V=%d)"),
			CX, CY, P.B, P.G, P.R, P.A, Y_val, Cb_val, Cr_val,
			YuvFrame->linesize[0], YuvFrame->linesize[1], YuvFrame->linesize[2]);
	}

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

	// Write KLV metadata packet for this frame
	WriteKlvPacket(Telemetry, FrameIdx);

	// Mark frame as successfully output (read by watchdog on game thread)
	++SuccessfulFrameCount;
}

// -------------------------------------------------------------------------
// WriteKlvPacket
// -------------------------------------------------------------------------

void FVideoEncoder::WriteKlvPacket(const FCamSimTelemetry& Telemetry, uint64 FrameIdx)
{
	TArray<uint8> KlvData = FKlvBuilder::BuildMisbST0601(Telemetry);
	if (KlvData.Num() == 0) return;

	AVPacket* KlvPkt = av_packet_alloc();
	if (!KlvPkt) return;

	av_new_packet(KlvPkt, KlvData.Num());
	FMemory::Memcpy(KlvPkt->data, KlvData.GetData(), KlvData.Num());

	// KLV PTS derived from video PTS via av_rescale_q (Phase 6)
	const int64 VideoPts = YuvFrame ? YuvFrame->pts : static_cast<int64>(FrameIdx);
	const int64 KlvPts = av_rescale_q(VideoPts, VideoCodecCtx->time_base, KlvStream->time_base);
	KlvPkt->pts          = KlvPts;
	KlvPkt->dts          = KlvPts;
	KlvPkt->duration     = av_rescale_q(1, VideoCodecCtx->time_base, KlvStream->time_base);
	KlvPkt->stream_index = KlvStream->index;

	// Duplicate KLV to local recording
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
	av_packet_free(&KlvPkt);
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
		Pkt->stream_index = VideoStream->index;
		av_interleaved_write_frame(FmtCtx, Pkt);
		av_packet_unref(Pkt);
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
	if (SwsCtx)        { sws_freeContext(SwsCtx); SwsCtx = nullptr; }
	if (YuvFrame)      { av_frame_free(&YuvFrame); }
	if (Pkt)           { av_packet_free(&Pkt); }
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
