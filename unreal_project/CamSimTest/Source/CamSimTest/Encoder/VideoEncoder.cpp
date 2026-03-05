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
	UE_LOG(LogCamSim, Log,
		TEXT("FVideoEncoder: H.264 %dx%d @ %.0ffps  bitrate=%d bps  preset=%s  tune=%s  -> %s"),
		Config.CaptureWidth, Config.CaptureHeight, Config.FrameRate,
		Config.VideoBitrate, *Config.H264Preset, *Config.H264Tune, *UdpUrl);
	return true;
}

// -------------------------------------------------------------------------
// OpenVideoStream
// -------------------------------------------------------------------------

bool FVideoEncoder::OpenVideoStream()
{
	const AVCodec* Codec = avcodec_find_encoder_by_name("libx264");
	if (!Codec)
	{
		Codec = avcodec_find_encoder(AV_CODEC_ID_H264);
	}
	if (!Codec)
	{
		UE_LOG(LogCamSim, Error, TEXT("FVideoEncoder: H.264 encoder not found"));
		return false;
	}
	UE_LOG(LogCamSim, Log, TEXT("FVideoEncoder: using encoder %s"), ANSI_TO_TCHAR(Codec->name));

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
	VideoCodecCtx->time_base   = AVRational{1, (int)FMath::RoundToInt(Config.FrameRate)};
	VideoCodecCtx->framerate   = AVRational{(int)FMath::RoundToInt(Config.FrameRate), 1};
	VideoCodecCtx->bit_rate    = Config.VideoBitrate;
	VideoCodecCtx->gop_size    = 30;
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

	// libx264 options
	av_opt_set(VideoCodecCtx->priv_data, "preset", TCHAR_TO_ANSI(*Config.H264Preset), 0);
	av_opt_set(VideoCodecCtx->priv_data, "tune",   TCHAR_TO_ANSI(*Config.H264Tune),   0);

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

	KlvStream->codecpar->codec_type = AVMEDIA_TYPE_DATA;
	KlvStream->codecpar->codec_id   = AV_CODEC_ID_SMPTE_KLV;
	KlvStream->codecpar->codec_tag  = MKTAG('K','L','V','A');
	KlvStream->time_base            = AVRational{1, 90000};

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

	// Convert BGRA → YUV420P via sws_scale
	av_frame_make_writable(YuvFrame);

	const uint8* SrcData[1] = { reinterpret_cast<const uint8*>(PixelData.GetData()) };
	int SrcLinesize[1]      = { Config.CaptureWidth * 4 };  // 4 bytes per BGRA pixel

	sws_scale(SwsCtx,
		SrcData, SrcLinesize,
		0, Config.CaptureHeight,
		YuvFrame->data, YuvFrame->linesize);

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

	// Set PTS in {1, fps} timebase
	YuvFrame->pts = static_cast<int64>(FrameIdx);

	// Send frame to encoder
	int Ret = avcodec_send_frame(VideoCodecCtx, YuvFrame);
	if (Ret < 0)
	{
		LogFfmpegError(Ret, TEXT("avcodec_send_frame"));
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

	// KLV PTS in 90 kHz timebase: frame_idx * 3000 (= 90000/30)
	const int64 KlvPts = static_cast<int64>(FrameIdx) * 3000;
	KlvPkt->pts          = KlvPts;
	KlvPkt->dts          = KlvPts;
	KlvPkt->duration     = 3000;
	KlvPkt->stream_index = KlvStream->index;

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
