/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Derived from the PipeWire consumer in OBS Studio / ShaderGlass WineCap:
 *   Copyright 2020 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *   SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "prism_log.h"
#include "pipewire.h"

#include <fcntl.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/debug/types.h>
#include <spa/utils/defs.h>

/* BGRx/BGRA are byte-identical to DXGI_FORMAT_B8G8R8A8_UNORM and cost nothing.
 * RGBx/RGBA are accepted as a second choice and swapped explicitly on the PE
 * side, which is counted and shown in diagnostics rather than hidden. */
#define PRISM_MAX_DIMENSION 16384

struct prism_pw
{
    int      pipewire_fd;
    uint32_t pipewire_node;

    struct pw_loop*    loop;
    struct pw_context* ctx;
    struct pw_core*    core;
    struct pw_stream*  stream;

    struct spa_hook core_listener;
    struct spa_hook stream_listener;

    struct pw_core_events   core_events;
    struct pw_stream_events stream_events;

    struct spa_video_info format;
    bool                  have_format;

    struct prism_pw_callbacks cb;

    /* Delivery pacing. Owned by the capture thread. */
    unsigned long long min_interval_ns;
    unsigned long long last_delivery_ns;

    /* Counters. Written only by the capture thread, read by anyone. */
    volatile unsigned long long received;
    volatile unsigned long long delivered;
    volatile unsigned long long throttled;
    volatile unsigned long long corrupt;
    volatile unsigned long long sequence;
    volatile unsigned int       last_stride;
    volatile unsigned int       last_maxsize;
    volatile double             callback_ms;
};

static struct prism_pw pw_state;

/* ------------------------------------------------------------ core hooks -- */

static void on_core_info(void* user_data, const struct pw_core_info* info)
{
    (void)user_data;
    prism_debug("PipeWire core: %s (cookie %u)", info->name ? info->name : "?", info->cookie);
}

static void on_core_done(void* user_data, uint32_t id, int seq)
{
    (void)user_data;
    (void)id;
    (void)seq;
}

static void on_core_error(void* user_data, uint32_t id, int seq, int res, const char* message)
{
    (void)user_data;
    (void)seq;
    prism_warn("PipeWire error on id %u: %s (%d)", id, message ? message : "?", res);
    if(pw_state.cb.on_state)
        pw_state.cb.on_state(PRISM_STATE_ERROR, message ? message : "PipeWire error");
}

/* ---------------------------------------------------------- format setup -- */

static const struct spa_pod* prism_build_format(struct spa_pod_builder* builder)
{
    /* BGRx first: it is what KWin hands out for opaque sources and it needs no
     * conversion anywhere in the pipeline. The framerate range is deliberately
     * open-ended so a 240 Hz source is not clamped to 60. */
    /* The first entry of a CHOICE_ENUM is the preferred value and is repeated
     * as the first alternative, so BGRx wins whenever the source can offer it. */
    return spa_pod_builder_add_object(
        builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
        SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                               SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(1920, 1080), &SPA_RECTANGLE(1, 1),
                                       &SPA_RECTANGLE(PRISM_MAX_DIMENSION, PRISM_MAX_DIMENSION)),
        SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)));
}

static unsigned int prism_map_format(uint32_t spa_format)
{
    switch(spa_format)
    {
    case SPA_VIDEO_FORMAT_BGRx: return PRISM_FORMAT_BGRX;
    case SPA_VIDEO_FORMAT_BGRA: return PRISM_FORMAT_BGRA;
    case SPA_VIDEO_FORMAT_RGBx: return PRISM_FORMAT_RGBX;
    case SPA_VIDEO_FORMAT_RGBA: return PRISM_FORMAT_RGBA;
    default: return 0;
    }
}

static void on_param_changed(void* user_data, uint32_t id, const struct spa_pod* param)
{
    struct prism_pw*       s = user_data;
    struct spa_pod_builder builder;
    const struct spa_pod*  params[2];
    uint8_t                buffer[1024];
    uint32_t               n_params = 0;
    unsigned int           mapped;

    if(param == NULL || id != SPA_PARAM_Format)
        return;

    if(spa_format_parse(param, &s->format.media_type, &s->format.media_subtype) < 0)
        return;
    if(s->format.media_type != SPA_MEDIA_TYPE_video || s->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if(spa_format_video_raw_parse(param, &s->format.info.raw) < 0)
        return;

    mapped = prism_map_format(s->format.info.raw.format);
    if(mapped == 0)
    {
        prism_warn("negotiated an unsupported pixel layout (%s), refusing to guess",
                   spa_debug_type_find_name(spa_type_video_format, s->format.info.raw.format));
        if(s->cb.on_state)
            s->cb.on_state(PRISM_STATE_ERROR, "unsupported pixel format from PipeWire");
        return;
    }

    s->have_format = true;
    prism_info("stream format: %ux%u %s @ %u/%u", s->format.info.raw.size.width, s->format.info.raw.size.height,
               spa_debug_type_find_name(spa_type_video_format, s->format.info.raw.format),
               s->format.info.raw.framerate.num, s->format.info.raw.framerate.denom);

    if(s->cb.on_format)
        s->cb.on_format(s->format.info.raw.size.width, s->format.info.raw.size.height, mapped,
                        s->format.info.raw.framerate.num, s->format.info.raw.framerate.denom);

    /* Ask for the crop rectangle (KWin uses it for window sources whose buffer
     * is padded) and for the header, which carries the presentation stamp we
     * report as capture latency. */
    spa_pod_builder_init(&builder, buffer, sizeof(buffer));
    params[n_params++] = spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region)));
    params[n_params++] = spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

    pw_stream_update_params(s->stream, params, n_params);
}

static void on_state_changed(void* user_data, enum pw_stream_state old, enum pw_stream_state state, const char* error)
{
    struct prism_pw* s = user_data;

    (void)old;
    prism_info("stream state: %s%s%s", pw_stream_state_as_string(state), error ? " - " : "", error ? error : "");

    if(!s->cb.on_state)
        return;

    switch(state)
    {
    case PW_STREAM_STATE_STREAMING: s->cb.on_state(PRISM_STATE_ACTIVE, "streaming"); break;
    case PW_STREAM_STATE_ERROR: s->cb.on_state(PRISM_STATE_ERROR, error ? error : "stream error"); break;
    case PW_STREAM_STATE_UNCONNECTED: s->cb.on_state(PRISM_STATE_IDLE, "stream closed"); break;
    default: break;
    }
}

/* ---------------------------------------------------------- frame arrival -- */

static void on_process(void* user_data)
{
    struct prism_pw*        s = user_data;
    struct pw_buffer*       b;
    struct spa_buffer*      buf;
    struct spa_data*        d;
    struct spa_chunk*       chunk;
    struct spa_meta_region* crop;
    struct spa_meta_header* header;
    PrismFrame              frame;
    const uint8_t*          base;
    const uint8_t*          pixels;
    unsigned int            width, height, stride;
    unsigned long long      available;
    unsigned long long      required;
    unsigned long long      now;
    unsigned long long      entered;

    entered = prism_now_ns();

    /* Drain to the newest queued buffer: PipeWire hands us a queue, and Prism
     * only ever cares about the freshest entry. Anything older is recycled
     * without being copied. */
    b = NULL;
    for(;;)
    {
        struct pw_buffer* next = pw_stream_dequeue_buffer(s->stream);
        if(next == NULL)
            break;
        if(b != NULL)
        {
            s->throttled++;
            pw_stream_queue_buffer(s->stream, b);
        }
        b = next;
    }
    if(b == NULL)
        return;

    s->received++;
    buf = b->buffer;

    if(!s->have_format || buf->n_datas < 1)
    {
        s->corrupt++;
        goto recycle;
    }

    d     = &buf->datas[0];
    chunk = d->chunk;

    if(d->data == NULL)
    {
        /* No mapped pointer: almost always a DMA-BUF-only buffer, which v0.1
         * does not import. Recycle it rather than guessing. */
        s->corrupt++;
        goto recycle;
    }

    width  = s->format.info.raw.size.width;
    height = s->format.info.raw.size.height;
    stride = chunk ? (unsigned int)chunk->stride : 0u;

    /* A negotiated format is not a promise about layout. Rows are frequently
     * padded, and a chunk can start partway into the mapping, so every bound is
     * derived from what this buffer actually reports and then checked. Nothing
     * here assumes a tightly packed BGRx image. */
    if(stride == 0u)
        stride = width * 4u;

    base      = (const uint8_t*)d->data + (chunk ? chunk->offset : 0u);
    available = d->maxsize > (chunk ? chunk->offset : 0u) ? d->maxsize - (chunk ? chunk->offset : 0u) : 0ull;
    if(chunk && chunk->size > 0 && (unsigned long long)chunk->size < available)
        available = chunk->size;

    s->last_stride  = stride;
    s->last_maxsize = (unsigned int)d->maxsize;

    if(width == 0u || height == 0u || width > PRISM_MAX_DIMENSION || height > PRISM_MAX_DIMENSION ||
       stride < width * 4u)
    {
        prism_warn("rejecting a frame with implausible geometry: %ux%u stride %u", width, height, stride);
        s->corrupt++;
        goto recycle;
    }

    pixels = base;

    crop = spa_buffer_find_meta_data(buf, SPA_META_VideoCrop, sizeof(*crop));
    if(crop && spa_meta_region_is_valid(crop) &&
       (crop->region.size.width != width || crop->region.size.height != height))
    {
        const unsigned int crop_x = crop->region.position.x;
        const unsigned int crop_y = crop->region.position.y;

        if(crop_x + crop->region.size.width > width || crop_y + crop->region.size.height > height ||
           crop->region.size.width == 0u || crop->region.size.height == 0u)
        {
            prism_warn("ignoring an out-of-range crop rectangle: %ux%u at %u,%u of %ux%u",
                       crop->region.size.width, crop->region.size.height, crop_x, crop_y, width, height);
        }
        else
        {
            const unsigned long long crop_offset = (unsigned long long)crop_x * 4ull +
                                                   (unsigned long long)crop_y * (unsigned long long)stride;
            pixels = base + crop_offset;
            available = available > crop_offset ? available - crop_offset : 0ull;
            width     = crop->region.size.width;
            height    = crop->region.size.height;
        }
    }

    /* The last row only needs its visible pixels, not a full padded stride. */
    required = (unsigned long long)(height - 1u) * (unsigned long long)stride + (unsigned long long)width * 4ull;
    if(required > available)
    {
        prism_warn("rejecting a short frame: %ux%u stride %u needs %llu bytes, buffer offers %llu", width, height,
                   stride, required, available);
        s->corrupt++;
        goto recycle;
    }

    now = prism_now_ns();

    /* Delivery ceiling. Enforced before the copy so a throttled frame costs
     * nothing beyond the dequeue. Prism never duplicates frames to reach a
     * target rate, it only refuses to consume faster than the ceiling. */
    if(s->min_interval_ns != 0 && s->last_delivery_ns != 0 && now - s->last_delivery_ns < s->min_interval_ns)
    {
        s->throttled++;
        goto recycle;
    }
    s->last_delivery_ns = now;

    header          = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*header));
    frame.data      = pixels;
    frame.width     = width;
    frame.height    = height;
    frame.pitch     = stride;
    frame.format    = prism_map_format(s->format.info.raw.format);
    frame.pts_ns    = (header && header->pts > 0) ? (unsigned long long)header->pts : 0ull;
    frame.recv_ns   = now;
    frame.sequence  = ++s->sequence;
    frame.data_size = available;

    if(s->cb.on_frame)
        s->cb.on_frame(&frame);
    s->delivered++;

recycle:
    pw_stream_queue_buffer(s->stream, b);
    s->callback_ms = (double)(prism_now_ns() - entered) / 1000000.0;
}

/* ---------------------------------------------------------------- public -- */

static int prism_pipewire_connect_core(struct prism_pw* s)
{
    s->core = pw_context_connect_fd(s->ctx, fcntl(s->pipewire_fd, F_DUPFD_CLOEXEC, 5), NULL, 0);
    if(!s->core)
    {
        prism_warn("could not connect to the PipeWire remote handed over by the portal");
        return -1;
    }

    spa_zero(s->core_events);
    s->core_events.version = PW_VERSION_CORE_EVENTS;
    s->core_events.info    = on_core_info;
    s->core_events.done    = on_core_done;
    s->core_events.error   = on_core_error;
    pw_core_add_listener(s->core, &s->core_listener, &s->core_events, s);
    return 0;
}

static int prism_pipewire_connect_stream(struct prism_pw* s)
{
    struct pw_properties*  props;
    struct spa_pod_builder builder;
    const struct spa_pod*  params[1];
    uint8_t                buffer[1024];
    int                    res;

    props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE,
                              "Screen", PW_KEY_NODE_NAME, "prism-capture", NULL);

    spa_zero(s->stream_events);
    s->stream_events.version       = PW_VERSION_STREAM_EVENTS;
    s->stream_events.param_changed = on_param_changed;
    s->stream_events.state_changed = on_state_changed;
    s->stream_events.process       = on_process;

    s->stream = pw_stream_new(s->core, "Prism Capture", props);
    if(!s->stream)
    {
        prism_warn("pw_stream_new failed");
        return -1;
    }
    pw_stream_add_listener(s->stream, &s->stream_listener, &s->stream_events, s);

    spa_pod_builder_init(&builder, buffer, sizeof(buffer));
    params[0] = prism_build_format(&builder);

    res = pw_stream_connect(s->stream, PW_DIRECTION_INPUT, s->pipewire_node,
                            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
    if(res != 0)
    {
        prism_warn("pw_stream_connect failed: %d", res);
        return -1;
    }

    pw_stream_set_active(s->stream, true);
    return 0;
}

int prism_pipewire_start(struct pw_loop* loop, struct pw_context* ctx, int pipewire_fd, uint32_t pipewire_node,
                         const struct prism_pw_callbacks* callbacks)
{
    unsigned long long min_interval = pw_state.min_interval_ns;

    spa_zero(pw_state);
    pw_state.loop            = loop;
    pw_state.ctx             = ctx;
    pw_state.pipewire_fd     = pipewire_fd;
    pw_state.pipewire_node   = pipewire_node;
    pw_state.cb              = *callbacks;
    pw_state.min_interval_ns = min_interval;

    if(prism_pipewire_connect_core(&pw_state) < 0 || prism_pipewire_connect_stream(&pw_state) < 0)
    {
        prism_pipewire_stop();
        return -1;
    }
    return 0;
}

void prism_pipewire_stop(void)
{
    if(pw_state.stream)
    {
        pw_stream_set_active(pw_state.stream, false);
        pw_stream_disconnect(pw_state.stream);
        pw_stream_destroy(pw_state.stream);
        pw_state.stream = NULL;
    }
    pw_state.core = NULL;
    if(pw_state.pipewire_fd > 0)
    {
        close(pw_state.pipewire_fd);
        pw_state.pipewire_fd = 0;
    }
    pw_state.cb.on_frame  = NULL;
    pw_state.cb.on_format = NULL;
    pw_state.have_format  = false;
}

void prism_pipewire_set_max_fps(unsigned int max_fps)
{
    pw_state.min_interval_ns = max_fps ? (1000000000ull / max_fps) : 0ull;
}

void prism_pipewire_get_counters(unsigned long long* received, unsigned long long* delivered,
                                 unsigned long long* throttled, unsigned long long* corrupt)
{
    if(received)
        *received = pw_state.received;
    if(delivered)
        *delivered = pw_state.delivered;
    if(throttled)
        *throttled = pw_state.throttled;
    if(corrupt)
        *corrupt = pw_state.corrupt;
}

void prism_pipewire_get_layout(unsigned int* stride, unsigned int* maxsize, double* callback_ms)
{
    if(stride)
        *stride = pw_state.last_stride;
    if(maxsize)
        *maxsize = pw_state.last_maxsize;
    if(callback_ms)
        *callback_ms = pw_state.callback_ms;
}
