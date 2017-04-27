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

    AVBufferRef *ref;

    MppCtx ctx;
    MppApi *mpi;
    MppBufferGroup frame_group;

    char first_packet;
    char eos_reached;

} RKMPPDecoder;

typedef struct {
    AVClass *av_class;

    RKMPPDecoder *decoder;

    int framecount;

} RKMPPDecodeContext;

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
    RKMPPDecoder *decoder = rk_context->decoder;

    // create the MPP packet
    ret = mpp_packet_init(&packet, buffer, size);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init MPP packet (code = %d)\n", ret);
        return ret;
    }

    // set the pts
    mpp_packet_set_pts(packet, pts);

    // if we have a NULL packet, then assume EOS
    if (!buffer)
        mpp_packet_set_eos(packet);

    // write it to decoder
    ret = decoder->mpi->decode_put_packet(decoder->ctx, packet);

    if (ret != MPP_ERR_BUFFER_FULL)
        av_log(avctx, AV_LOG_ERROR, "Wrote %d bytes to decoder", size);

    mpp_packet_deinit(&packet);

    return ret;
}

static int ffrkmpp_close_decoder(AVCodecContext *avctx)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = rk_context->decoder;

    av_buffer_unref(&decoder->ref);

    return 0;
}

static void ffrkmpp_release_decoder(void *opaque, uint8_t *data)
{
    RKMPPDecoder *decoder = (RKMPPDecoder *)data;

    decoder->mpi->reset(decoder->ctx);
    mpp_destroy(decoder->ctx);
    decoder->ctx = NULL;
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

    rk_context->decoder = decoder;

    decoder->ref = av_buffer_create((uint8_t*)decoder, sizeof(*decoder), ffrkmpp_release_decoder,
                                    NULL, AV_BUFFER_FLAG_READONLY);
    if (!decoder->ref) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    av_log(avctx, AV_LOG_DEBUG, "Initializing RKMPP decoder.\n");

    // Create the MPP context
    ret = mpp_create(&decoder->ctx, &decoder->mpi);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create MPP context (code = %d).\n", ret);
        goto fail;
    }

    // initialize the context
    codectype = ffrkmpp_get_codingtype(avctx);
    if (codectype == MPP_VIDEO_CodingUnused) {
        av_log(avctx, AV_LOG_ERROR, "Unknown codec type (%d).\n", ret);
        goto fail;
    }

    // initialize mpp
    ret = mpp_init(decoder->ctx, MPP_CTX_DEC, codectype);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize MPP context (code = %d).\n", ret);
        goto fail;
    }

    // make decode calls blocking with a timeout
    paramS32 = MPP_POLL_BLOCK;
    ret = decoder->mpi->control(decoder->ctx, MPP_SET_OUTPUT_BLOCK, &paramS32);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set blocking mode on MPI (code = %d).\n", ret);
        goto fail;
    }

    paramS64 = RECEIVE_FRAME_TIMEOUT;
    ret = decoder->mpi->control(decoder->ctx, MPP_SET_OUTPUT_BLOCK_TIMEOUT, &paramS64);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set block timeout on MPI (code = %d).\n", ret);
        goto fail;
    }

    decoder->first_packet = 1;

    av_log(avctx, AV_LOG_DEBUG, "RKMPP decoder initialized successfully.\n");
    return 0;

fail:
    if (decoder)
        av_buffer_unref(&decoder->ref);

    av_log(avctx, AV_LOG_ERROR, "Failed to initialize RKMPP decoder.\n");
    ffrkmpp_close_decoder(avctx);
    return ret;
}

static int ffrkmpp_send_packet(AVCodecContext *avctx, const AVPacket *avpkt)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = rk_context->decoder;
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
        ret = ffrkmpp_write_data(avctx, avctx->extradata,
                                        avctx->extradata_size,
                                        avpkt->pts);
        if (ret) {
            av_log(avctx, AV_LOG_ERROR, "Failed to write extradata to decoder\n");
            return ret;
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
    MppFrame mppframe = (MppFrame)opaque;
    mpp_frame_deinit(&mppframe);
}

static int ffrkmpp_retreive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = rk_context->decoder;
    MPP_RET  ret = MPP_OK;
    MppFrame mppframe = NULL;
    MppBuffer buffer = NULL;
    MppBufferInfo bufferinfo;
    av_drmprime *primedata = NULL;
    int retrycount = 0;

    // now we will try to get a frame back

retry_get_frame :
    ret = decoder->mpi->decode_get_frame(decoder->ctx, &mppframe);

    // on start of decoding, MPP can return -1, which is supposed to be expected
    // then we need to retry a couple times
    if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT) {
        if (retrycount < 5) {
            usleep(50000);
            retrycount++;
            goto retry_get_frame;
        } else {
            av_log(avctx, AV_LOG_ERROR, "Failed to get a frame from MPP (code = %d)\n", ret);
            goto fail;
        }
    }

    // Check wether we have an info frame or not
    if (mppframe) {
        if (mpp_frame_get_info_change(mppframe)) {
            av_log(avctx, AV_LOG_INFO, "Decoder noticed an info change (%dx%d), format=%d\n",
                                        (int)mpp_frame_get_width(mppframe),  (int)mpp_frame_get_height(mppframe),
                                        (int)mpp_frame_get_fmt(mppframe));
            decoder->mpi->control(decoder->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
            mpp_frame_deinit(&mppframe);

            // here decoder is fully initialized, we need to feed it again with data
            return AVERROR(EAGAIN);
        } else {
            rk_context->framecount++;
            av_log(avctx, AV_LOG_DEBUG, "Received a frame.\n");
        }
    }

    // here we should have a valid frame
    if (mppframe) {
        // setup general frame fields
        frame->format = AV_PIX_FMT_RKMPP;
        frame->width  = mpp_frame_get_width(mppframe);
        frame->height = mpp_frame_get_height(mppframe);
        frame->pts    = mpp_frame_get_pts(mppframe);

        // now setup the frame buffer info
        buffer = mpp_frame_get_buffer(mppframe);
        if (buffer) {
            ret = mpp_buffer_info_get(buffer, &bufferinfo);
            if (ret != MPP_OK) {
                av_log(avctx, AV_LOG_ERROR, "Failed to get info from MPP buffer (code = %d)\n", ret);
                goto fail;
            }

            primedata = av_mallocz(sizeof(av_drmprime));
            if (!primedata) {
                av_log(avctx, AV_LOG_ERROR, "Failed to allocated drm prime data.\n");
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            // now fill up the DRMPRIME data
            primedata->strides[0]   = mpp_frame_get_hor_stride(mppframe);
            primedata->strides[1]   = primedata->strides[0];
            primedata->offsets[0]   = 0;
            primedata->offsets[1]   = primedata->strides[0] * mpp_frame_get_ver_stride(mppframe);
            primedata->fd[0]        = mpp_buffer_get_fd(buffer);
            primedata->format       = ffrkmpp_get_frameformat(mpp_frame_get_fmt(mppframe));

            frame->data[3] = (uint8_t*)primedata;
            frame->buf[0]  = av_buffer_create((uint8_t*)primedata, sizeof(*primedata), ffrkmpp_release_frame,
                                          mppframe, AV_BUFFER_FLAG_READONLY);

            if (!frame->buf[0]) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            // add a ref to decoder for each frame as we need to release it
            // only once all frames have been released
            frame->buf[1] = av_buffer_ref(decoder->ref);

            return 0;
        } else {
            av_log(avctx, AV_LOG_ERROR, "Failed to retrieve the frame buffer, frame is dropped (code = %d)\n", ret);
        }
    } else {
        if (decoder->eos_reached)
            return AVERROR_EOF;
    }

    return AVERROR(EAGAIN);

fail:
    if (primedata)
        av_free(primedata);

    if (mppframe)
        mpp_frame_deinit(&mppframe);

    return ret;
}

static int ffrkmpp_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = rk_context->decoder;
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

    return ffrkmpp_retreive_frame(avctx, frame);
}

static void ffrkmpp_flush(AVCodecContext *avctx)
{
    MPP_RET ret = MPP_OK;
    RKMPPDecodeContext *rk_context = avctx->priv_data;
    RKMPPDecoder *decoder = rk_context->decoder;

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
