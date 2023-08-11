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

#ifndef AVFILTER_AMF_COMMON_H
#define AVFILTER_AMF_COMMON_H

#include "avfilter.h"

#include "AMF/core/Surface.h"
#include "AMF/components/Component.h"

typedef struct AMFScaleContext {
    const AVClass *class;

    int width, height;
    enum AVPixelFormat format;
    int scale_type;
    int color_profile;
    int color_range;
    int primaries;
    int trc;

    char *w_expr;
    char *h_expr;
    char *format_str;

    AMFComponent        *scaler;
    AVBufferRef         *amf_device_ref;

    AVBufferRef         *hwframes_in_ref;
    AVBufferRef         *hwframes_out_ref;
    AVBufferRef         *hwdevice_ref;

    AVBufferRef        *amf_device_ctx_internal;
} AMFScaleContext;

int amf_copy_surface(AVFilterContext *avctx, const AVFrame *frame, AMFSurface* surface);
void amf_free_amfsurface(void *opaque, uint8_t *data);
AVFrame *amf_amfsurface_to_avframe(AVFilterContext *avctx, AMFSurface* pSurface);
int amf_avframe_to_amfsurface(AVFilterContext *avctx, const AVFrame *frame, AMFSurface** ppSurface);

#endif /* AVFILTER_AMF_COMMON_H */
