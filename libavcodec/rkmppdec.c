#include <libdrm/drm_fourcc.h>
#include <pthread.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/rk_mpi.h>
#include <time.h>
#include <unistd.h>

#include "avcodec.h"
#include "decode.h"
#include "drmprime.h"
#include "internal.h"
#include "libavutil/common.h"
#include "libavutil/buffer.h"
#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"

#define RECEIVE_FRAME_TIMEOUT	100

typedef struct {
    MppCtx ctx;
    MppApi *mpi;
    MppBufferGroup frame_group;

    char first_packet;
    char eos_reached;

} RKMPPDecoder;

typedef struct {
    AVClass *av_class;

    AVBufferRef *decoder_ref;
} RKMPPDecodeContext;

typedef struct {
    MppFrame frame;
    AVBufferRef *decoder_ref;
} RKMPPFrameContext;

static void debuglog(const char* fmt, ...)
{
    va_list ap;
    char msg[512];
    FILE *fp;

    va_start(ap, fmt);
    vsprintf(msg, fmt, ap);
    va_end(ap);

    fp = fopen("/storage/log.txt", "a");
    if (fp)
    {
        fwrite(msg, strlen(msg), 1, fp);
        fclose(fp);
    }
}

static MppCodingType ffrkmpp_get_codingtype(AVCodecContext *avctx)
{
    switch(avctx->codec_id) {
    case AV_CODEC_ID_H264:  return MPP_VIDEO_CodingAVC;
    case AV_CODEC_ID_HEVC:  return MPP_VIDEO_CodingHEVC;
    case AV_CODEC_ID_VP8:   return MPP_VIDEO_CodingVP8;
    default:                return MPP_VIDEO_CodingUnused;
    }
}

static int ffrkmpp_get_frameformat(MppFrameFormat mppformat)
{
    switch(mppformat) {
    case MPP_FMT_YUV420SP:          return DRM_FORMAT_NV12;
    case MPP_FMT_YUV420SP_10BIT:    return DRM_FORMAT_NV12_10;
    default:                        return 0;
    }
}

static int ffrkmpp_write_data(AVCodecContext *avctx, char *buffer, int size, int64_t pts)
{
    MppPacket packet;
    MPP_RET ret = MPP_OK;
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;

    // create the MPP packet
    ret = mpp_packet_init(&packet, buffer, size);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init MPP packet (code = %d)\n", ret);
        return AVERROR_UNKNOWN;
    }

    mpp_packet_set_pts(packet, pts);

    if (!buffer)
        mpp_packet_set_eos(packet);

    ret = decoder->mpi->decode_put_packet(decoder->ctx, packet);

    if (ret < 0) {
        if (ret == MPP_ERR_BUFFER_FULL)
            ret = AVERROR(EAGAIN);
        else
            ret = AVERROR_UNKNOWN;
    }
    else
        av_log(avctx, AV_LOG_DEBUG, "Wrote %d bytes to decoder\n", size);

    mpp_packet_deinit(&packet);

    return ret;
}

static int ffrkmpp_close_decoder(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    debuglog("FFMPEG : closing decoder (left =%d)\n", av_buffer_get_ref_count(rk_context->decoder_ref));
    av_buffer_unref(&rk_context->decoder_ref);
    return 0;
}

static void ffrkmpp_release_decoder(void *opaque, uint8_t *data)
{
    RKMPPDecoder *decoder = (RKMPPDecoder *)data;
    debuglog("FFMPEG : releasing decoder (framegroup =%p)\n", decoder->frame_group);

    decoder->mpi->reset(decoder->ctx);
    mpp_destroy(decoder->ctx);
    decoder->ctx = NULL;

    if (decoder->frame_group)
    {
        debuglog("FFMPEG : releasing framegroup\n");
        mpp_buffer_group_put(decoder->frame_group);
        decoder->frame_group = NULL;
    }

    av_free(decoder);
}

static int ffrkmpp_init_decoder(AVCodecContext *avctx)
{
    MPP_RET ret = MPP_OK;
    MppCodingType codectype = MPP_VIDEO_CodingUnused;

    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder;
    RK_S64 paramS64;
    RK_S32 paramS32;

    if ((ret = ff_get_format(avctx, avctx->codec->pix_fmts)) < 0)
        return ret;

    avctx->pix_fmt = ret;

    // create a decoder and a ref to it
    decoder = av_mallocz(sizeof(RKMPPDecoder));
    if (!decoder) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    rk_context->decoder_ref = av_buffer_create((uint8_t*)decoder, sizeof(*decoder), ffrkmpp_release_decoder,
                                    NULL, AV_BUFFER_FLAG_READONLY);
    if (!rk_context->decoder_ref) {
        av_free(decoder);
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    av_log(avctx, AV_LOG_DEBUG, "Initializing RKMPP decoder.\n");

    // Create the MPP context
    ret = mpp_create(&decoder->ctx, &decoder->mpi);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // initialize the context
    codectype = ffrkmpp_get_codingtype(avctx);
    if (codectype == MPP_VIDEO_CodingUnused) {
        av_log(avctx, AV_LOG_ERROR, "Unknown codec type (%d).\n", avctx->codec_id);
        goto fail;
    }

    // initialize mpp
    ret = mpp_init(decoder->ctx, MPP_CTX_DEC, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize MPP context (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    // make decode calls blocking with a timeout
    paramS32 = MPP_POLL_BLOCK;
    ret = decoder->mpi->control(decoder->ctx, MPP_SET_OUTPUT_BLOCK, &paramS32);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set blocking mode on MPI (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    paramS64 = RECEIVE_FRAME_TIMEOUT;
    ret = decoder->mpi->control(decoder->ctx, MPP_SET_OUTPUT_BLOCK_TIMEOUT, &paramS64);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set block timeout on MPI (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        goto fail;
    }

    ret = mpp_buffer_group_get_internal(&decoder->frame_group, MPP_BUFFER_TYPE_ION);
    if (ret) {
       av_log(avctx, AV_LOG_ERROR, "Failed to retrieve buffer group (code = %d)\n", ret);
       ret = AVERROR_UNKNOWN;
       goto fail;
    }

    ret = decoder->mpi->control(decoder->ctx, MPP_DEC_SET_EXT_BUF_GROUP, decoder->frame_group);
    if (ret) {
       av_log(avctx, AV_LOG_ERROR, "Failed to assign buffer group (code = %d)\n", ret);
       ret = AVERROR_UNKNOWN;
       goto fail;
    }

    decoder->first_packet = 1;

    av_log(avctx, AV_LOG_DEBUG, "RKMPP decoder initialized successfully.\n");
    return 0;

fail:
    av_buffer_unref(&rk_context->decoder_ref);

    av_log(avctx, AV_LOG_ERROR, "Failed to initialize RKMPP decoder.\n");
    ffrkmpp_close_decoder(avctx);
    return ret;
}

static int ffrkmpp_send_packet(AVCodecContext *avctx, const AVPacket *avpkt)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    MPP_RET  ret = MPP_OK;

    // handle EOF
    if (avpkt->size == 0) {
        decoder->eos_reached = 1;
        ret = ffrkmpp_write_data(avctx, NULL, 0, 0);

        if (ret)
            av_log(avctx, AV_LOG_ERROR, "Failed to send EOS to decoder\n");

        return ret;
    }

    // on first packet, send extradata
    if (decoder->first_packet) {
        if (avctx->extradata_size) {
            ret = ffrkmpp_write_data(avctx, avctx->extradata,
                                            avctx->extradata_size,
                                            avpkt->pts);
            if (ret) {
                av_log(avctx, AV_LOG_ERROR, "Failed to write extradata to decoder\n");
                return ret;
            }
        }
        decoder->first_packet = 0;
    }

    // now send packet
    ret = ffrkmpp_write_data(avctx, avpkt->data, avpkt->size, avpkt->pts);
    if (ret != MPP_OK) {
        if (ret == MPP_ERR_BUFFER_FULL) {
            return AVERROR(EAGAIN);
        } else {
            av_log(avctx, AV_LOG_ERROR, "Failed to write data to decoder (%d)\n", ret);
            return ret;
        }
    }

    return ret;
}


static void ffrkmpp_release_frame(void *opaque, uint8_t *data)
{
    av_drmprime *primedata = (av_drmprime*)data;
    AVBufferRef *framecontextref = (AVBufferRef *)opaque;
    RKMPPFrameContext *framecontext = (RKMPPFrameContext *)framecontextref->data;

    debuglog("releasing frame with fd=%d (decoder ref=%d)\n", primedata->fds[0],av_buffer_get_ref_count(framecontext->decoder_ref));
    mpp_frame_deinit(&framecontext->frame);
    av_buffer_unref(&framecontext->decoder_ref);
    av_buffer_unref(&framecontextref);

    av_free(primedata);
}

static int ffrkmpp_retrieve_frame(AVCodecContext *avctx, AVFrame *frame)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    RKMPPFrameContext *framecontext = NULL;
    AVBufferRef *framecontextref = NULL;
    MPP_RET  ret = MPP_OK;
    MppFrame mppframe = NULL;
    MppBuffer buffer = NULL;
    av_drmprime *primedata = NULL;

retry_get_frame :
    ret = decoder->mpi->decode_get_frame(decoder->ctx, &mppframe);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "can't get a frame frome decoder (code = %d)\n", ret);
        goto fail;
    }

    if (decoder->eos_reached && !mpp_frame_get_eos(mppframe)) {
        if (mppframe)
            mpp_frame_deinit(&mppframe);
        usleep(10000);
        goto retry_get_frame;
    }
    if (mppframe && mpp_frame_get_eos(mppframe)) {
        av_log(avctx, AV_LOG_DEBUG, "EOS frame found\n");
        ret = AVERROR_EXIT;
        goto fail;
    }

    // Check wether we have an info frame or not
    if (mppframe) {
        if (mpp_frame_get_info_change(mppframe)) {
            av_log(avctx, AV_LOG_INFO, "Decoder noticed an info change (%dx%d), format=%d\n",
                                        (int)mpp_frame_get_width(mppframe),  (int)mpp_frame_get_height(mppframe),
                                        (int)mpp_frame_get_fmt(mppframe));

            avctx->width = mpp_frame_get_width(mppframe);
            avctx->height = mpp_frame_get_height(mppframe);

            decoder->mpi->control(decoder->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
            mpp_frame_deinit(&mppframe);


            // here decoder is fully initialized, we need to feed it again with data
            return AVERROR(EAGAIN);
        } else {
            av_log(avctx, AV_LOG_DEBUG, "Received a frame.\n");
        }
    }

    // here we should have a valid frame
    if (mppframe) {

        if (mpp_frame_get_discard(mppframe) || mpp_frame_get_errinfo(mppframe)) {
            ret = AVERROR(EAGAIN);
            goto fail;
        }

        // setup general frame fields
        frame->format = AV_PIX_FMT_RKMPP;
        frame->width  = mpp_frame_get_width(mppframe);
        frame->height = mpp_frame_get_height(mppframe);
        frame->pts    = mpp_frame_get_pts(mppframe);

        // now setup the frame buffer info
        buffer = mpp_frame_get_buffer(mppframe);
        if (buffer) {
            primedata = av_mallocz(sizeof(av_drmprime));
            if (!primedata) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            primedata->strides[0]   = mpp_frame_get_hor_stride(mppframe);
            primedata->strides[1]   = primedata->strides[0];
            primedata->offsets[0]   = 0;
            primedata->offsets[1]   = primedata->strides[0] * mpp_frame_get_ver_stride(mppframe);
            primedata->fds[0]       = mpp_buffer_get_fd(buffer);
            primedata->format       = ffrkmpp_get_frameformat(mpp_frame_get_fmt(mppframe));


            // we also allocate a struct in buf[0] that will allow to hold additionnal information
            // for releasing properly MPP frames and decoder
            framecontextref = av_buffer_allocz(sizeof(*framecontext));
            if (!framecontextref) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            // MPP decoder needs to be closed only when all frames have been released.
            framecontext = (RKMPPFrameContext *)framecontextref->data;
            framecontext->decoder_ref = av_buffer_ref(rk_context->decoder_ref);
            framecontext->frame = mppframe;

            frame->data[3] = (uint8_t*)primedata;
            frame->buf[0]  = av_buffer_create((uint8_t*)primedata, sizeof(*primedata), ffrkmpp_release_frame,
                                                framecontextref, AV_BUFFER_FLAG_READONLY);

            if (!frame->buf[0]) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            debuglog("Returning frame with fd=%d\n", primedata->fds[0]);

            return 0;
        } else {
            av_log(avctx, AV_LOG_ERROR, "Failed to retrieve the frame buffer, frame is dropped (code = %d)\n", ret);
            mpp_frame_deinit(&mppframe);
        }
    } else {
        if (decoder->eos_reached)
            return AVERROR_EOF;
    }

    return AVERROR(EAGAIN);

fail:
    if (mppframe)
        mpp_frame_deinit(&mppframe);

    if (framecontext)
        av_buffer_unref(&framecontext->decoder_ref);

    if (framecontextref)
        av_buffer_unref(&framecontextref);

    if (primedata)
        av_free(primedata);


    return ret;
}

static int ffrkmpp_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;
    AVPacket pkt = {0};
    int ret;
    RK_S32 freeslots;

    // we get the available slots in decoder
    ret = decoder->mpi->control(decoder->ctx, MPP_DEC_GET_FREE_PACKET_SLOT_COUNT, &freeslots);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get decoder free slots (code = %d).\n", ret);
        return ret;
    }

    if (freeslots > 0 && !decoder->eos_reached)
    {
        ret = ff_decode_get_packet(avctx, &pkt);
        if (ret < 0 && ret != AVERROR_EOF) {
            return ret;
        }

        ret = ffrkmpp_send_packet(avctx, &pkt);
        av_packet_unref(&pkt);

        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to send packet to decoder (code = %d)\n", ret);
            return ret;
        }
    }

    return ffrkmpp_retrieve_frame(avctx, frame);
}

static void ffrkmpp_flush(AVCodecContext *avctx)
{
    MPP_RET ret = MPP_OK;
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = (RKMPPDecoder *)rk_context->decoder_ref->data;

    ret = decoder->mpi->reset(decoder->ctx);
    if (ret == MPP_OK)
        decoder->first_packet = 1;
    else
        av_log(avctx, AV_LOG_ERROR, "Failed to reset MPI (code = %d)\n", ret);
}

#define FFRKMPP_DEC_HWACCEL(NAME, ID) \
  AVHWAccel ff_##NAME##_rkmpp_hwaccel = { \
      .name       = #NAME "_rkmpp", \
      .type       = AVMEDIA_TYPE_VIDEO,\
      .id         = ID, \
      .pix_fmt    = AV_PIX_FMT_RKMPP,\
  };

#define FFRKMPP_DEC_CLASS(NAME) \
    static const AVClass ffrkmpp_##NAME##_dec_class = { \
        .class_name = "rkmpp_" #NAME "_dec", \
        .version    = LIBAVUTIL_VERSION_INT, \
    };

#define FFRKMPP_DEC(NAME, ID, BSFS) \
    FFRKMPP_DEC_CLASS(NAME) \
    FFRKMPP_DEC_HWACCEL(NAME, ID) \
    AVCodec ff_##NAME##_rkmpp_decoder = { \
        .name               = #NAME "_rkmpp", \
        .long_name          = NULL_IF_CONFIG_SMALL(#NAME " (rkmpp)"), \
        .type               = AVMEDIA_TYPE_VIDEO, \
        .id                 = ID, \
        .priv_data_size     = sizeof(RKMPPDecodeContext), \
        .init               = ffrkmpp_init_decoder, \
        .close              = ffrkmpp_close_decoder, \
        .receive_frame      = ffrkmpp_receive_frame, \
        .flush              = ffrkmpp_flush, \
        .priv_class         = &ffrkmpp_##NAME##_dec_class, \
        .capabilities       = AV_CODEC_CAP_DELAY, \
        .caps_internal      = FF_CODEC_CAP_SETS_PKT_DTS, \
        .pix_fmts           = (const enum AVPixelFormat[]) { AV_PIX_FMT_RKMPP, \
                                                         AV_PIX_FMT_NONE}, \
        .bsfs               = #BSFS, \
    };

FFRKMPP_DEC(h264, AV_CODEC_ID_H264, h264_mp4toannexb)
FFRKMPP_DEC(hevc, AV_CODEC_ID_HEVC, hevc_mp4toannexb)
FFRKMPP_DEC(vp8,  AV_CODEC_ID_VP8,  )
