/*
 * Copyright 2024 Rémi Bernon for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"
#include "unix_private.h"

#include "wine/debug.h"

#ifdef HAVE_FFMPEG

WINE_DEFAULT_DEBUG_CHANNEL(dmo);

static inline const char *debugstr_averr( int err )
{
    return wine_dbg_sprintf( "%d (%s)", err, av_err2str(err) );
}

static AVFormatContext *get_demuxer( struct winedmo_demuxer demuxer )
{
    return (AVFormatContext *)(UINT_PTR)demuxer.handle;
}

static INT64 get_user_time( INT64 time, AVRational time_base )
{
    static const AVRational USER_TIME_BASE_Q = {1, 10000000};
    return av_rescale_q_rnd( time, time_base, USER_TIME_BASE_Q, AV_ROUND_PASS_MINMAX );
}

static INT64 get_stream_time( const AVStream *stream, INT64 time )
{
    if (stream->time_base.num && stream->time_base.den) return get_user_time( time, stream->time_base );
    return get_user_time( time, AV_TIME_BASE_Q );
}

static INT64 get_context_duration( const AVFormatContext *ctx )
{
    INT64 i, max_duration = AV_NOPTS_VALUE;

    for (i = 0; i < ctx->nb_streams; i++)
    {
        const AVStream *stream = ctx->streams[i];
        INT64 duration = get_stream_time( stream, stream->duration );
        if (duration == AV_NOPTS_VALUE) continue;
        if (duration >= max_duration) max_duration = duration;
        if (max_duration == AV_NOPTS_VALUE) max_duration = duration;
    }

    return max_duration;
}

NTSTATUS demuxer_check( void *arg )
{
    struct demuxer_check_params *params = arg;
    const AVInputFormat *format = NULL;

    if (!strcmp( params->mime_type, "video/mp4" )) format = av_find_input_format( "mp4" );
    else if (!strcmp( params->mime_type, "video/avi" )) format = av_find_input_format( "avi" );
    else if (!strcmp( params->mime_type, "audio/wav" )) format = av_find_input_format( "wav" );
    else if (!strcmp( params->mime_type, "audio/x-ms-wma" )) format = av_find_input_format( "asf" );
    else if (!strcmp( params->mime_type, "video/x-ms-wmv" )) format = av_find_input_format( "asf" );
    else if (!strcmp( params->mime_type, "video/x-ms-asf" )) format = av_find_input_format( "asf" );
    else if (!strcmp( params->mime_type, "video/mpeg" )) format = av_find_input_format( "mpeg" );
    else if (!strcmp( params->mime_type, "audio/mp3" )) format = av_find_input_format( "mp3" );

    if (format) TRACE( "Found format %s (%s)\n", format->name, format->long_name );
    else FIXME( "Unsupported MIME type %s\n", debugstr_a(params->mime_type) );

    return format ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

NTSTATUS demuxer_create( void *arg )
{
    struct demuxer_create_params *params = arg;
    const char *ext = params->url ? strrchr( params->url, '.' ) : "";
    AVFormatContext *ctx;
    int ret;

    TRACE( "context %p, url %s, mime %s\n", params->context, debugstr_a(params->url), debugstr_a(params->mime_type) );

    if (!(ctx = avformat_alloc_context())) return STATUS_NO_MEMORY;
    if (!(ctx->pb = avio_alloc_context( NULL, 0, 0, params->context, unix_read_callback, NULL, unix_seek_callback )))
    {
        avformat_free_context( ctx );
        return STATUS_NO_MEMORY;
    }

    if ((ret = avformat_open_input( &ctx, NULL, NULL, NULL )) < 0)
    {
        ERR( "Failed to open input, error %s.\n", debugstr_averr(ret) );
        avio_context_free( &ctx->pb );
        avformat_free_context( ctx );
        return STATUS_UNSUCCESSFUL;
    }

    if ((params->duration = get_context_duration( ctx )) == AV_NOPTS_VALUE)
    {
        if ((ret = avformat_find_stream_info( ctx, NULL )) < 0)
        {
            ERR( "Failed to find stream info, ret %d (%s).\n", ret, av_err2str(ret) );
            avio_context_free( &ctx->pb );
            avformat_free_context( ctx );
            return STATUS_UNSUCCESSFUL;
        }
        params->duration = get_context_duration( ctx );
    }

    params->demuxer.handle = (UINT_PTR)ctx;
    params->stream_count = ctx->nb_streams;
    if (strstr( ctx->iformat->name, "mp4" )) strcpy( params->mime_type, "video/mp4" );
    else if (strstr( ctx->iformat->name, "avi" )) strcpy( params->mime_type, "video/avi" );
    else if (strstr( ctx->iformat->name, "mpeg" )) strcpy( params->mime_type, "video/mpeg" );
    else if (strstr( ctx->iformat->name, "mp3" )) strcpy( params->mime_type, "audio/mp3" );
    else if (strstr( ctx->iformat->name, "wav" )) strcpy( params->mime_type, "audio/wav" );
    else if (strstr( ctx->iformat->name, "asf" ))
    {
        if (!strcmp( ext, ".wma" )) strcpy( params->mime_type, "audio/x-ms-wma" );
        else if (!strcmp( ext, ".wmv" )) strcpy( params->mime_type, "video/x-ms-wmv" );
        else strcpy( params->mime_type, "video/x-ms-asf" );
    }
    else
    {
        FIXME( "Unknown MIME type for format %s, url %s\n", debugstr_a(ctx->iformat->name), debugstr_a(params->url) );
        strcpy( params->mime_type, "video/x-application" );
    }

    return STATUS_SUCCESS;
}

NTSTATUS demuxer_destroy( void *arg )
{
    struct demuxer_destroy_params *params = arg;
    AVFormatContext *ctx = get_demuxer( params->demuxer );

    TRACE( "context %p\n", ctx );

    params->context = ctx->pb->opaque;
    avio_context_free( &ctx->pb );
    avformat_free_context( ctx );

    return STATUS_SUCCESS;
}

NTSTATUS demuxer_stream_type( void *arg )
{
    struct demuxer_stream_type_params *params = arg;
    AVFormatContext *ctx = get_demuxer( params->demuxer );
    AVStream *stream = ctx->streams[params->stream];

    TRACE( "context %p, stream %u, stream %p, index %u\n", ctx, params->stream, stream, stream->index );

    return media_type_from_codec_params( stream->codecpar, &stream->sample_aspect_ratio,
                                         &stream->avg_frame_rate, 0, &params->media_type );
}

#endif /* HAVE_FFMPEG */
