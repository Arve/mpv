/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <stdbool.h>
#include <sys/types.h>

#include <libavutil/common.h>
#include <libavutil/opt.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/pixdesc.h>

#include "compat/libav.h"

#include "talloc.h"
#include "config.h"
#include "core/mp_msg.h"
#include "core/options.h"
#include "core/av_opts.h"

#include "compat/mpbswap.h"
#include "video/fmt-conversion.h"

#include "vd.h"
#include "video/img_format.h"
#include "video/mp_image_pool.h"
#include "video/filter/vf.h"
#include "demux/stheader.h"
#include "demux/demux_packet.h"
#include "core/codec-cfg.h"
#include "osdep/numcores.h"
#include "video/csputils.h"

static const vd_info_t info = {
    "libavcodec video codecs",
    "ffmpeg",
    "",
    "",
    "native codecs",
    .print_name = "libavcodec",
};

#include "libavcodec/avcodec.h"
#include "lavc.h"

#if AVPALETTE_SIZE != MP_PALETTE_SIZE
#error palette too large, adapt video/mp_image.h:MP_PALETTE_SIZE
#endif

#include "core/m_option.h"

static int init_avctx(sh_video_t *sh, AVCodec *lavc_codec, struct hwdec *hwdec);
static void uninit_avctx(sh_video_t *sh);
static int get_buffer_hwdec(AVCodecContext *avctx, AVFrame *pic);
static void release_buffer_hwdec(AVCodecContext *avctx, AVFrame *pic);
static void draw_slice_hwdec(struct AVCodecContext *s, const AVFrame *src,
                             int offset[4], int y, int type, int height);

static enum PixelFormat get_format_hwdec(struct AVCodecContext *avctx,
                                         const enum PixelFormat *pix_fmt);

static void uninit(struct sh_video *sh);

const m_option_t lavc_decode_opts_conf[] = {
    OPT_INTRANGE("bug", lavc_param.workaround_bugs, 0, -1, 999999),
    OPT_FLAG_ON("gray", lavc_param.gray, 0),
    OPT_INTRANGE("idct", lavc_param.idct_algo, 0, 0, 99),
    OPT_INTRANGE("ec", lavc_param.error_concealment, 0, 0, 99),
    OPT_INTRANGE("debug", lavc_param.debug, 0, 0, 9999999),
    OPT_INTRANGE("vismv", lavc_param.vismv, 0, 0, 9999999),
    OPT_INTRANGE("st", lavc_param.skip_top, 0, 0, 999),
    OPT_INTRANGE("sb", lavc_param.skip_bottom, 0, 0, 999),
    OPT_FLAG_CONSTANTS("fast", lavc_param.fast, 0, 0, CODEC_FLAG2_FAST),
    OPT_STRING("skiploopfilter", lavc_param.skip_loop_filter_str, 0),
    OPT_STRING("skipidct", lavc_param.skip_idct_str, 0),
    OPT_STRING("skipframe", lavc_param.skip_frame_str, 0),
    OPT_INTRANGE("threads", lavc_param.threads, 0, 0, 16),
    OPT_FLAG_CONSTANTS("bitexact", lavc_param.bitexact, 0, 0, CODEC_FLAG_BITEXACT),
    OPT_STRING("o", lavc_param.avopt, 0),
    {NULL, NULL, 0, 0, 0, 0, NULL}
};

// keep in sync with --hwdec option
enum hwdec_type {
    HWDEC_NONE = 0,
    HWDEC_VDPAU = 1,
    HWDEC_VDA = 2,
    HWDEC_CRYSTALHD = 3,
};

struct hwdec {
    enum hwdec_type api;
    char *codec, *hw_codec;
};

static const struct hwdec hwdec[] = {
    {HWDEC_VDPAU,       "h264",         "h264_vdpau"},
    {HWDEC_VDPAU,       "wmv3",         "wmv3_vdpau"},
    {HWDEC_VDPAU,       "vc1",          "vc1_vdpau"},
    {HWDEC_VDPAU,       "mpegvideo",    "mpegvideo_vdpau"},
    {HWDEC_VDPAU,       "mpeg1video",   "mpeg1video_vdpau"},
    {HWDEC_VDPAU,       "mpeg2video",   "mpegvideo_vdpau"},
    {HWDEC_VDPAU,       "mpeg2",        "mpeg2_vdpau"},
    {HWDEC_VDPAU,       "mpeg4",        "mpeg4_vdpau"},

    {HWDEC_VDA,         "h264",         "h264_vda"},

    {HWDEC_CRYSTALHD,   "mpeg2",        "mpeg2_crystalhd"},
    {HWDEC_CRYSTALHD,   "msmpeg4",      "msmpeg4_crystalhd"},
    {HWDEC_CRYSTALHD,   "wmv3",         "wmv3_crystalhd"},
    {HWDEC_CRYSTALHD,   "vc1",          "vc1_crystalhd"},
    {HWDEC_CRYSTALHD,   "h264",         "h264_crystalhd"},
    {HWDEC_CRYSTALHD,   "mpeg4",        "mpeg4_crystalhd"},

    {0}
};

static struct hwdec *find_hwcodec(enum hwdec_type api, const char *codec)
{
    for (int n = 0; hwdec[n].api; n++) {
        if (hwdec[n].api == api && strcmp(hwdec[n].codec, codec) == 0)
            return (struct hwdec *)&hwdec[n];
    }
    return NULL;
}

static enum AVDiscard str2AVDiscard(char *str)
{
    if (!str)                               return AVDISCARD_DEFAULT;
    if (strcasecmp(str, "none"   ) == 0)    return AVDISCARD_NONE;
    if (strcasecmp(str, "default") == 0)    return AVDISCARD_DEFAULT;
    if (strcasecmp(str, "nonref" ) == 0)    return AVDISCARD_NONREF;
    if (strcasecmp(str, "bidir"  ) == 0)    return AVDISCARD_BIDIR;
    if (strcasecmp(str, "nonkey" ) == 0)    return AVDISCARD_NONKEY;
    if (strcasecmp(str, "all"    ) == 0)    return AVDISCARD_ALL;
    mp_msg(MSGT_DECVIDEO, MSGL_ERR, "Unknown discard value %s\n", str);
    return AVDISCARD_DEFAULT;
}

static int init(sh_video_t *sh)
{
    vd_ffmpeg_ctx *ctx;
    AVCodec *lavc_codec = NULL;

    ctx = sh->context = talloc_zero(NULL, vd_ffmpeg_ctx);
    ctx->non_dr1_pool = talloc_steal(ctx, mp_image_pool_new(16));

    if (sh->codec->dll) {
        lavc_codec = avcodec_find_decoder_by_name(sh->codec->dll);
        if (!lavc_codec) {
            mp_tmsg(MSGT_DECVIDEO, MSGL_ERR,
                    "Cannot find codec '%s' in libavcodec...\n",
                    sh->codec->dll);
            uninit(sh);
            return 0;
        }
    } else if (sh->libav_codec_id) {
        lavc_codec = avcodec_find_decoder(sh->libav_codec_id);
        if (!lavc_codec) {
            mp_tmsg(MSGT_DECVIDEO, MSGL_INFO, "Libavcodec has no decoder "
                   "for this codec\n");
            uninit(sh);
            return 0;
        }
    }
    if (!lavc_codec) {
        uninit(sh);
        return 0;
    }

    struct hwdec *hwdec = find_hwcodec(sh->opts->hwdec_api, lavc_codec->name);
    if (hwdec) {
        AVCodec *lavc_hwcodec = avcodec_find_decoder_by_name(hwdec->hw_codec);
        if (lavc_hwcodec) {
            ctx->software_fallback = lavc_codec;
            lavc_codec = lavc_hwcodec;
        } else {
            hwdec = NULL;
            mp_tmsg(MSGT_DECVIDEO, MSGL_WARN, "Using software decoding.\n");
        }
    }

    if (!init_avctx(sh, lavc_codec, hwdec)) {
        mp_tmsg(MSGT_DECVIDEO, MSGL_ERR, "Error initializing hardware "
                "decoding, falling back to software decoding.\n");
        lavc_codec = ctx->software_fallback;
        ctx->software_fallback = NULL;
        if (!init_avctx(sh, lavc_codec, NULL)) {
            uninit(sh);
            return 0;
        }
    }
    return 1;
}

static int init_avctx(sh_video_t *sh, AVCodec *lavc_codec, struct hwdec *hwdec)
{
    vd_ffmpeg_ctx *ctx = sh->context;
    struct lavc_param *lavc_param = &sh->opts->lavc_param;

    sh->codecname = lavc_codec->long_name;
    if (!sh->codecname)
        sh->codecname = lavc_codec->name;

    ctx->do_dr1 = ctx->do_hw_dr1 = 0;
    ctx->pix_fmt = PIX_FMT_NONE;
    ctx->vo_initialized = 0;
    ctx->hwdec = hwdec;
    ctx->pic = avcodec_alloc_frame();
    ctx->avctx = avcodec_alloc_context3(lavc_codec);
    AVCodecContext *avctx = ctx->avctx;
    avctx->opaque = sh;
    avctx->codec_type = AVMEDIA_TYPE_VIDEO;
    avctx->codec_id = lavc_codec->id;

    avctx->thread_count = lavc_param->threads;

    if (ctx->hwdec && ctx->hwdec->api == HWDEC_VDPAU) {
        assert(lavc_codec->capabilities & CODEC_CAP_HWACCEL_VDPAU);
        ctx->do_hw_dr1         = true;
        avctx->thread_count    = 1;
        avctx->get_format      = get_format_hwdec;
        avctx->get_buffer      = get_buffer_hwdec;
        avctx->release_buffer  = release_buffer_hwdec;
        if (ctx->hwdec->api == HWDEC_VDPAU) {
            avctx->draw_horiz_band = draw_slice_hwdec;
            avctx->slice_flags =
                SLICE_FLAG_CODED_ORDER | SLICE_FLAG_ALLOW_FIELD;
        }
    } else if (lavc_codec->capabilities & CODEC_CAP_DR1) {
        ctx->do_dr1            = true;
        avctx->get_buffer      = mp_codec_get_buffer;
        avctx->release_buffer  = mp_codec_release_buffer;
    }

    if (avctx->thread_count == 0) {
        int threads = default_thread_count();
        if (threads < 1) {
            mp_msg(MSGT_DECVIDEO, MSGL_WARN, "[VD_FFMPEG] Could not determine "
                   "thread count to use, defaulting to 1.\n");
            threads = 1;
        }
        threads = FFMIN(threads, 16);
        avctx->thread_count = threads;
    }

    avctx->flags |= lavc_param->bitexact;

    avctx->coded_width = sh->disp_w;
    avctx->coded_height = sh->disp_h;
    avctx->workaround_bugs = lavc_param->workaround_bugs;
    if (lavc_param->gray)
        avctx->flags |= CODEC_FLAG_GRAY;
    avctx->flags2 |= lavc_param->fast;
    avctx->codec_tag = sh->format;
    if (sh->gsh->lavf_codec_tag)
        avctx->codec_tag = sh->gsh->lavf_codec_tag;
    avctx->stream_codec_tag = sh->video.fccHandler;
    avctx->idct_algo = lavc_param->idct_algo;
    avctx->error_concealment = lavc_param->error_concealment;
    avctx->debug = lavc_param->debug;
    if (lavc_param->debug)
        av_log_set_level(AV_LOG_DEBUG);
    avctx->debug_mv = lavc_param->vismv;
    avctx->skip_top   = lavc_param->skip_top;
    avctx->skip_bottom = lavc_param->skip_bottom;
    avctx->skip_loop_filter = str2AVDiscard(lavc_param->skip_loop_filter_str);
    avctx->skip_idct = str2AVDiscard(lavc_param->skip_idct_str);
    avctx->skip_frame = str2AVDiscard(lavc_param->skip_frame_str);

    if (lavc_param->avopt) {
        if (parse_avopts(avctx, lavc_param->avopt) < 0) {
            mp_msg(MSGT_DECVIDEO, MSGL_ERR,
                   "Your options /%s/ look like gibberish to me pal\n",
                   lavc_param->avopt);
            uninit(sh);
            return 0;
        }
    }

    // Do this after the above avopt handling in case it changes values
    ctx->skip_frame = avctx->skip_frame;

    mp_dbg(MSGT_DECVIDEO, MSGL_DBG2,
           "libavcodec.size: %d x %d\n", avctx->width, avctx->height);
    switch (sh->format) {
    case mmioFOURCC('S','V','Q','3'):
    case mmioFOURCC('A','V','R','n'):
    case mmioFOURCC('M','J','P','G'):
        /* AVRn stores huffman table in AVI header */
        /* Pegasus MJPEG stores it also in AVI header, but it uses the common
         * MJPG fourcc :( */
        if (!sh->bih || sh->bih->biSize <= sizeof(*sh->bih))
            break;
        av_opt_set_int(avctx, "extern_huff", 1, AV_OPT_SEARCH_CHILDREN);
        avctx->extradata_size = sh->bih->biSize - sizeof(*sh->bih);
        avctx->extradata = av_mallocz(avctx->extradata_size +
                                      FF_INPUT_BUFFER_PADDING_SIZE);
        memcpy(avctx->extradata, sh->bih + 1, avctx->extradata_size);
        break;

    case mmioFOURCC('R','V','1','0'):
    case mmioFOURCC('R','V','1','3'):
    case mmioFOURCC('R','V','2','0'):
    case mmioFOURCC('R','V','3','0'):
    case mmioFOURCC('R','V','4','0'):
        if (sh->bih->biSize < sizeof(*sh->bih) + 8) {
            // only 1 packet per frame & sub_id from fourcc
            avctx->extradata_size = 8;
            avctx->extradata = av_mallocz(avctx->extradata_size +
                                          FF_INPUT_BUFFER_PADDING_SIZE);
            ((uint32_t *)avctx->extradata)[0] = 0;
            ((uint32_t *)avctx->extradata)[1] =
                    sh->format == mmioFOURCC('R','V','1','3') ?
                    0x10003001 : 0x10000000;
        } else {
            // has extra slice header (demux_rm or rm->avi streamcopy)
            avctx->extradata_size = sh->bih->biSize - sizeof(*sh->bih);
            avctx->extradata = av_mallocz(avctx->extradata_size +
                                          FF_INPUT_BUFFER_PADDING_SIZE);
            memcpy(avctx->extradata, sh->bih + 1, avctx->extradata_size);
        }
        break;

    case MKTAG('M', 'P', 'v', 'f'):
        avctx->codec_tag = 0;
        avctx->pix_fmt = imgfmt2pixfmt(sh->imgfmt);
        break;
    case MKTAG('M', 'P', 'r', 'v'):
        avctx->codec_tag = sh->imgfmt;
        break;

    default:
        if (!sh->bih || sh->bih->biSize <= sizeof(*sh->bih))
            break;
        avctx->extradata_size = sh->bih->biSize - sizeof(*sh->bih);
        avctx->extradata = av_mallocz(avctx->extradata_size +
                                      FF_INPUT_BUFFER_PADDING_SIZE);
        memcpy(avctx->extradata, sh->bih + 1, avctx->extradata_size);
        break;
    }

    if (sh->bih)
        avctx->bits_per_coded_sample = sh->bih->biBitCount;

    /* open it */
    if (avcodec_open2(avctx, lavc_codec, NULL) < 0) {
        mp_tmsg(MSGT_DECVIDEO, MSGL_ERR, "Could not open codec.\n");
        uninit_avctx(sh);
        return 0;
    }
    return 1;
}

static void uninit_avctx(sh_video_t *sh)
{
    vd_ffmpeg_ctx *ctx = sh->context;
    AVCodecContext *avctx = ctx->avctx;

    sh->codecname = NULL;

    if (avctx) {
        if (avctx->codec && avcodec_close(avctx) < 0)
            mp_tmsg(MSGT_DECVIDEO, MSGL_ERR, "Could not close codec.\n");

        av_freep(&avctx->extradata);
        av_freep(&avctx->slice_offset);
    }

    av_freep(&avctx);
    avcodec_free_frame(&ctx->pic);

    mp_buffer_pool_free(&ctx->dr1_buffer_pool);
}

static void uninit(sh_video_t *sh)
{
    vd_ffmpeg_ctx *ctx = sh->context;
    AVCodecContext *avctx = ctx->avctx;

    uninit_avctx(sh);
    talloc_free(ctx);
}

static int init_vo(sh_video_t *sh, AVFrame *frame)
{
    vd_ffmpeg_ctx *ctx = sh->context;
    int width = frame->width;
    int height = frame->height;
    float aspect = av_q2d(frame->sample_aspect_ratio) * width / height;

    /* Reconfiguring filter/VO chain may invalidate direct rendering buffers
     * we have allocated for libavcodec (including the VDPAU HW decoding
     * case). Is it guaranteed that the code below only triggers in a situation
     * with no busy direct rendering buffers for reference frames?
     */
    if (av_cmp_q(frame->sample_aspect_ratio, ctx->last_sample_aspect_ratio) ||
            width != sh->disp_w || height != sh->disp_h ||
            frame->format != ctx->pix_fmt || !ctx->vo_initialized)
    {
        mp_image_pool_clear(ctx->non_dr1_pool);
        ctx->vo_initialized = 0;
        mp_msg(MSGT_DECVIDEO, MSGL_V, "[ffmpeg] aspect_ratio: %f\n", aspect);

        // Do not overwrite s->aspect on the first call, so that a container
        // aspect if available is preferred.
        // But set it even if the sample aspect did not change, since a
        // resolution change can cause an aspect change even if the
        // _sample_ aspect is unchanged.
        if (sh->aspect == 0 || ctx->last_sample_aspect_ratio.den)
            sh->aspect = aspect;
        ctx->last_sample_aspect_ratio = frame->sample_aspect_ratio;
        sh->disp_w = width;
        sh->disp_h = height;

        ctx->pix_fmt = frame->format;
        ctx->best_csp = pixfmt2imgfmt(frame->format);

        sh->colorspace = avcol_spc_to_mp_csp(ctx->avctx->colorspace);
        sh->color_range = avcol_range_to_mp_csp_levels(ctx->avctx->color_range);

        if (!mpcodecs_config_vo(sh, sh->disp_w, sh->disp_h, ctx->best_csp))
            return -1;

        ctx->vo_initialized = 1;
    }
    return 0;
}

static enum PixelFormat get_format_hwdec(struct AVCodecContext *avctx,
                                         const enum PixelFormat *fmt)
{
    sh_video_t *sh = avctx->opaque;
    vd_ffmpeg_ctx *ctx = sh->context;

    mp_msg(MSGT_DECVIDEO, MSGL_V, "Pixel formats supported by decoder:");
    for (int i = 0; fmt[i] != PIX_FMT_NONE; i++)
        mp_msg(MSGT_DECVIDEO, MSGL_V, " %s", av_get_pix_fmt_name(fmt[i]));
    mp_msg(MSGT_DECVIDEO, MSGL_V, "\n");

    assert(ctx->hwdec);

    for (int i = 0; fmt[i] != PIX_FMT_NONE; i++) {
        int imgfmt = pixfmt2imgfmt(fmt[i]);
        if (ctx->hwdec->api == HWDEC_VDPAU && IMGFMT_IS_VDPAU(imgfmt))
            return fmt[i];
    }

    return PIX_FMT_NONE;
}

static void draw_slice_hwdec(struct AVCodecContext *s,
                             const AVFrame *src, int offset[4],
                             int y, int type, int height)
{
    sh_video_t *sh = s->opaque;
    struct vf_instance *vf = sh->vfilter;
    void *state_ptr = src->data[0];
    vf->control(vf, VFCTRL_HWDEC_DECODER_RENDER, state_ptr);
}

static int get_buffer_hwdec(AVCodecContext *avctx, AVFrame *pic)
{
    sh_video_t *sh = avctx->opaque;
    vd_ffmpeg_ctx *ctx = sh->context;

    /* Decoders using ffmpeg's hwaccel architecture (everything except vdpau)
     * can fall back to software decoding automatically. However, we don't
     * want that: multithreading was already disabled. ffmpeg's fallback
     * isn't really useful, and causes more trouble than it helps.
     *
     * Instead of trying to "adjust" the thread_count fields in avctx, let
     * decoding fail hard. Then decode_with_fallback() will do our own software
     * fallback. Fully reinitializing the decoder is saner, and will probably
     * save us from other weird corner cases, like having to "reroute" the
     * get_buffer callback.
     */
    int imgfmt = pixfmt2imgfmt(pic->format);
    if (!IMGFMT_IS_HWACCEL(imgfmt))
        return -1;

    if (init_vo(sh, pic) < 0)
        return -1;

    assert(IMGFMT_IS_HWACCEL(ctx->best_csp));

    struct mp_image *mpi = NULL;

    struct vf_instance *vf = sh->vfilter;
    vf->control(vf, VFCTRL_HWDEC_ALLOC_SURFACE, &mpi);
    if (!mpi)
        return -1;

    for (int i = 0; i < 4; i++)
        pic->data[i] = mpi->planes[i];
    pic->opaque = mpi;
    pic->type = FF_BUFFER_TYPE_USER;

    /* The libavcodec reordered_opaque functionality is implemented by
     * a similar copy in avcodec_default_get_buffer() and without a
     * workaround like this it'd stop working when a custom buffer
     * callback is used.
     */
    pic->reordered_opaque = avctx->reordered_opaque;
    return 0;
}

static void release_buffer_hwdec(AVCodecContext *avctx, AVFrame *pic)
{
    mp_image_t *mpi = pic->opaque;

    assert(pic->type == FF_BUFFER_TYPE_USER);
    assert(mpi);

    talloc_free(mpi);

    for (int i = 0; i < 4; i++)
        pic->data[i] = NULL;
}

static void fb_ref(void *b)
{
    mp_buffer_ref(b);
}

static void fb_unref(void *b)
{
    mp_buffer_unref(b);
}

static bool fb_is_unique(void *b)
{
    return mp_buffer_is_unique(b);
}

static int decode(struct sh_video *sh, struct demux_packet *packet, void *data,
                  int len, int flags, double *reordered_pts,
                  struct mp_image **out_image)
{
    int got_picture = 0;
    int ret;
    vd_ffmpeg_ctx *ctx = sh->context;
    AVFrame *pic = ctx->pic;
    AVCodecContext *avctx = ctx->avctx;
    AVPacket pkt;

    if (flags & 2)
        avctx->skip_frame = AVDISCARD_ALL;
    else if (flags & 1)
        avctx->skip_frame = AVDISCARD_NONREF;
    else
        avctx->skip_frame = ctx->skip_frame;

    av_init_packet(&pkt);
    pkt.data = data;
    pkt.size = len;
    /* Some codecs (ZeroCodec, some cases of PNG) may want keyframe info
     * from demuxer. */
    if (packet && packet->keyframe)
        pkt.flags |= AV_PKT_FLAG_KEY;
    if (packet && packet->avpacket) {
        pkt.side_data = packet->avpacket->side_data;
        pkt.side_data_elems = packet->avpacket->side_data_elems;
    }
    // The avcodec opaque field stupidly supports only int64_t type
    union pts { int64_t i; double d; };
    avctx->reordered_opaque = (union pts){.d = *reordered_pts}.i;
    ret = avcodec_decode_video2(avctx, pic, &got_picture, &pkt);
    if (ret < 0) {
        mp_msg(MSGT_DECVIDEO, MSGL_WARN, "Error while decoding frame!\n");
        return -1;
    }
    *reordered_pts = (union pts){.i = pic->reordered_opaque}.d;

    if (!got_picture)
        return 0;                     // skipped image

    if (init_vo(sh, pic) < 0)
        return -1;

    struct mp_image *mpi = NULL;
    if (ctx->do_hw_dr1 && pic->opaque) {
        mpi = pic->opaque; // reordered frame
        assert(mpi);
        mpi = mp_image_new_ref(mpi);
    }

    if (!mpi) {
        struct mp_image new = {0};
        mp_image_set_size(&new, pic->width, pic->height);
        mp_image_setfmt(&new, ctx->best_csp);
        for (int i = 0; i < 4; i++) {
            new.planes[i] = pic->data[i];
            new.stride[i] = pic->linesize[i];
        }
        if (ctx->do_dr1 && pic->opaque) {
            struct FrameBuffer *fb = pic->opaque;
            mp_buffer_ref(fb); // initial reference for mpi
            mpi = mp_image_new_external_ref(&new, fb, fb_ref, fb_unref,
                                            fb_is_unique);
        } else {
            mpi = mp_image_pool_get(ctx->non_dr1_pool, new.imgfmt,
                                    new.w, new.h);
            mp_image_copy(mpi, &new);
        }
    }

    assert(mpi->planes[0]);

    assert(mpi->imgfmt == pixfmt2imgfmt(pic->format));

    mpi->colorspace = sh->colorspace;
    mpi->levels = sh->color_range;
    mpi->qscale = pic->qscale_table;
    mpi->qstride = pic->qstride;
    mpi->pict_type = pic->pict_type;
    mpi->qscale_type = pic->qscale_type;
    mpi->fields = MP_IMGFIELD_ORDERED;
    if (pic->interlaced_frame)
        mpi->fields |= MP_IMGFIELD_INTERLACED;
    if (pic->top_field_first)
        mpi->fields |= MP_IMGFIELD_TOP_FIRST;
    if (pic->repeat_pict == 1)
        mpi->fields |= MP_IMGFIELD_REPEAT_FIRST;

    *out_image = mpi;
    return 1;
}

static struct mp_image *decode_with_fallback(struct sh_video *sh,
                                struct demux_packet *packet, void *data,
                                int len, int flags, double *reordered_pts)
{
    vd_ffmpeg_ctx *ctx = sh->context;
    if (!ctx->avctx)
        return NULL;

    struct mp_image *mpi = NULL;
    int res = decode(sh, packet, data, len, flags, reordered_pts, &mpi);
    if (res >= 0)
        return mpi;

    // Failed hardware decoding? Try again in software.
    if (ctx->software_fallback) {
        uninit_avctx(sh);
        sh->vf_initialized = 0;
        mp_tmsg(MSGT_DECVIDEO, MSGL_ERR, "Error using hardware "
                "decoding, falling back to software decoding.\n");
        AVCodec *codec = ctx->software_fallback;
        ctx->software_fallback = NULL;
        if (init_avctx(sh, codec, NULL)) {
            mpi = NULL;
            decode(sh, packet, data, len, flags, reordered_pts, &mpi);
            return mpi;
        }
    }

    return NULL;
}

static int control(sh_video_t *sh, int cmd, void *arg)
{
    vd_ffmpeg_ctx *ctx = sh->context;
    AVCodecContext *avctx = ctx->avctx;
    switch (cmd) {
    case VDCTRL_RESYNC_STREAM:
        avcodec_flush_buffers(avctx);
        return CONTROL_TRUE;
    case VDCTRL_QUERY_UNSEEN_FRAMES:;
        int delay = avctx->has_b_frames;
        if (avctx->active_thread_type & FF_THREAD_FRAME)
            delay += avctx->thread_count - 1;
        return delay + 10;
    case VDCTRL_RESET_ASPECT:
        if (ctx->vo_initialized)
            ctx->vo_initialized = false;
        init_vo(sh, ctx->pic);
        return true;
    }
    return CONTROL_UNKNOWN;
}

const struct vd_functions mpcodecs_vd_ffmpeg = {
    .info = &info,
    .init = init,
    .uninit = uninit,
    .control = control,
    .decode = decode_with_fallback,
};
