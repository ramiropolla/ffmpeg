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

#ifndef AVCODEC_ZLIB_WRAPPER_H
#define AVCODEC_ZLIB_WRAPPER_H

#include "libavutil/thread.h"
#include <zlib.h>

typedef struct FFZStream {
    z_stream zstream;
    int inited;
} FFZStream;

typedef struct FFZStream2 {
    z_stream zstream;
    int inited;
    uint8_t *buf;
    size_t buf_len;
    int chunk_order;
    pthread_t thread;
} FFZStream2;

typedef struct FFZStreamMT {
    FFZStream2 *streams;
    int state;
    int level;
    int thread_count;
    int total_out;
    int last;
    int cur_index;
    int wake_up;
    int out_chunk;
    int chunk_progress;
    uint8_t *out_buf;
    size_t out_size;
    pthread_mutex_t stream_lock;
    pthread_cond_t stream_cond;
    pthread_mutex_t chunk_lock;
    pthread_cond_t chunk_cond_1;
    pthread_cond_t chunk_cond_2;
} FFZStreamMT;

/**
 * Wrapper around inflateInit(). It initializes the fields that zlib
 * requires to be initialized before inflateInit().
 * In case of error it also returns an error message to the provided logctx;
 * in any case, it sets zstream->inited to indicate whether inflateInit()
 * succeeded.
 * @return Returns 0 on success or a negative error code on failure
 */
int ff_inflate_init(FFZStream *zstream, void *logctx);

/**
 * Wrapper around inflateEnd(). It calls inflateEnd() iff
 * zstream->inited is set and resets zstream->inited.
 * It is therefore safe to be called even if
 * ff_inflate_init() has never been called on it (or errored out)
 * provided that the FFZStream (or just FFZStream.inited) has been zeroed.
 */
void ff_inflate_end(FFZStream *zstream);

/**
 * Wrapper around deflateInit(). It works analogously to ff_inflate_init().
 */
int ff_deflate_init(FFZStream *zstream, int level, void *logctx);

/**
 * Wrapper around deflateEnd(). It works analogously to ff_inflate_end().
 */
void ff_deflate_end(FFZStream *zstream);



/* MT */
int ff_deflate_init_mt(FFZStreamMT *zstream, int level, int thread_count, void *logctx);
int ff_deflate_mt(FFZStreamMT *zstream, uint8_t *out_buf, size_t out_size, const uint8_t *in_buf, size_t in_size);
int ff_deflate_reset_mt(FFZStreamMT *zstream);
void ff_deflate_end_mt(FFZStreamMT *zstream);

#endif /* AVCODEC_ZLIB_WRAPPER_H */
