/*
 * Interface to libaacplus for aac encoding
 * Copyright (c) 2010 tipok <piratfm@gmail.com>, 2021 WrapperDev
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Interface to libaacplus encoder.
 */

#include <aacplus.h>

#include "avcodec.h"
#include "internal.h"
#include "encode.h"
#include "libavutil/opt.h"
#include "profiles.h"
#include "codec_internal.h"

#define USE_AUDIOQUEUE
#define FRAMEPKT_DURATION
#define FRAME_DELAY

#ifdef USE_AUDIOQUEUE
#include "audio_frame_queue.h"
#endif

typedef struct aacPlusAudioContext {
    const AVClass *class;
    aacplusEncHandle aacplus_handle;
    unsigned long max_output_bytes;
    unsigned long samples_input;
    // unsigned int blank_samples;
    int ps_delay;
    int rs_delay;

#ifdef USE_AUDIOQUEUE
    AudioFrameQueue afq;
#endif

} aacPlusAudioContext;

static const AVOption aac_enc_options[] = {   
    // { "blank_samples", "Blank samples after encoding.", offsetof(aacPlusAudioContext, blank_samples), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 8, AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_ENCODING_PARAM },
    { "ps_delay", "Delay between PS samples.", offsetof(aacPlusAudioContext, ps_delay), AV_OPT_TYPE_INT, { .i64 = 0 }, -4096, 4096, AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_ENCODING_PARAM }, 
    { "rs_delay", "Delay between SBR resampling.", offsetof(aacPlusAudioContext, rs_delay), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 2048, AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_ENCODING_PARAM }, 
    FF_AAC_PROFILE_OPTS
    { NULL }
};

static const AVClass aac_enc_class = {
    .class_name = "libaac",
    .item_name  = av_default_item_name,
    .option     = aac_enc_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static av_cold int aacPlus_encode_init(AVCodecContext *avctx)
{
    aacPlusAudioContext *s = avctx->priv_data;
    aacplusEncConfiguration *aacplus_cfg;

    /* number of channels */
    if (avctx->ch_layout.nb_channels < 1 || avctx->ch_layout.nb_channels > 2) {
        av_log(avctx, AV_LOG_ERROR, "encoding %d channel(s) is not allowed\n", avctx->ch_layout.nb_channels);
        return AVERROR(EINVAL);
    }

    av_log(avctx, AV_LOG_DEBUG, "AAC Profile: %d\n", avctx->profile);
    if (avctx->profile != FF_PROFILE_AAC_LOW && avctx->profile != FF_PROFILE_AAC_HE && avctx->profile != FF_PROFILE_AAC_HE_V2 && avctx->profile != FF_PROFILE_UNKNOWN) {
        av_log(avctx, AV_LOG_ERROR, "invalid AAC profile: %d, only LC, HE-AAC, and HE-AAC V2 supported\n", avctx->profile);
        return AVERROR(EINVAL);
    }

    if (avctx->profile == FF_PROFILE_AAC_HE_V2 && avctx->strict_std_compliance > FF_COMPLIANCE_EXPERIMENTAL) {
        av_log(avctx, AV_LOG_ERROR, "eAAC+ encoder is currently experimental and sounds worse compared to AAC+. To use it anyways, set -strict to experimental.\n");
        return AVERROR_EXPERIMENTAL;
    }
    //s->aacplus_handle = aacplusEncOpen(avctx->sample_rate, avctx->ch_layout.nb_channels,
    //                                   &s->samples_input, &s->max_output_bytes);
    s->aacplus_handle = aacplusEncOpenW(avctx->sample_rate, avctx->ch_layout.nb_channels,
                                         &s->samples_input, &s->max_output_bytes, (avctx->profile != FF_PROFILE_AAC_LOW && avctx->profile != FF_PROFILE_UNKNOWN));

    if (!s->aacplus_handle) {
        av_log(avctx, AV_LOG_ERROR, "can't open encoder\n");
        return AVERROR(EINVAL);
    }

    /* check aacplus version */
    aacplus_cfg = aacplusEncGetCurrentConfiguration(s->aacplus_handle);

    aacplus_cfg->bitRate = avctx->bit_rate;
    aacplus_cfg->bandWidth = avctx->cutoff;
    aacplus_cfg->outputFormat = !(avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER);
    aacplus_cfg->inputFormat = avctx->sample_fmt == AV_SAMPLE_FMT_FLT ? AACPLUS_INPUT_FLOAT : AACPLUS_INPUT_16BIT;
    aacplus_cfg->psDelay = s->ps_delay;
    if (s->rs_delay >= 0) aacplus_cfg->rsDelay = s->rs_delay;

    /*
    if (s->blank_samples != -1) {
        aacplus_cfg->
    }
    */

    if (avctx->profile == FF_PROFILE_AAC_HE) {
        aacplus_cfg->heV2Override = 0;
    } else if (avctx->profile == FF_PROFILE_AAC_HE_V2) {
        aacplus_cfg->heV2Override = 1;
    }    
    
    if (!aacplusEncSetConfiguration(s->aacplus_handle, aacplus_cfg)) {
        av_log(avctx, AV_LOG_ERROR, "libaacplus doesn't support this output format!\n");
        return AVERROR(EINVAL);
    }

    avctx->frame_size = s->samples_input / avctx->ch_layout.nb_channels;
#ifdef FRAME_DELAY
    avctx->initial_padding = (avctx->profile == FF_PROFILE_AAC_LOW || avctx->profile == FF_PROFILE_UNKNOWN) ? avctx->frame_size*2 : avctx->frame_size*3;
#else
    avctx->initial_padding = aacplusEncGetDelay(s->aacplus_handle);
#endif
    av_log(avctx, AV_LOG_TRACE, "AAC Initial Padding: %d\n", avctx->initial_padding);

#ifdef USE_AUDIOQUEUE
    ff_af_queue_init(avctx, &s->afq);
#endif

    /* Set decoder specific info */
    avctx->extradata_size = 0;
    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {

        unsigned char *buffer = NULL;
        unsigned long decoder_specific_info_size;

        if (aacplusEncGetDecoderSpecificInfo(s->aacplus_handle, &buffer,
                                           &decoder_specific_info_size) == 1) {
            avctx->extradata = av_malloc(decoder_specific_info_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!avctx->extradata) {
                free(buffer);
                return AVERROR(ENOMEM);
            }
            avctx->extradata_size = decoder_specific_info_size;
            memcpy(avctx->extradata, buffer, avctx->extradata_size);
        }
        free(buffer);
    }
    return 0;
}

#ifdef USE_AUDIOQUEUE
static int aacPlus_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                                const AVFrame *frame, int *got_packet)
{
    aacPlusAudioContext *s = avctx->priv_data;
    int32_t *input_buffer;
    int ret;    
    int flush;        
#ifndef FRAMEPKT_DURATION
    int64_t fp_dur;
#endif

    if ((ret = ff_alloc_packet(avctx, pkt, s->max_output_bytes)) < 0)
        return ret;
    
    if (!frame) {        
        flush = aacplusEncEncode(s->aacplus_handle, NULL, 0, pkt->data, pkt->size);
        av_log(avctx, AV_LOG_TRACE, "Flush: %d\n", flush);
        if (!flush) return 0;
        pkt->size = flush;        
    } else {
        input_buffer = (int32_t *)frame->data[0];        
        if ((ret = ff_af_queue_add(&s->afq, frame)) < 0) 
            return ret;
        
        pkt->size = aacplusEncEncodeW(s->aacplus_handle, input_buffer, frame->nb_samples*avctx->ch_layout.nb_channels, pkt->data, pkt->size, frame->nb_samples<avctx->frame_size);      
        av_log(avctx, AV_LOG_TRACE, "EncodeData: %d, Input Size: %d, Padding Required: %d, Encoder Frame Size: %d\n", pkt->size, frame->nb_samples, frame->nb_samples<avctx->frame_size, avctx->frame_size);
    }

    *got_packet = 1;
    //if (frame) pkt->pts      = frame->pts;
#ifndef FRAMEPKT_DURATION
    ff_af_queue_remove(&s->afq, avctx->frame_size, &pkt->pts, &fp_dur);
    av_log(avctx, AV_LOG_TRACE, "FP PTS: %ld\n", pkt->pts);
    av_log(avctx, AV_LOG_TRACE, "FP Duration: %ld\n", fp_dur);
#else
    ff_af_queue_remove(&s->afq, avctx->frame_size, &pkt->pts, &pkt->duration);
    av_log(avctx, AV_LOG_TRACE, "FP PTS: %ld\n", pkt->pts);
    av_log(avctx, AV_LOG_TRACE, "FP Duration: %ld\n", pkt->duration);
#endif

    return 0;
}
#else
static int aacPlus_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                                const AVFrame *frame, int *got_packet)
{
    aacPlusAudioContext *s = avctx->priv_data;
    #if 0
    int32_t *inp_buffer = (int32_t *)frame->data[0];
    int32_t *input_buffer = NULL;    
    #else
    int32_t *input_buffer = (int32_t *)frame->data[0];
    #endif

    int ret;    

    #if 0
    av_fast_padded_malloc(&input_buffer, (frame->nb_samples*avctx->ch_layout.nb_channels), s->samples_input);    
    if (!input_buffer) return AVERROR(ENOMEM);    

    for (int fi = 0; fi<(frame->nb_samples*avctx->ch_layout.nb_channels); fi++) {
        av_log(avctx, AV_LOG_TRACE, "%d audio bytes processed for AAC encoding\n", fi);
        input_buffer[fi] = inp_buffer[fi];
    }    

    //memcpy(input_buffer, inp_buffer, frame->nb_samples*avctx->ch_layout.nb_channels);
    #endif

    if ((ret = ff_alloc_packet(avctx, pkt, s->max_output_bytes)) < 0)
        return ret;

    pkt->size = aacplusEncEncodeW(s->aacplus_handle, input_buffer,
                                 frame->nb_samples*avctx->ch_layout.nb_channels, pkt->data, pkt->size, frame->nb_samples<avctx->frame_size);

    #if 0
    if (input_buffer) free(input_buffer);                                 
    #endif

    //av_log(avctx, AV_LOG_TRACE, "libAAC Encode Data: %d:%d - %d: %d - %d\n", frame->nb_samples, frame->nb_samples * avctx->ch_layout.nb_channels, s->samples_input, pkt->size, aacplusEncGetRequiredFrames(s->aacplus_handle));

    *got_packet   = 1;
    pkt->pts      = frame->pts;
    av_log(avctx, AV_LOG_TRACE, "FP PTS: %ld\n", pkt->pts);
    return 0;
}
#endif


static av_cold int aacPlus_encode_close(AVCodecContext *avctx)
{
    aacPlusAudioContext *s = avctx->priv_data;

    av_freep(&avctx->extradata);
    aacplusEncClose(s->aacplus_handle);

#ifdef USE_AUDIOQUEUE
    ff_af_queue_close(&s->afq);
#endif

    return 0;
}

static const FFCodecDefault defaults[] = {
    { "b", "128000" },
    { NULL }
};

static const AVProfile profiles[] = {
    { FF_PROFILE_AAC_LOW,   "LC"       },
    { FF_PROFILE_AAC_HE,    "HE-AAC"   },
    { FF_PROFILE_AAC_HE_V2, "HE-AACv2" },
    { FF_PROFILE_UNKNOWN },
};

static const int aac_sample_rates[] = {
    96000, 88200, 64000, 48000, 44100, 32000,
    24000, 22050, 16000, 12000, 11025, 8000, 0
};

const FFCodec ff_libaacplus_encoder = {
    .p.name           = "libaac",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Modified version of (3GPP/libaacplus) AAC (Advanced Audio Codec)"),
    .p.type           = AVMEDIA_TYPE_AUDIO,
    .p.id             = AV_CODEC_ID_AAC,
    .priv_data_size = sizeof(aacPlusAudioContext),
#ifdef USE_AUDIOQUEUE
    .p.capabilities   = AV_CODEC_CAP_SMALL_LAST_FRAME | AV_CODEC_CAP_DELAY,
#else
    .p.capabilities   = AV_CODEC_CAP_SMALL_LAST_FRAME,
#endif
    .init           = aacPlus_encode_init,
    FF_CODEC_ENCODE_CB(aacPlus_encode_frame),
    .close          = aacPlus_encode_close,
    .defaults       = defaults,
    .p.sample_fmts    = (const enum AVSampleFormat[]){ AV_SAMPLE_FMT_S16,
                                                     AV_SAMPLE_FMT_FLT,
                                                     AV_SAMPLE_FMT_NONE },
    .p.profiles       = profiles,    
    .p.priv_class            = &aac_enc_class,
    .p.supported_samplerates = aac_sample_rates,
    .p.channel_layouts = (const uint64_t[]) { AV_CH_LAYOUT_MONO,
                                            AV_CH_LAYOUT_STEREO,
                                            0 },
    .p.wrapper_name          = "libaac",
};
