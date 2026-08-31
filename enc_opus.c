/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / libopus Opus encoder filter - talks
 *  directly to libopus's native API (opus_encoder_create/opus_encode/
 *  opus_encoder_destroy). Reuses the same libopus.a already built and
 *  used by filters/libopus's decoder (dec_opus.c). Used as a progressive/
 *  MSE-compatible audio path: Opus-in-mp4 is MSE-supported (unlike MP3,
 *  which Chrome's MSE only accepts as a bare "audio/mpeg" elementary
 *  stream, never inside an ISOBMFF/mp4 container), and this project has
 *  no working native AAC encoder.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <gpac/mpeg4_odf.h>
#include <string.h>

#include <opus.h>

/* Opus only supports these fixed sample rates */
#define OPUSENC_TARGET_RATE 48000
/* 20ms frame at 48kHz - a standard, widely-used Opus frame size */
#define OPUSENC_FRAME_SAMPLES 960

typedef struct
{
	/* opts */
	u32 bitrate;

	GF_FilterPid *ipid, *opid;
	u32 sample_rate, num_channels;
	u64 samples_done;
	Bool config_sent;

	OpusEncoder *enc;

	s16 *pcm_buf;
	u32 pcm_buf_len, pcm_buf_alloc;
} GF_OpusEncCtx;

static GF_Err opusenc_send_config(GF_OpusEncCtx *ctx)
{
	GF_OpusConfig cfg;
	u8 *dsi;
	u32 dsi_size;
	GF_Err e;
	opus_int32 lookahead = 0;

	memset(&cfg, 0, sizeof(cfg));
	opus_encoder_ctl(ctx->enc, OPUS_GET_LOOKAHEAD(&lookahead));

	cfg.version = 1;
	cfg.OutputChannelCount = (u8)ctx->num_channels;
	cfg.PreSkip = (u16)lookahead;
	cfg.InputSampleRate = ctx->sample_rate;
	cfg.OutputGain = 0;
	cfg.ChannelMappingFamily = 0;

	dsi = NULL;
	dsi_size = 0;
	e = gf_odf_opus_cfg_write(&cfg, &dsi, &dsi_size);
	if (e != GF_OK)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[OpusEnc] Failed to write dOps config\n"));
		return e;
	}

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_DECODER_CONFIG, &PROP_DATA_NO_COPY(dsi, dsi_size));

	ctx->config_sent = GF_TRUE;
	return GF_OK;
}

static GF_Err opusenc_setup(GF_OpusEncCtx *ctx)
{
	int error = 0;

	if (ctx->enc)
	{
		opus_encoder_destroy(ctx->enc);
		ctx->enc = NULL;
	}

	ctx->enc = opus_encoder_create(OPUSENC_TARGET_RATE, ctx->num_channels, OPUS_APPLICATION_AUDIO, &error);
	if (!ctx->enc || (error != OPUS_OK))
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[OpusEnc] Failed to open encoder (%d)\n", error));
		return GF_IO_ERR;
	}

	opus_encoder_ctl(ctx->enc, OPUS_SET_BITRATE(ctx->bitrate * 1000));

	ctx->samples_done = 0;
	ctx->pcm_buf_len = 0;
	ctx->config_sent = GF_FALSE;
	return opusenc_send_config(ctx);
}

static GF_Err opusenc_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *prop;
	GF_OpusEncCtx *ctx = (GF_OpusEncCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		if (ctx->enc)
		{
			opus_encoder_destroy(ctx->enc);
			ctx->enc = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;

	if (!ctx->opid)
	{
		ctx->opid = gf_filter_pid_new(filter);
	}
	gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_OPUS));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, NULL);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_UNFRAMED, NULL);

	gf_filter_set_name(filter, "encopus:libopus");

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_SAMPLE_RATE);
	if (!prop)
		return GF_OK;
	/* Opus only encodes at fixed rates (8/12/16/24/48kHz); this filter
	 * only supports sources already at 48kHz (matches this project's
	 * existing decoders, e.g. Vorbis) - no resampler is wired in */
	if (prop->value.uint != OPUSENC_TARGET_RATE)
	{
		gf_filter_pid_negotiate_property(pid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(OPUSENC_TARGET_RATE));
		return GF_OK;
	}
	ctx->sample_rate = prop->value.uint;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_NUM_CHANNELS);
	if (!prop)
		return GF_OK;
	ctx->num_channels = prop->value.uint;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_AUDIO_FORMAT);
	if (prop && (prop->value.uint != GF_AUDIO_FMT_S16))
	{
		gf_filter_pid_negotiate_property(pid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));
		return GF_OK;
	}

	return opusenc_setup(ctx);
}

static GF_Err opusenc_encode_frame(GF_OpusEncCtx *ctx, const s16 *pcm)
{
	GF_FilterPacket *dst_pck;
	u8 *output;
	unsigned char packet[4000];
	opus_int32 ret;

	ret = opus_encode(ctx->enc, pcm, OPUSENC_FRAME_SAMPLES, packet, sizeof(packet));
	if (ret < 0)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[OpusEnc] Encoding failed (%d)\n", ret));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	if (ret > 0)
	{
		dst_pck = gf_filter_pck_new_alloc(ctx->opid, ret, &output);
		if (dst_pck)
		{
			memcpy(output, packet, ret);
			gf_filter_pck_set_cts(dst_pck, ctx->samples_done);
			gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
			gf_filter_pck_send(dst_pck);
		}
	}
	ctx->samples_done += OPUSENC_FRAME_SAMPLES;
	return GF_OK;
}

static GF_Err opusenc_process(GF_Filter *filter)
{
	GF_OpusEncCtx *ctx = (GF_OpusEncCtx *)gf_filter_get_udta(filter);
	GF_FilterPacket *pck;
	const u8 *in_data;
	u32 size, num_samples, frame_stride;

	frame_stride = OPUSENC_FRAME_SAMPLES * ctx->num_channels;

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			if (ctx->enc && (ctx->pcm_buf_len >= ctx->num_channels))
			{
				/* pad the last partial frame with silence */
				u32 pad = frame_stride - ctx->pcm_buf_len;
				memset(ctx->pcm_buf + ctx->pcm_buf_len, 0, pad * sizeof(s16));
				opusenc_encode_frame(ctx, ctx->pcm_buf);
				ctx->pcm_buf_len = 0;
			}
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}

	if (!ctx->enc)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_SERVICE_ERROR;
	}

	in_data = (const u8 *)gf_filter_pck_get_data(pck, &size);
	if (!in_data || !ctx->num_channels)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OK;
	}

	num_samples = (size / 2) / ctx->num_channels;
	{
		u32 needed = ctx->pcm_buf_len + num_samples * ctx->num_channels;
		if (needed > ctx->pcm_buf_alloc)
		{
			ctx->pcm_buf_alloc = needed + frame_stride;
			ctx->pcm_buf = (s16 *)gf_realloc(ctx->pcm_buf, ctx->pcm_buf_alloc * sizeof(s16));
		}
	}
	memcpy(ctx->pcm_buf + ctx->pcm_buf_len, in_data, num_samples * ctx->num_channels * sizeof(s16));
	ctx->pcm_buf_len += num_samples * ctx->num_channels;

	gf_filter_pid_drop_packet(ctx->ipid);

	while (ctx->pcm_buf_len >= frame_stride)
	{
		opusenc_encode_frame(ctx, ctx->pcm_buf);
		ctx->pcm_buf_len -= frame_stride;
		if (ctx->pcm_buf_len)
			memmove(ctx->pcm_buf, ctx->pcm_buf + frame_stride, ctx->pcm_buf_len * sizeof(s16));
	}

	return GF_OK;
}

static void opusenc_finalize(GF_Filter *filter)
{
	GF_OpusEncCtx *ctx = (GF_OpusEncCtx *)gf_filter_get_udta(filter);
	if (ctx->enc)
		opus_encoder_destroy(ctx->enc);
	if (ctx->pcm_buf)
		gf_free(ctx->pcm_buf);
}

static const GF_FilterCapability OpusEncCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_AUDIO_FORMAT, GF_AUDIO_FMT_S16),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_OPUS),
};

#define OFFS(_n) #_n, offsetof(GF_OpusEncCtx, _n)
static GF_FilterArgs OpusEncArgs[] =
	{
		{OFFS(bitrate), "target bitrate in kbps", GF_PROP_UINT, "96", NULL, GF_FS_ARG_HINT_ADVANCED},
		{0}};

GF_FilterRegister OpusEncRegister = {
	.name = "encopus",
	GF_FS_SET_DESCRIPTION("Opus audio encoder (native libopus)")
		GF_FS_SET_HELP("This filter encodes a raw S16 PCM audio PID (48kHz) to Opus by calling libopus's "
					   "native API directly.")
			.private_size = sizeof(GF_OpusEncCtx),
	.args = OpusEncArgs,
	SETCAPS(OpusEncCaps),
	.configure_pid = opusenc_configure_pid,
	.process = opusenc_process,
	.finalize = opusenc_finalize,
};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE dynCall_encopus_register(GF_FilterSession *session)
{
	return &OpusEncRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_encopus(void) {
    gf_filter_auto_register("encopus", dynCall_encopus_register);
}
