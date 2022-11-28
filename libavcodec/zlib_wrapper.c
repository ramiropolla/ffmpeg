/*
 * Wrappers for zlib
 * Copyright (C) 2022 Andreas Rheinhardt
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

#include <zlib.h>

#include "config.h"
#include "libavutil/error.h"
#include "libavutil/thread.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "zlib_wrapper.h"

static void *alloc_wrapper(void *opaque, uInt items, uInt size)
{
    return av_malloc_array(items, size);
}

static void free_wrapper(void *opaque, void *ptr)
{
    av_free(ptr);
}

#if CONFIG_INFLATE_WRAPPER
int ff_inflate_init(FFZStream *z, void *logctx)
{
    z_stream *const zstream = &z->zstream;
    int zret;

    z->inited = 0;
    zstream->next_in  = Z_NULL;
    zstream->avail_in = 0;
    zstream->zalloc   = alloc_wrapper;
    zstream->zfree    = free_wrapper;
    zstream->opaque   = Z_NULL;

    zret = inflateInit(zstream);
    if (zret == Z_OK) {
        z->inited = 1;
    } else {
        av_log(logctx, AV_LOG_ERROR, "inflateInit error %d, message: %s\n",
               zret, zstream->msg ? zstream->msg : "");
        return AVERROR_EXTERNAL;
    }
    return 0;
}

void ff_inflate_end(FFZStream *z)
{
    if (z->inited) {
        z->inited = 0;
        inflateEnd(&z->zstream);
    }
}
#endif

#if CONFIG_DEFLATE_WRAPPER
int ff_deflate_init(FFZStream *z, int level, void *logctx)
{
    z_stream *const zstream = &z->zstream;
    int zret;

    z->inited = 0;
    zstream->zalloc = alloc_wrapper;
    zstream->zfree  = free_wrapper;
    zstream->opaque = Z_NULL;

    zret = deflateInit(zstream, level);
    if (zret == Z_OK) {
        z->inited = 1;
    } else {
        av_log(logctx, AV_LOG_ERROR, "deflateInit error %d, message: %s\n",
               zret, zstream->msg ? zstream->msg : "");
        return AVERROR_EXTERNAL;
    }
    return 0;
}

void ff_deflate_end(FFZStream *z)
{
    if (z->inited) {
        z->inited = 0;
        deflateEnd(&z->zstream);
    }
}


/* MT */
#define DICT_SIZE  (1 << 15) //  32Kb
#define CHUNK_SIZE (1 << 17) // 128Kb

static void *deflate_thread(void *arg);

int ff_deflate_init_mt(FFZStreamMT *z, int level, int thread_count, void *logctx)
{
    int ret;

    if (level < 0 || level > 9) {
        av_log(logctx, AV_LOG_ERROR, "Compression level should be 0-9, not %i\n", level);
        return AVERROR(EINVAL);
    }

    z->streams = NULL;
    z->state = 0;
    z->level = level;
    z->thread_count = thread_count;
    z->total_out = 0;
    z->last = -1;
    z->cur_index = 0;
    z->wake_up = -1;
    z->out_chunk = 1;
    z->chunk_progress = 0;
    pthread_mutex_init(&z->stream_lock, NULL);
    pthread_cond_init(&z->stream_cond, NULL);
    pthread_mutex_init(&z->chunk_lock, NULL);
    pthread_cond_init(&z->chunk_cond_1, NULL);
    pthread_cond_init(&z->chunk_cond_2, NULL);

    z->streams = av_calloc(thread_count, sizeof(FFZStream2));
    if (!z->streams)
        goto error_nomem;

    for (int i = 0; i < thread_count; i++) {
        FFZStream2 *stream = &z->streams[i];
        z_stream *zstream = &stream->zstream;
        int zret;

        stream->inited = 0;

        zstream->zalloc = alloc_wrapper;
        zstream->zfree  = free_wrapper;
        zstream->opaque = Z_NULL;

        zret = deflateInit2(zstream, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
        if (zret == Z_OK) {
            stream->buf_len = deflateBound(zstream, CHUNK_SIZE);
            stream->inited = 1;
            stream->buf = av_malloc(stream->buf_len);
            if (!stream->buf)
                goto error_nomem;
            pthread_create(&stream->thread, NULL, deflate_thread, z);
            stream->inited = 2;
        } else {
            av_log(logctx, AV_LOG_ERROR, "deflateInit error %d, message: %s\n",
                   zret, zstream->msg ? zstream->msg : "");
            goto error_external;
        }
    }
    return 0;

error_nomem:
    ret = AVERROR(ENOMEM);
    goto the_end;

error_external:
    ret = AVERROR_EXTERNAL;

the_end:
    ff_deflate_end_mt(z);
    return ret;
}

static int get_idle_worker(FFZStreamMT *z, int chunk_order)
{
    int cur_index = -1;

    pthread_mutex_lock(&z->stream_lock);

    cur_index = z->cur_index;
    for (;;) {
        for (int i = 0; i < z->thread_count; i++) {
            FFZStream2 *stream = &z->streams[cur_index];
            if (stream->chunk_order == 0) {
                stream->chunk_order = chunk_order;
                goto got_it;
            }
            cur_index = (cur_index + 1) % z->thread_count;
        }
        pthread_cond_wait(&z->stream_cond, &z->stream_lock);
    }

got_it:
    z->cur_index = (cur_index + 1) % z->thread_count;
    pthread_cond_broadcast(&z->stream_cond);
    pthread_mutex_unlock(&z->stream_lock);

    return cur_index;
}

static void worker_finished(FFZStreamMT *z, FFZStream2 *stream)
{
    z_stream *const zstream = &stream->zstream;

    pthread_mutex_lock(&z->stream_lock);

    for (;;) {
        if (z->out_chunk == stream->chunk_order) {
            /* merge stream buf back into out_buf */
            if (stream->chunk_order != 1)
                memcpy(z->out_buf, stream->buf, zstream->total_out);
            z->out_buf += zstream->total_out;
            z->out_size -= zstream->total_out;
            z->total_out += zstream->total_out;
            z->chunk_progress = stream->chunk_order;
            stream->chunk_order = 0;
            z->out_chunk++;
            goto got_it;
        }
        pthread_cond_wait(&z->stream_cond, &z->stream_lock);
    }

got_it:
    pthread_cond_broadcast(&z->stream_cond);
    pthread_mutex_unlock(&z->stream_lock);
}

static void wake_up_worker(FFZStreamMT *z, int index)
{
    pthread_mutex_lock(&z->chunk_lock);
    while (z->wake_up != -1)
        pthread_cond_wait(&z->chunk_cond_1, &z->chunk_lock);
    z->wake_up = index;
    pthread_cond_signal(&z->chunk_cond_2);
    pthread_mutex_unlock(&z->chunk_lock);
}

static void *deflate_thread(void *arg)
{
    FFZStreamMT *z = (FFZStreamMT *) arg;

    ff_thread_setname("deflate");

    for (;;)
    {
        FFZStream2 *stream;
        int cur_index;

        pthread_mutex_lock(&z->chunk_lock);
        while (z->wake_up == -1)
            pthread_cond_wait(&z->chunk_cond_2, &z->chunk_lock);
        cur_index = z->wake_up;
        z->wake_up = -1;
        pthread_cond_signal(&z->chunk_cond_1);
        pthread_mutex_unlock(&z->chunk_lock);
        /* poison_pill */
        if (cur_index == -2)
            return NULL;

        /* TODO check for errors */
        stream = &z->streams[cur_index];
        deflate(&stream->zstream, Z_SYNC_FLUSH);
        worker_finished(z, stream);
    }
}

int ff_deflate_mt(FFZStreamMT *z, uint8_t *out_buf, size_t out_size, const uint8_t *in_buf, size_t in_size)
{
    const uint8_t *next_dict = NULL;
    int chunk_order = 2;
    int last_index = z->last;
    int ret;

    z->total_out = 0;

    if (in_size == 0)
        return Z_OK;

    if (z->state == 0) {
        /* write zlib header */
        static const uint8_t flg[] = { 0x01, 0x01, 0x5e, 0x5e, 0x5e,
                                       0x5e, 0x9c, 0xda, 0xda, 0xda };
        *out_buf++ = 0x78;
        *out_buf++ = flg[z->level];
        out_size -= 2;
        z->total_out += 2;
        z->state = 1;
    }

    z->out_chunk = 1;
    z->chunk_progress = 0;
    z->out_buf = out_buf;
    z->out_size = out_size;

    if (z->last != -1) {
        FFZStream2 *stream = &z->streams[z->last];
        z_stream *const zstream = &stream->zstream;
        size_t in_size_chunk = FFMIN(in_size, CHUNK_SIZE);

        /* no need to update dictionary */

        zstream->next_in   = in_buf;
        zstream->avail_in  = in_size_chunk;
        zstream->total_in  = 0;

        zstream->next_out  = out_buf;
        zstream->avail_out = out_size;
        zstream->total_out = 0;

        /* TODO check for errors */
        stream->chunk_order = 1; /* is_last */
        wake_up_worker(z, z->last);

        in_buf += in_size_chunk;
        in_size -= in_size_chunk;
        if (in_size)
            next_dict = in_buf - DICT_SIZE;

        z->last = -1;
    } else {
        z->out_chunk++;
    }

    while (in_size) {
        int index = get_idle_worker(z, chunk_order++);
        FFZStream2 *stream = &z->streams[index];
        z_stream *const zstream = &stream->zstream;
        size_t in_size_chunk = FFMIN(in_size, CHUNK_SIZE);

        /* update dictionary */
        if (next_dict) {
            deflateReset(zstream);
            deflateSetDictionary(zstream, next_dict, DICT_SIZE);
            next_dict = NULL;
        }

        zstream->next_in   = in_buf;
        zstream->avail_in  = in_size_chunk;
        zstream->total_in  = 0;

        zstream->next_out  = stream->buf;
        zstream->avail_out = stream->buf_len;
        zstream->total_out = 0;

        /* TODO check for errors */
        wake_up_worker(z, index);

        in_buf += in_size_chunk;
        in_size -= in_size_chunk;
        if (in_size)
            next_dict = in_buf - DICT_SIZE;

        last_index = index;
    }

    z->last = last_index;

    /* wait for all chunks to finish writing */
    pthread_mutex_lock(&z->stream_lock);
    while (z->chunk_progress != (chunk_order-1))
        pthread_cond_wait(&z->stream_cond, &z->stream_lock);
    pthread_cond_signal(&z->stream_cond);
    pthread_mutex_unlock(&z->stream_lock);

    return ret;
}

int ff_deflate_reset_mt(FFZStreamMT *z)
{
    int fret = Z_OK;
    for (int i = 0; i < z->thread_count; i++) {
        FFZStream2 *stream = &z->streams[i];
        if (stream->inited) {
            int ret = deflateReset(&stream->zstream);
            if (ret != Z_OK)
                fret = ret;
        }
    }
    z->state = 0;
    return fret;
}

void ff_deflate_end_mt(FFZStreamMT *z)
{
    if (z->streams) {
        /* poison pill */
        for (int i = 0; i < z->thread_count; i++)
            wake_up_worker(z, -2);
        for (int i = 0; i < z->thread_count; i++) {
            FFZStream2 *stream = &z->streams[i];
            if (stream->inited) {
                if (stream->inited == 2)
                    pthread_join(stream->thread, NULL);
                stream->inited = 0;
                deflateEnd(&stream->zstream);
            }
            if (stream->buf)
                av_freep(&stream->buf);
        }
    }
    if (z->streams)
        av_freep(&z->streams);
    pthread_mutex_destroy(&z->stream_lock);
    pthread_cond_destroy(&z->stream_cond);
    pthread_mutex_destroy(&z->chunk_lock);
    pthread_cond_destroy(&z->chunk_cond_1);
    pthread_cond_destroy(&z->chunk_cond_2);
}
#endif
