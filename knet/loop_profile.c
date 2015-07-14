/*
 * Copyright (c) 2014-2015, dennis wang
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL dennis wang BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "loop_profile.h"
#include "loop.h"
#include "list.h"
#include "stream.h"

struct _loop_profile_t {
    loop_t*  loop;
    uint64_t recv_bytes;
    uint64_t send_bytes;
    uint32_t established_channel;
    uint32_t active_channel;
    uint32_t close_channel;
    uint64_t last_send_bytes;
    uint64_t last_recv_bytes;
    time_t   last_send_tick;
    time_t   last_recv_tick;
};

loop_profile_t* loop_profile_create(loop_t* loop) {
    loop_profile_t* profile = 0;
    verify(loop);
    profile = create(loop_profile_t);
    verify(profile);
    memset(profile, 0, sizeof(loop_profile_t));
    profile->loop           = loop;
    profile->last_send_tick = time(0);
    profile->last_recv_tick = time(0);
    return profile;
}

void loop_profile_destroy(loop_profile_t* profile) {
    verify(profile);
    destroy(profile);
}

uint32_t loop_profile_increase_established_channel_count(loop_profile_t* profile) {
    verify(profile);
    return ++profile->established_channel;
}

uint32_t loop_profile_decrease_established_channel_count(loop_profile_t* profile) {
    verify(profile);
    return --profile->established_channel;
}

uint32_t loop_profile_get_established_channel_count(loop_profile_t* profile) {
    verify(profile);
    return profile->established_channel - 2; /* �¼�֪ͨ�ܵ�������ͳ������ */
}

uint32_t loop_profile_increase_active_channel_count(loop_profile_t* profile) {
    verify(profile);
    return ++profile->active_channel;
}

uint32_t loop_profile_decrease_active_channel_count(loop_profile_t* profile) {
    verify(profile);
    return --profile->active_channel;
}

uint32_t loop_profile_get_active_channel_count(loop_profile_t* profile) {
    verify(profile);
    return profile->active_channel;
}

uint32_t loop_profile_increase_close_channel_count(loop_profile_t* profile) {
    verify(profile);
    return ++profile->close_channel;
}

uint32_t loop_profile_decrease_close_channel_count(loop_profile_t* profile) {
    verify(profile);
    return --profile->close_channel;
}

uint32_t loop_profile_get_close_channel_count(loop_profile_t* profile) {
    verify(profile);
    return profile->close_channel;
}

uint64_t loop_profile_add_send_bytes(loop_profile_t* profile, uint64_t send_bytes) {
    verify(profile);
    return (profile->send_bytes += send_bytes);
}

uint64_t loop_profile_get_sent_bytes(loop_profile_t* profile) {
    verify(profile);
    return profile->send_bytes;
}

uint64_t loop_profile_add_recv_bytes(loop_profile_t* profile, uint64_t recv_bytes) {
    verify(profile);
    return (profile->recv_bytes += recv_bytes);
}

uint64_t loop_profile_get_recv_bytes(loop_profile_t* profile) {
    verify(profile);
    return profile->recv_bytes;
}

uint32_t loop_profile_get_sent_bindwidth(loop_profile_t* profile) {
    time_t   tick      = time(0);
    uint64_t bindwidth = 0;
    uint64_t intval    = 0;
    uint64_t bytes     = profile->send_bytes - profile->last_send_tick;
    verify(profile);
    if (tick == profile->last_send_tick) {
        intval = 1;
    } else {
        intval = tick - profile->last_send_tick;
    }
    bindwidth = bytes / intval;
    profile->last_send_tick  = tick;
    profile->last_send_bytes = profile->send_bytes;
    return (uint32_t)(bytes / intval);
}

uint32_t loop_profile_get_recv_bindwidth(loop_profile_t* profile) {
    time_t   tick      = time(0);
    uint64_t bindwidth = 0;
    uint64_t intval    = 0;
    uint64_t bytes     = profile->recv_bytes - profile->last_recv_tick;
    verify(profile);
    if (tick == profile->last_recv_tick) {
        intval = 1;
    } else {
        intval = tick - profile->last_recv_tick;
    }
    bindwidth = bytes / intval;
    profile->last_recv_tick  = tick;
    profile->last_recv_bytes = profile->recv_bytes;
    return (uint32_t)(bytes / intval);
}

int loop_profile_dump_file(loop_profile_t* profile, FILE* fp) {
    int len = 0;
    verify(profile);
    verify(fp);
    len = fprintf(
        fp,
        "Established channel: %ld\n"
        "Active channel:      %ld\n"
        "Close channel:       %ld\n"
        "Received bytes:      %lld\n"
        "Sent bytes:          %lld\n",
        (long)loop_profile_get_established_channel_count(profile),
        (long)loop_profile_get_active_channel_count(profile),
        (long)loop_profile_get_close_channel_count(profile),
        (long long)loop_profile_get_recv_bytes(profile),
        (long long)loop_profile_get_sent_bytes(profile));
    if (len <= 0) {
        return error_fail;
    }
    return error_ok;
}

int loop_profile_dump_stream(loop_profile_t* profile, stream_t* stream) {
    verify(profile);
    verify(stream);
    return stream_push_varg(
        stream,
        "Established channel: %ld\n"
        "Active channel:      %ld\n"
        "Close channel:       %ld\n"
        "Received bytes:      %lld\n"
        "Sent bytes:          %lld\n",
        (long)loop_profile_get_established_channel_count(profile),
        (long)loop_profile_get_active_channel_count(profile),
        (long)loop_profile_get_close_channel_count(profile),
        (long long)loop_profile_get_recv_bytes(profile),
        (long long)loop_profile_get_sent_bytes(profile));
}

int loop_profile_dump_stdout(loop_profile_t* profile) {
    int len = 0;
    verify(profile);
    len = fprintf(
        stdout,
        "Established channel: %ld\n"
        "Active channel:      %ld\n"
        "Close channel:       %ld\n"
        "Received bytes:      %lld\n"
        "Sent bytes:          %lld\n",
        (long)loop_profile_get_established_channel_count(profile),
        (long)loop_profile_get_active_channel_count(profile),
        (long)loop_profile_get_close_channel_count(profile),
        (long long)loop_profile_get_recv_bytes(profile),
        (long long)loop_profile_get_sent_bytes(profile));
    if (len <= 0) {
        return error_fail;
    }
    return error_ok;
}