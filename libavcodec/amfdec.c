/*
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

#include <AMF/core/Variant.h>
#include <AMF/core/PropertyStorage.h>
#include <AMF/components/FFMPEGFileDemuxer.h>
#include "libavutil/hwcontext_amf.h"
#include "amfdec.h"
#include "codec_internal.h"
#include "hwconfig.h"
#include "libavutil/imgutils.h"
#include "libavutil/time.h"
#include "decode.h"
#include "libavutil/mastering_display_metadata.h"

#if CONFIG_D3D11VA
#include "libavutil/hwcontext_d3d11va.h"
#endif
#if CONFIG_DXVA2
#define COBJMACROS
#include "libavutil/hwcontext_dxva2.h"
#endif

#ifdef _WIN32
#include "compat/w32dlfcn.h"
#else
#include <dlfcn.h>
#endif

#define propNotFound 0

const enum AVPixelFormat amf_dec_pix_fmts[] = {
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_BGRA,
    AV_PIX_FMT_ARGB,
    AV_PIX_FMT_RGBA,
    AV_PIX_FMT_GRAY8,
    AV_PIX_FMT_BGR0,
    AV_PIX_FMT_YUYV422,
    AV_PIX_FMT_P010,
    AV_PIX_FMT_P012,
    AV_PIX_FMT_YUV420P10,
    AV_PIX_FMT_YUV420P12,
    AV_PIX_FMT_YUV420P16,
    AV_PIX_FMT_YUV422P10LE,
    AV_PIX_FMT_YUV444P10LE,
#if CONFIG_D3D11VA
    AV_PIX_FMT_D3D11,
#endif
#if CONFIG_DXVA2
    AV_PIX_FMT_DXVA2_VLD,
#endif
    AV_PIX_FMT_AMF,
    AV_PIX_FMT_NONE
};

static const AVCodecHWConfigInternal *const amf_hw_configs[] = {
    &(const AVCodecHWConfigInternal) {
        .public = {
            .pix_fmt     = AV_PIX_FMT_AMF,
            .methods     = AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX |
                           AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX,
            .device_type = AV_HWDEVICE_TYPE_AMF,
        },
        .hwaccel = NULL,
    },
    NULL
};

static void amf_free_amfsurface(void *opaque, uint8_t *data)
{
    AMFSurface *surface = (AMFSurface*)(opaque);
    //FIXME: release shared surface properly
    //surface->pVtbl->Release(surface);
}

static int amf_init_decoder(AVCodecContext *avctx)
{
    enum AMF_SURFACE_FORMAT output_format = AMF_SURFACE_UNKNOWN;
    AvAmfDecoderContext     *ctx = avctx->priv_data;
    AVAMFDeviceContextInternal * internal = (AVAMFDeviceContextInternal *)ctx->amf_device_ctx_internal->data;
    const wchar_t           *codec_id = NULL;
    AMF_RESULT              res;
    AMFBuffer               *buffer;
    amf_int64               color_profile;

    output_format = amf_av_to_amf_format(avctx->pix_fmt);

    if (output_format == AMF_SURFACE_UNKNOWN)
        output_format = AMF_SURFACE_NV12;

    ctx->drained = 0;

    switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            codec_id = AMFVideoDecoderUVD_H264_AVC;
            break;
        case AV_CODEC_ID_HEVC: {
            if (output_format == AMF_SURFACE_P010)
                codec_id = AMFVideoDecoderHW_H265_MAIN10;
            else
                codec_id = AMFVideoDecoderHW_H265_HEVC;
        } break;
        case AV_CODEC_ID_AV1:
            if (output_format == AMF_SURFACE_P012)
                codec_id = AMFVideoDecoderHW_AV1_12BIT;
            else
                codec_id = AMFVideoDecoderHW_AV1;
            break;
        default:
            break;
    }
    AMF_RETURN_IF_FALSE(ctx, codec_id != NULL, AVERROR(EINVAL), "Codec %d is not supported\n", avctx->codec->id);

    res = internal->factory->pVtbl->CreateComponent(internal->factory, internal->context, codec_id, &ctx->decoder);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_ENCODER_NOT_FOUND, "CreateComponent(%ls) failed with error %d\n", codec_id, res);

    // Color Metadata
    /// Color Range (Support for older Drivers)
    if (avctx->color_range == AVCOL_RANGE_JPEG) {
        AMF_ASSIGN_PROPERTY_BOOL(res, ctx->decoder, AMF_VIDEO_DECODER_FULL_RANGE_COLOR, 1);
    } else if (avctx->color_range != AVCOL_RANGE_UNSPECIFIED) {
        AMF_ASSIGN_PROPERTY_BOOL(res, ctx->decoder, AMF_VIDEO_DECODER_FULL_RANGE_COLOR, 0);
    }
    color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN;
    switch (avctx->colorspace) {
    case AVCOL_SPC_SMPTE170M:
        if (avctx->color_range == AVCOL_RANGE_JPEG) {
            color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_601;
        } else {
            color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_601;
        }
        break;
    case AVCOL_SPC_BT709:
        if (avctx->color_range == AVCOL_RANGE_JPEG) {
            color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_709;
        } else {
            color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_709;
        }
        break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        if (avctx->color_range == AVCOL_RANGE_JPEG) {
            color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_2020;
        } else {
            color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020;
        }
        break;
    }
    if (color_profile != AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN)
        AMF_ASSIGN_PROPERTY_INT64(res, ctx->decoder, AMF_VIDEO_DECODER_COLOR_PROFILE, color_profile);
    if (avctx->color_trc != AVCOL_TRC_UNSPECIFIED)
        AMF_ASSIGN_PROPERTY_INT64(res, ctx->decoder, AMF_VIDEO_DECODER_COLOR_TRANSFER_CHARACTERISTIC, (amf_int64)avctx->color_trc);

    if (avctx->color_primaries != AVCOL_PRI_UNSPECIFIED)
        AMF_ASSIGN_PROPERTY_INT64(res, ctx->decoder, AMF_VIDEO_DECODER_COLOR_PRIMARIES, (amf_int64)avctx->color_primaries);

    AMF_ASSIGN_PROPERTY_INT64(res, ctx->decoder, AMF_TIMESTAMP_MODE, ctx->timestamp_mode);
    AMF_ASSIGN_PROPERTY_INT64(res, ctx->decoder, AMF_VIDEO_DECODER_REORDER_MODE, ctx->decoder_mode);

    AMF_ASSIGN_PROPERTY_INT64(res, ctx->decoder, AMF_VIDEO_DECODER_SURFACE_POOL_SIZE, ctx->surface_pool_size);
    AMF_ASSIGN_PROPERTY_INT64(res, ctx->decoder, AMF_VIDEO_DECODER_DPB_SIZE, ctx->dpb_size);

    AMF_ASSIGN_PROPERTY_INT64(res, ctx->decoder, AMF_VIDEO_DECODER_LOW_LATENCY, ctx->lowlatency);
    AMF_ASSIGN_PROPERTY_INT64(res, ctx->decoder, AMF_VIDEO_DECODER_ENABLE_SMART_ACCESS_VIDEO, ctx->smart_access_video);
    AMF_ASSIGN_PROPERTY_INT64(res, ctx->decoder, AMF_VIDEO_DECODER_SKIP_TRANSFER_SMART_ACCESS_VIDEO, ctx->skip_transfer_sav);

    if (avctx->extradata_size) {
        res = internal->context->pVtbl->AllocBuffer(internal->context, AMF_MEMORY_HOST, avctx->extradata_size, &buffer);
        if (res == AMF_OK) {
            memcpy(buffer->pVtbl->GetNative(buffer), avctx->extradata, avctx->extradata_size);
            AMF_ASSIGN_PROPERTY_INTERFACE(res,ctx->decoder, AMF_VIDEO_DECODER_EXTRADATA, buffer);
            buffer->pVtbl->Release(buffer);
            buffer = NULL;
        }
    }

    res = ctx->decoder->pVtbl->Init(ctx->decoder, output_format, avctx->width, avctx->height);
    return 0;
}

static int amf_init_decoder_context(AVCodecContext *avctx)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;
    AMFContext1 *context1 = NULL;
    int ret;

    if (avctx->hw_frames_ctx) {
        AVHWFramesContext *frames_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
        ret = amf_context_derive((AVAMFDeviceContextInternal *)ctx->amf_device_ctx_internal->data, frames_ctx->device_ctx, NULL, 0);
        if (ret < 0)
            return ret;
        ctx->hw_frames_ctx = av_buffer_ref(avctx->hw_frames_ctx);
        if (!ctx->hw_frames_ctx)
            return AVERROR(ENOMEM);
    }
    else if (avctx->hw_device_ctx) {
        AVHWDeviceContext *device_ctx = (AVHWDeviceContext*)avctx->hw_device_ctx->data;
        ret = amf_context_derive((AVAMFDeviceContextInternal *)ctx->amf_device_ctx_internal->data, device_ctx, NULL, 0);
        if (ret < 0)
            return ret;
        ctx->hw_device_ctx = av_buffer_ref(avctx->hw_device_ctx);
        if (!ctx->hw_device_ctx)
            return AVERROR(ENOMEM);
    } else {
        ret = amf_context_init((AVAMFDeviceContextInternal *)ctx->amf_device_ctx_internal->data, avctx);
        if (ret != 0) {
            return ret;
        }
    }

    return ret;
}

static int amf_decode_close(AVCodecContext *avctx)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;

    if (ctx->decoder) {
        ctx->decoder->pVtbl->Terminate(ctx->decoder);
        ctx->decoder->pVtbl->Release(ctx->decoder);
        ctx->decoder = NULL;
    }

    av_buffer_unref(&ctx->amf_device_ctx_internal);
    av_buffer_unref(&ctx->hw_device_ctx);
    av_buffer_unref(&ctx->hw_frames_ctx);
    av_buffer_unref(&ctx->amf_device_ctx);

    return 0;

}

static int amf_decode_init(AVCodecContext *avctx)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;
    int ret;
    enum AVPixelFormat pix_fmts[3] = {
        AV_PIX_FMT_AMF,
        avctx->pix_fmt,
        AV_PIX_FMT_NONE };

    ret = ff_get_format(avctx, pix_fmts);
    if (ret < 0) {
        avctx->pix_fmt = AV_PIX_FMT_NONE;
    }

    if (avctx->hw_frames_ctx){
        AVHWFramesContext *frames_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
        if (frames_ctx->device_ctx->type == AV_HWDEVICE_TYPE_AMF) {
            AVAMFDeviceContext * amf_ctx =  frames_ctx->device_ctx->hwctx;
            ctx->amf_device_ctx_internal = av_buffer_ref(amf_ctx->internal);
        }
    }
    else if  (avctx->hw_device_ctx) {
        AVHWDeviceContext   *hwdev_ctx;
        hwdev_ctx = (AVHWDeviceContext*)avctx->hw_device_ctx->data;
        if (hwdev_ctx->type == AV_HWDEVICE_TYPE_AMF)
        {
            AVAMFDeviceContext * amf_ctx =  hwdev_ctx->hwctx;
            ctx->amf_device_ctx_internal = av_buffer_ref(amf_ctx->internal);
        }

        if (avctx->hw_device_ctx && !avctx->hw_frames_ctx && ret == AV_PIX_FMT_AMF) {
            AVHWFramesContext *hwframes_ctx;
            avctx->hw_frames_ctx = av_hwframe_ctx_alloc(avctx->hw_device_ctx);

            if (!avctx->hw_frames_ctx) {
                av_log(avctx, AV_LOG_ERROR, "av_hwframe_ctx_alloc failed\n");
                return AVERROR(ENOMEM);
            }

            hwframes_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
            hwframes_ctx->width             = FFALIGN(avctx->coded_width,  32);
            hwframes_ctx->height            = FFALIGN(avctx->coded_height, 32);
            hwframes_ctx->format            = AV_PIX_FMT_AMF;
            hwframes_ctx->sw_format         = avctx->sw_pix_fmt;
            hwframes_ctx->initial_pool_size = 0;

            ret = av_hwframe_ctx_init(avctx->hw_frames_ctx);

            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error initializing a AMF frame pool\n");
                av_buffer_unref(&avctx->hw_frames_ctx);
                return ret;
            }
        }

    }  else {
        AVAMFDeviceContextInternal *wrapped = av_mallocz(sizeof(*wrapped));
        ctx->amf_device_ctx_internal = av_buffer_create((uint8_t *)wrapped, sizeof(*wrapped),
                                                amf_context_internal_free, NULL, 0);
        if ((ret == amf_context_internal_create((AVAMFDeviceContextInternal *)ctx->amf_device_ctx_internal->data, avctx, "", NULL, 0)) != 0) {
            amf_decode_close(avctx);
            return ret;
        }
        if ((ret = amf_init_decoder_context(avctx)) != 0) {
            return ret;
        }
    }
    if ((ret = amf_init_decoder(avctx)) == 0) {
        return 0;
    }
    amf_decode_close(avctx);
    return ret;
}

static AMF_RESULT amf_get_property_buffer(AMFData *object, const wchar_t *name, AMFBuffer **val)
{
    AMF_RESULT res;
    AMFVariantStruct var;
    res = AMFVariantInit(&var);
    if (res == AMF_OK) {
        res = object->pVtbl->GetProperty(object, name, &var);
        if (res == AMF_OK) {
            if (var.type == AMF_VARIANT_INTERFACE) {
                AMFGuid guid_AMFBuffer = IID_AMFBuffer();
                AMFInterface *amf_interface = AMFVariantInterface(&var);
                res = amf_interface->pVtbl->QueryInterface(amf_interface, &guid_AMFBuffer, (void**)val);
            } else {
                res = AMF_INVALID_DATA_TYPE;
            }
        }
        AMFVariantClear(&var);
    }
    return res;
}

static int amf_amfsurface_to_avframe(AVCodecContext *avctx, AMFSurface* surface, AVFrame *frame)
{
    AMFVariantStruct    var = {0};
    AMFPlane            *plane;
    int                 i;
    AMF_RESULT          ret = AMF_OK;

    if (!frame)
        return AMF_INVALID_POINTER;

    if (avctx->hw_frames_ctx) {
        AVHWFramesContext *hwframes_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
        if (hwframes_ctx->format == AV_PIX_FMT_AMF) {
            frame->data[3] = surface;
            frame->format = AV_PIX_FMT_AMF;
            frame->hw_frames_ctx = av_buffer_ref(avctx->hw_frames_ctx);
            // FIXME: Need to find how to delete this buffer creation
            frame->buf[0] = av_buffer_create(NULL,
                                     0,
                                     amf_free_amfsurface,
                                     surface,
                                     AV_BUFFER_FLAG_READONLY);
            surface->pVtbl->Acquire(surface);
        } else {
            av_log(avctx, AV_LOG_ERROR, "Unknown format for hwframes_ctx\n");
            return AVERROR(ENOMEM);
        }
    } else {
        switch (surface->pVtbl->GetMemoryType(surface))
        {
#if CONFIG_D3D11VA
            case AMF_MEMORY_DX11:
            {
                AMFPlane *plane0 = surface->pVtbl->GetPlaneAt(surface, 0);
                frame->data[0] = plane0->pVtbl->GetNative(plane0);
                frame->linesize[0] = plane0->pVtbl->GetHPitch(plane0);
                frame->data[1] = (uint8_t*)(intptr_t)0;

                frame->buf[0] = av_buffer_create(NULL,
                                         0,
                                         amf_free_amfsurface,
                                         surface,
                                         AV_BUFFER_FLAG_READONLY);
                surface->pVtbl->Acquire(surface);
            }
            break;
#endif
#if CONFIG_DXVA2
            case AMF_MEMORY_DX9:
            {
                AMFPlane *plane0 = surface->pVtbl->GetPlaneAt(surface, 0);
                frame->data[3] = plane0->pVtbl->GetNative(plane0);

                frame->buf[0] = av_buffer_create(NULL,
                                         0,
                                         amf_free_amfsurface,
                                         surface,
                                         AV_BUFFER_FLAG_READONLY);
                surface->pVtbl->Acquire(surface);
            }
            break;
    #endif
        default:
            {
                ret = surface->pVtbl->Convert(surface, AMF_MEMORY_HOST);
                AMF_RETURN_IF_FALSE(avctx, ret == AMF_OK, AMF_UNEXPECTED, "Convert(amf::AMF_MEMORY_HOST) failed with error %d\n", ret);

                for (i = 0; i < surface->pVtbl->GetPlanesCount(surface); i++)
                {
                    plane = surface->pVtbl->GetPlaneAt(surface, i);
                    frame->data[i] = plane->pVtbl->GetNative(plane);
                    frame->linesize[i] = plane->pVtbl->GetHPitch(plane);
                }
                surface->pVtbl->Acquire(surface);
                frame->buf[0] = av_buffer_create(NULL,
                                                     0,
                                                     amf_free_amfsurface,
                                                     surface,
                                                     AV_BUFFER_FLAG_READONLY);
           }
        }
        frame->format = amf_to_av_format(surface->pVtbl->GetFormat(surface));
    }

    frame->width  = avctx->width;
    frame->height = avctx->height;

    frame->pts = surface->pVtbl->GetPts(surface);

    surface->pVtbl->GetProperty(surface, L"FFMPEG:dts", &var);
    frame->pkt_dts = var.int64Value;

    frame->duration = surface->pVtbl->GetDuration(surface);

#if FF_API_FRAME_PKT
FF_DISABLE_DEPRECATION_WARNINGS
    surface->pVtbl->GetProperty(surface, L"FFMPEG:size", &var);
    frame->pkt_size = var.int64Value;
    surface->pVtbl->GetProperty(surface, L"FFMPEG:pos", &var);
    frame->pkt_pos = var.int64Value;
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    frame->color_range = avctx->color_range;

    frame->colorspace = avctx->colorspace;
    frame->color_trc = avctx->color_trc;
    frame->color_primaries = avctx->color_primaries;

    if (frame->color_trc == AVCOL_TRC_SMPTE2084) {
        AMFBuffer * hdrmeta_buffer = NULL;
        ret = amf_get_property_buffer((AMFData *)surface, AMF_VIDEO_DECODER_HDR_METADATA, &hdrmeta_buffer);
        if (hdrmeta_buffer != NULL) {
            AMFHDRMetadata * hdrmeta = (AMFHDRMetadata*)hdrmeta_buffer->pVtbl->GetNative(hdrmeta_buffer);
            if (ret != AMF_OK)
                return ret;
            if (hdrmeta != NULL) {
                AVMasteringDisplayMetadata *mastering = av_mastering_display_metadata_create_side_data(frame);
                const int chroma_den = 50000;
                const int luma_den = 10000;

                if (!mastering)
                    return AVERROR(ENOMEM);

                mastering->display_primaries[0][0] = av_make_q(hdrmeta->redPrimary[0], chroma_den);
                mastering->display_primaries[0][1] = av_make_q(hdrmeta->redPrimary[1], chroma_den);

                mastering->display_primaries[1][0] = av_make_q(hdrmeta->greenPrimary[0], chroma_den);
                mastering->display_primaries[1][1] = av_make_q(hdrmeta->greenPrimary[1], chroma_den);

                mastering->display_primaries[2][0] = av_make_q(hdrmeta->bluePrimary[0], chroma_den);
                mastering->display_primaries[2][1] = av_make_q(hdrmeta->bluePrimary[1], chroma_den);

                mastering->white_point[0] = av_make_q(hdrmeta->whitePoint[0], chroma_den);
                mastering->white_point[1] = av_make_q(hdrmeta->whitePoint[1], chroma_den);

                mastering->max_luminance = av_make_q(hdrmeta->maxMasteringLuminance, luma_den);
                mastering->min_luminance = av_make_q(hdrmeta->maxMasteringLuminance, luma_den);

                mastering->has_luminance = 1;
                mastering->has_primaries = 1;
                if (hdrmeta->maxContentLightLevel) {
                   AVContentLightMetadata *light = av_content_light_metadata_create_side_data(frame);

                    if (!light)
                        return AVERROR(ENOMEM);

                    light->MaxCLL  = hdrmeta->maxContentLightLevel;
                    light->MaxFALL = hdrmeta->maxFrameAverageLightLevel;
                }
            }
        }
    }

    return ret;
}

static AMF_RESULT amf_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;
    AMF_RESULT          ret = AMF_OK;
    AMFSurface          *surface = NULL;
    AVFrame             *data = NULL;
    AMFData             *data_out = NULL;

    if (!ctx->decoder)
        return AVERROR(EINVAL);

    ret = ctx->decoder->pVtbl->QueryOutput(ctx->decoder, &data_out);

    if (data_out == NULL) {
        av_log(avctx, AV_LOG_VERBOSE, "QueryOutput() returned empty data\n");
        return AMF_FAIL;
    }
    if (ret == AMF_EOF) {
        av_log(avctx, AV_LOG_VERBOSE, "QueryOutput() returned AMF_EOF\n");
        return AMF_EOF;
    }

    if (data_out) {
        AMFGuid guid = IID_AMFSurface();
        data_out->pVtbl->QueryInterface(data_out, &guid, (void**)&surface); // query for buffer interface
        data_out->pVtbl->Release(data_out);
        data_out = NULL;
    }

    data = av_frame_alloc();
    ret = amf_amfsurface_to_avframe(avctx, surface, data);
    AMF_GOTO_FAIL_IF_FALSE(avctx, ret == AMF_OK, AVERROR_UNKNOWN, "Failed to convert AMFSurface to AVFrame");

    av_frame_move_ref(frame, data);
fail:
    if (data) {
        av_frame_free(&data);
    }
    if (surface) {
        surface->pVtbl->Release(surface);
        surface = NULL;
    }
    return ret;
}

static AMF_RESULT amf_update_buffer_properties(AVCodecContext *avctx, AMFBuffer* buffer, const AVPacket* pkt)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;
    AVAMFDeviceContextInternal * internal = (AVAMFDeviceContextInternal * )ctx->amf_device_ctx_internal->data;
    AMFContext          *ctxt = internal->context;

    AMF_RESULT          res;

    AMF_RETURN_IF_FALSE(ctxt, buffer != NULL, AMF_INVALID_ARG, "update_buffer_properties() - buffer not passed in");
    AMF_RETURN_IF_FALSE(ctxt, pkt != NULL, AMF_INVALID_ARG, "update_buffer_properties() - packet not passed in");
    buffer->pVtbl->SetPts(buffer, pkt->pts);
    buffer->pVtbl->SetDuration(buffer, pkt->duration);
    AMF_ASSIGN_PROPERTY_INT64(res, buffer, L"FFMPEG:dts", pkt->dts);
    AMF_ASSIGN_PROPERTY_INT64(res, buffer, L"FFMPEG:size", pkt->size);
    AMF_ASSIGN_PROPERTY_INT64(res, buffer, L"FFMPEG:pos", pkt->pos);

    return AMF_OK;
}

static AMF_RESULT amf_buffer_from_packet(AVCodecContext *avctx, const AVPacket* pkt, AMFBuffer** buffer)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;
    AVAMFDeviceContextInternal * internal = (AVAMFDeviceContextInternal * )ctx->amf_device_ctx_internal->data;
    AMFContext          *ctxt = internal->context;
    void                *mem;
    AMF_RESULT          err;
    AMFBuffer           *buf = NULL;

    AMF_RETURN_IF_FALSE(ctxt, pkt != NULL, AMF_INVALID_ARG, "amf_buffer_from_packet() - packet not passed in");
    AMF_RETURN_IF_FALSE(ctxt, buffer != NULL, AMF_INVALID_ARG, "amf_buffer_from_packet() - buffer pointer not passed in");

    err = ctxt->pVtbl->AllocBuffer(ctxt, AMF_MEMORY_HOST, pkt->size + AV_INPUT_BUFFER_PADDING_SIZE, buffer);
    AMF_RETURN_IF_FALSE(ctxt, err == AMF_OK, err, "amf_buffer_from_packet() -   failed");
    buf = *buffer;
    err = buf->pVtbl->SetSize(buf, pkt->size);
    AMF_RETURN_IF_FALSE(ctxt, err == AMF_OK, err, "amf_buffer_from_packet() - SetSize failed");
    // get the memory location and check the buffer was indeed allocated
    mem = buf->pVtbl->GetNative(buf);
    AMF_RETURN_IF_FALSE(ctxt, mem != NULL, AMF_INVALID_POINTER, "amf_buffer_from_packet() - GetNative failed");

    // copy the packet memory and don't forget to
    // clear data padding like it is done by FFMPEG
    memcpy(mem, pkt->data, pkt->size);
    memset((amf_int8*)(mem)+pkt->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    return amf_update_buffer_properties(avctx, buf, pkt);
}

static int amf_decode_frame(AVCodecContext *avctx, AVFrame *data,
                       int *got_frame, AVPacket *avpkt)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;
    AVFrame             *frame = data;
    AMFBuffer           *buf;
    AMF_RESULT          res;

    if (!avpkt->size && ctx->drained == 0) {
        ctx->decoder->pVtbl->Drain(ctx->decoder);
        ctx->drained = 1;
    }

    if (avpkt->size > 0) {
        res = amf_buffer_from_packet(avctx, avpkt, &buf);
        AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, 0, "Cannot convert AVPacket to AMFbuffer");
        res = ctx->decoder->pVtbl->SubmitInput(ctx->decoder, (AMFData*) buf);
        // FIXME: check other return values
        if (res == AMF_OK || res == AMF_NEED_MORE_INPUT)
        {
            *got_frame = 0;
        }
        buf->pVtbl->Release(buf);
        buf = NULL;
    }

    while(1) {
        res = amf_receive_frame(avctx, frame);
        if (res == AMF_OK) {
            AMF_RETURN_IF_FALSE(avctx, !*got_frame, avpkt->size, "frame already got");
            *got_frame = 1;
            break;
        } else if (res == AMF_FAIL || res == AMF_EOF) {
            break;
        } else {
            AMF_RETURN_IF_FALSE(avctx, res, 0, "Unkown result from QueryOutput");
        }
    }
    return avpkt->size;
}

static void amf_decode_flush(AVCodecContext *avctx)
{
    AvAmfDecoderContext *ctx = avctx->priv_data;
    ctx->decoder->pVtbl->Flush(ctx->decoder);
}

#define OFFSET(x) offsetof(AvAmfDecoderContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM

static const AVOption options[] = {
    // Decoder mode
    { "decoder_mode",       "Decoder mode",                                                 OFFSET(decoder_mode),       AV_OPT_TYPE_INT,   { .i64 = AMF_VIDEO_DECODER_MODE_COMPLIANT  }, AMF_VIDEO_DECODER_MODE_REGULAR, AMF_VIDEO_DECODER_MODE_LOW_LATENCY, VD, "decoder_mode" },
    { "regular",            "DPB delay is based on number of reference frames + 1",         0,                          AV_OPT_TYPE_CONST, { .i64 = AMF_VIDEO_DECODER_MODE_REGULAR      }, 0, 0, VD, "decoder_mode" },
    { "compliant",          "DPB delay is based on profile - up to 16",                     0,                          AV_OPT_TYPE_CONST, { .i64 = AMF_VIDEO_DECODER_MODE_COMPLIANT    }, 0, 0, VD, "decoder_mode" },
    { "low_latency",        "DPB delay is 0",                                               0,                          AV_OPT_TYPE_CONST, { .i64 = AMF_VIDEO_DECODER_MODE_LOW_LATENCY  }, 0, 0, VD, "decoder_mode" },

    // Timestamp mode
    { "timestamp_mode",     "Timestamp mode",                                               OFFSET(timestamp_mode),     AV_OPT_TYPE_INT,   { .i64 = AMF_TS_SORT }, AMF_TS_PRESENTATION, AMF_TS_DECODE, VD, "timestamp_mode" },
    { "presentation",       "Preserve timestamps from input to output",                     0,                          AV_OPT_TYPE_CONST, { .i64 = AMF_TS_PRESENTATION }, 0, 0, VD, "timestamp_mode" },
    { "sort",               "Resort PTS list",                                              0,                          AV_OPT_TYPE_CONST, { .i64 = AMF_TS_SORT         }, 0, 0, VD, "timestamp_mode" },
    { "decode",             "Decode order",                                                 0,                          AV_OPT_TYPE_CONST, { .i64 = AMF_TS_DECODE       }, 0, 0, VD, "timestamp_mode" },

    // Reference frame management
    { "surface_pool_size",  "Number of surfaces in the decode pool",                        OFFSET(surface_pool_size),  AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, INT_MAX, VD, NULL },
    { "dpb_size",           "Minimum number of surfaces for reordering",                    OFFSET(dpb_size),           AV_OPT_TYPE_INT,  { .i64 = 1 }, 0, 32, VD, NULL },

    { "lowlatency",         "Low latency",                                                  OFFSET(lowlatency),         AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VD, NULL },
    { "smart_access_video", "Smart Access Video",                                           OFFSET(smart_access_video), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VD, NULL },
    { "skip_transfer_sav",  "Skip transfer on another GPU when SAV enabled",                OFFSET(skip_transfer_sav),  AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VD, NULL },

    { NULL }
};

static const AVClass amf_decode_class = {
    .class_name = "amf",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};


#define DEFINE_AMF_DECODER(x, X, bsf_name) \
const FFCodec ff_##x##_amf_decoder = { \
    .p.name         = #x "_amf", \
    CODEC_LONG_NAME(#X " AMD AMF video decoder"), \
    .priv_data_size = sizeof(AvAmfDecoderContext), \
    .p.type         = AVMEDIA_TYPE_VIDEO, \
    .p.id           = AV_CODEC_ID_##X, \
    .init           = amf_decode_init, \
    FF_CODEC_DECODE_CB(amf_decode_frame), \
    .flush          = amf_decode_flush, \
    .close          = amf_decode_close, \
    .bsfs           = bsf_name, \
    .p.capabilities = AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING, \
    .p.priv_class   = &amf_decode_class, \
    .p.pix_fmts     = amf_dec_pix_fmts, \
    .hw_configs     = amf_hw_configs, \
    .p.wrapper_name = "amf", \
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE, \
}; \

DEFINE_AMF_DECODER(h264, H264, "h264_mp4toannexb")
DEFINE_AMF_DECODER(hevc, HEVC, NULL)
DEFINE_AMF_DECODER(av1, AV1, NULL)
