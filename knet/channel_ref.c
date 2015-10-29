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
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "channel_ref.h"
#include "channel.h"
#include "loop.h"
#include "misc.h"
#include "stream.h"
#include "loop_balancer.h"
#include "buffer.h"
#include "ringbuffer.h"
#include "address.h"
#include "loop_profile.h"
#include "logger.h"


typedef struct _channel_ref_info_t {
    /* 基础数据成员 */
    int                           balance;              /* 是否被负载均衡标志 */
    kchannel_t*                   channel;              /* 内部管道 */
    kdlist_node_t*                loop_node;            /* 管道链表节点 */
    kstream_t*                    stream;               /* 管道(读/写)数据流 */
    kloop_t*                      loop;                 /* 管道所关联的kloop_t */
    kaddress_t*                   peer_address;         /* 对端地址 */
    kaddress_t*                   local_address;        /* 本地地址 */
    knet_channel_event_e          event;                /* 管道投递事件 */
    volatile knet_channel_state_e state;                /* 管道状态 */
    atomic_counter_t              ref_count;            /* 引用计数 */
    knet_channel_ref_cb_t         cb;                   /* 回调 */
    time_t                        last_recv_ts;         /* 最后一次读操作时间戳（秒） */
    time_t                        timeout;              /* 读空闲超时（秒） */
    time_t                        last_connect_timeout; /* 最后一次connect()超时（秒） */
    time_t                        connect_timeout;      /* connect()超时间隔（秒） */
    int                           auto_reconnect;       /* 自动重连标志 */
    int                           flag;                 /* 选取器所使用自定义标志位 */
    void*                         data;                 /* 选取器所使用自定义数据 */
    void*                         user_data;            /* 用户数据指针 - 内部使用 */
    void*                         user_ptr;             /* 暴露给外部使用的数据指针 - 外部使用 */
    /* 扩展数据成员 */
} channel_ref_info_t;

struct _channel_ref_t {
    int                 share;     /* 是否通过knet_channel_ref_share()创建 */
    uint64_t            domain_id; /* 域ID */
    kdlist_node_t*      list_node; /* 域链表节点 */
    channel_ref_info_t* ref_info;  /* 管道信息 */
};

kchannel_ref_t* knet_channel_ref_create(kloop_t* loop, kchannel_t* channel) {
    kchannel_ref_t* channel_ref = create(kchannel_ref_t);
    verify(channel_ref);
    memset(channel_ref, 0, sizeof(kchannel_ref_t));
    channel_ref->ref_info = create(channel_ref_info_t);
    verify(channel_ref->ref_info);
    memset(channel_ref->ref_info, 0, sizeof(channel_ref_info_t));
    channel_ref->ref_info->stream = stream_create(channel_ref);
    verify(channel_ref->ref_info->stream);
    channel_ref->ref_info->channel      = channel;
    channel_ref->ref_info->ref_count    = 0;
    channel_ref->ref_info->loop         = loop;
    channel_ref->ref_info->last_recv_ts = time(0);
    channel_ref->ref_info->state        = channel_state_init;
    knet_loop_profile_increase_active_channel_count(knet_loop_get_profile(loop));
    return channel_ref;
}

int knet_channel_ref_destroy(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    if (channel_ref->ref_info) {
        /* 检测引用计数 */
        if (!atomic_counter_zero(&channel_ref->ref_info->ref_count)) {
            return error_ref_nonzero;
        }
        if (channel_ref->ref_info->peer_address) {
            knet_address_destroy(channel_ref->ref_info->peer_address);
        }
        if (channel_ref->ref_info->local_address) {
            knet_address_destroy(channel_ref->ref_info->local_address);
        }
        /* 通知选取器删除管道相关资源 */
        if ((channel_ref->ref_info->state != channel_state_init) && /* 已经被加入到loop管道链表 */
            channel_ref->ref_info->loop) {
            knet_impl_remove_channel_ref(channel_ref->ref_info->loop, channel_ref);
        }
        if (channel_ref->ref_info->channel) {
            knet_channel_destroy(channel_ref->ref_info->channel);
        }
        if (channel_ref->ref_info->stream) {
            stream_destroy(channel_ref->ref_info->stream);
        }
        destroy(channel_ref->ref_info);
    }
    destroy(channel_ref);
    return error_ok;
}

int knet_channel_ref_connect(kchannel_ref_t* channel_ref, const char* ip, int port, int timeout) {
    int      error = error_ok;
    kloop_t* loop  = 0;
    verify(channel_ref);
    verify(port);
    if (!ip) {
        ip = "127.0.0.1";
    }
    if (knet_channel_ref_check_state(channel_ref, channel_state_connect)) {
        /* 已经处于连接状态 */
        return error_connect_in_progress;
    }
    if (!channel_ref->ref_info->peer_address) {
        channel_ref->ref_info->peer_address = knet_address_create();        
    }
    knet_address_set(channel_ref->ref_info->peer_address, ip, port);
    if (timeout > 0) {
        channel_ref->ref_info->connect_timeout = timeout;
        /* 设置超时时间戳 */
        channel_ref->ref_info->last_connect_timeout = time(0) + timeout;
    }
    /* 如果目标积极拒绝，返回失败 */
    error = knet_channel_connect(channel_ref->ref_info->channel, ip, port);
    if (error_ok != error) {
        return error;
    }
    log_info("start connect to IP[%s], port[%d]", ip, port);
    /* 负载均衡 */
    loop = knet_channel_ref_choose_loop(channel_ref);
    if (loop) {
        /* 减少原loop的active管道数量 */
        knet_loop_profile_decrease_active_channel_count(
            knet_loop_get_profile(channel_ref->ref_info->loop));
        /* 设置目标loop */
        channel_ref->ref_info->loop = loop;
        /* 增加目标loop的active管道数量 */
        knet_loop_profile_increase_active_channel_count(knet_loop_get_profile(loop));
        /* 添加到其他loop */
        knet_loop_notify_connect(loop, channel_ref);
        return error_ok;
    }
    /* 当前线程内发起连接 */
    return knet_channel_ref_connect_in_loop(channel_ref);
}

int knet_channel_ref_reconnect(kchannel_ref_t* channel_ref, int timeout) {
    int                   error               = error_ok;
    char                  ip[32]              = {0};
    int                   port                = 0;
    kchannel_ref_t*       new_channel         = 0;
    kaddress_t*           peer_address        = 0;
    time_t                connect_timeout     = 0;
    knet_channel_ref_cb_t cb                  = 0;
    kloop_t*              loop                = 0;
    uint32_t              max_send_list_len   = 0;
    uint32_t              max_recv_buffer_len = 0;
    int                   auto_reconnect      = 0;
    void*                 user_data           = 0;
    void*                 ptr                 = 0;
    verify(channel_ref);
    verify(channel_ref->ref_info->channel);
    if (!knet_channel_ref_check_state(channel_ref, channel_state_connect)) {
        /* 未处于正在连接状态的管道不能重连 */
        return error_channel_not_connect;
    }
    /* 获取原有管道属性 */
    loop                = knet_channel_ref_get_loop(channel_ref);
    max_send_list_len   = knet_channel_get_max_send_list_len(channel_ref->ref_info->channel);
    max_recv_buffer_len = knet_channel_get_max_recv_buffer_len(channel_ref->ref_info->channel);
    cb                  = knet_channel_ref_get_cb(channel_ref);
    user_data           = knet_channel_ref_get_user_data(channel_ref);
    ptr                 = knet_channel_ref_get_ptr(channel_ref);
    auto_reconnect      = knet_channel_ref_check_auto_reconnect(channel_ref);
    peer_address        = channel_ref->ref_info->peer_address;
    verify(peer_address);
    strcpy(ip, address_get_ip(peer_address));
    port = address_get_port(peer_address);
    /* 建立新管道 */
    new_channel = knet_loop_create_channel(loop, max_send_list_len, max_recv_buffer_len);
    verify(new_channel);
    if (timeout > 0) {
        /* 设置新的超时时间戳 */
        connect_timeout = timeout;
    } else {
        /* 使用原有的超时时间戳 */
        if (channel_ref->ref_info->connect_timeout) {
            connect_timeout = channel_ref->ref_info->connect_timeout;
        }
    }
    /* 设置原有回调 */
    knet_channel_ref_set_cb(new_channel, cb);
    /* 设置原有用户数据 */
    knet_channel_ref_set_user_data(new_channel, user_data);
    /* 设置用户指针 */
    knet_channel_ref_set_ptr(new_channel, ptr);
    /* 设置自动重连标志 */
    knet_channel_ref_set_auto_reconnect(new_channel, auto_reconnect);
    /* 启动新的连接器 */
    error = knet_channel_ref_connect(new_channel, ip, port, (int)connect_timeout);
    if (error_ok != error) {
        return error;
    }
    /* 销毁原有管道 */
    knet_channel_ref_close(channel_ref);
    return error;
}

void knet_channel_ref_set_auto_reconnect(kchannel_ref_t* channel_ref, int auto_reconnect) {
    verify(channel_ref);
    channel_ref->ref_info->auto_reconnect = auto_reconnect;
}

int knet_channel_ref_check_auto_reconnect(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return channel_ref->ref_info->auto_reconnect;
}

void knet_channel_ref_accept_async(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    knet_loop_add_channel_ref(channel_ref->ref_info->loop, channel_ref);
    knet_channel_ref_set_state(channel_ref, channel_state_accept);
    knet_channel_ref_set_event(channel_ref, channel_event_recv);
}

int knet_channel_ref_accept(kchannel_ref_t* channel_ref, const char* ip, int port, int backlog) {
    int error = 0;
    thread_id_t thread_id = 0;
    verify(channel_ref);
    verify(port);
    if (knet_channel_ref_check_state(channel_ref, channel_state_accept)) {
        /* 已经处于监听状态 */
        return error_accept_in_progress;
    }
    /* 监听 */
    error = knet_channel_accept(channel_ref->ref_info->channel, ip, port, backlog);
    if (error == error_ok) {
        thread_id = knet_loop_get_thread_id(channel_ref->ref_info->loop);
        if (thread_id) { /* kloop_t在某个线程运行过 */
            if (thread_id != thread_get_self_id()) { /* 跨线程启动监听器 */
                knet_loop_notify_accept_async(channel_ref->ref_info->loop, channel_ref);
                return error;
            }
        }
        /* 当前线程内 */
        knet_loop_add_channel_ref(channel_ref->ref_info->loop, channel_ref);
        knet_channel_ref_set_state(channel_ref, channel_state_accept);
        knet_channel_ref_set_event(channel_ref, channel_event_recv);
    }
    return error;
}

kchannel_ref_t* knet_channel_ref_share(kchannel_ref_t* channel_ref) {
    kchannel_ref_t* channel_ref_shared = 0;
    verify(channel_ref);
    channel_ref_shared = create(kchannel_ref_t);
    verify(channel_ref_shared);
    memset(channel_ref_shared, 0, sizeof(kchannel_ref_t));
    /* 增加管道引用计数 */
    atomic_counter_inc(&channel_ref->ref_info->ref_count);
    /* 共享管道信息指针 */
    channel_ref_shared->ref_info = channel_ref->ref_info;
    channel_ref_shared->share = 1;
    return channel_ref_shared;
}

void knet_channel_ref_leave(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    /* 递减引用计数 */
    atomic_counter_dec(&channel_ref->ref_info->ref_count);
    /* 管道信息最终由kloop_t销毁 */
    destroy(channel_ref);
}

void knet_channel_ref_update_close_in_loop(kloop_t* loop, kchannel_ref_t* channel_ref) {
    verify(loop);
    verify(channel_ref);
    if (knet_channel_ref_check_state(channel_ref, channel_state_close)) {
        return;
    }
    knet_channel_ref_set_state(channel_ref, channel_state_close);
    knet_channel_ref_clear_event(channel_ref, channel_event_recv | channel_event_send);
    /* 先调用回调，保证在内部还可以获得地址信息 */
    if (channel_ref->ref_info->cb) {
        channel_ref->ref_info->cb(channel_ref, channel_cb_event_close);
    }
    knet_channel_close(channel_ref->ref_info->channel);
    knet_loop_close_channel_ref(channel_ref->ref_info->loop, channel_ref);
}

void knet_channel_ref_close_check_reconnect(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    if (knet_channel_ref_check_auto_reconnect(channel_ref)) {
        /* 自动重连 */
        /* 伪造当前状态 */
        knet_channel_ref_set_state(channel_ref, channel_state_connect);
        /* 重连 */
        knet_channel_ref_reconnect(channel_ref, 0);
    } else {
        /* 关闭管道 */
        knet_channel_ref_close(channel_ref);
    }
}

void knet_channel_ref_close(kchannel_ref_t* channel_ref) {
    kloop_t*     loop = 0;
    verify(channel_ref);
    loop = channel_ref->ref_info->loop;
    if (!knet_loop_get_thread_id(loop) || (channel_ref->ref_info->state == channel_state_init)) {
        /* 未被加入到链表内 */
        knet_channel_ref_destroy(channel_ref);
        return;
    }
    if (knet_channel_ref_check_state(channel_ref, channel_state_close)) {
        /* 已经在关闭链表内 */
        return;
    }
    if (knet_loop_get_thread_id(loop) != thread_get_self_id()) {
        /* 通知管道所属线程 */
        log_info("close channel cross thread, notify thread[id:%ld]", knet_loop_get_thread_id(loop));
        knet_loop_notify_close(loop, channel_ref);
    } else {
        /* 本线程内关闭 */
        log_info("close channel[%llu] in loop thread[id: %ld]", knet_channel_ref_get_uuid(channel_ref), knet_loop_get_thread_id(loop));
        knet_channel_ref_update_close_in_loop(loop, channel_ref);
    }
}

int knet_channel_ref_check_close(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return knet_channel_ref_check_state(channel_ref, channel_state_close);
}

void knet_channel_ref_update_send_in_loop(kloop_t* loop, kchannel_ref_t* channel_ref, kbuffer_t* send_buffer) {
    int error = 0;
    verify(loop);
    verify(channel_ref);
    verify(send_buffer);
    knet_loop_profile_add_send_bytes(knet_loop_get_profile(loop), knet_buffer_get_length(send_buffer));
    error = knet_channel_send_buffer(channel_ref->ref_info->channel, send_buffer);
    switch (error) {
    case error_send_patial:
        knet_channel_ref_set_event(channel_ref, channel_event_send);
        break;
    case error_send_fail:
        knet_channel_ref_close_check_reconnect(channel_ref);
        break;
    default:
        break;
    }
}

int knet_channel_ref_write(kchannel_ref_t* channel_ref, const char* data, int size) {
    kloop_t*   loop        = 0;
    kbuffer_t* send_buffer = 0;
    int       error       = error_ok;
    verify(channel_ref);
    verify(data);
    verify(size);
    if (!knet_channel_ref_check_state(channel_ref, channel_state_active)) {
        return error_not_connected;
    }
    loop = channel_ref->ref_info->loop;
    if (knet_loop_get_thread_id(loop) != thread_get_self_id()) {
        /* 转到loop所在线程发送 */
        log_info("send cross thread, notify thread[id:%ld]", knet_loop_get_thread_id(loop));
        send_buffer = knet_buffer_create(size);
        verify(send_buffer);
        if (!send_buffer) {
            return error_no_memory;
        }
        knet_buffer_put(send_buffer, data, size);
        knet_loop_notify_send(loop, channel_ref, send_buffer);
    } else {
        knet_loop_profile_add_send_bytes(knet_loop_get_profile(channel_ref->ref_info->loop), size);
        /* 当前线程发送 */
        error = knet_channel_send(channel_ref->ref_info->channel, data, size);
        switch (error) {
        case error_send_patial:
            knet_channel_ref_set_event(channel_ref, channel_event_send);
            /* 对于调用者不是错误 */
            error = error_ok;
            break;
        case error_send_fail:
            knet_channel_ref_close_check_reconnect(channel_ref);
            break;
        default:
            break;
        }
    }
    return error;
}

socket_t knet_channel_ref_get_socket_fd(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return knet_channel_get_socket_fd(channel_ref->ref_info->channel);
}

kstream_t* knet_channel_ref_get_stream(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return channel_ref->ref_info->stream;
}

kloop_t* knet_channel_ref_get_loop(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return channel_ref->ref_info->loop;
}

void knet_channel_ref_set_loop_node(kchannel_ref_t* channel_ref, kdlist_node_t* node) {
    verify(channel_ref); /* node可以为0 */
    channel_ref->ref_info->loop_node = node;
}

kdlist_node_t* knet_channel_ref_get_loop_node(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return channel_ref->ref_info->loop_node;
}

void knet_channel_ref_set_event(kchannel_ref_t* channel_ref, knet_channel_event_e e) {
    verify(channel_ref);
    knet_impl_event_add(channel_ref, e);
    channel_ref->ref_info->event |= e;
}

knet_channel_event_e knet_channel_ref_get_event(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return channel_ref->ref_info->event;
}

void knet_channel_ref_clear_event(kchannel_ref_t* channel_ref, knet_channel_event_e e) {
    verify(channel_ref);
    knet_impl_event_remove(channel_ref, e);
    channel_ref->ref_info->event &= ~e;
}

void knet_channel_ref_set_state(kchannel_ref_t* channel_ref, knet_channel_state_e state) {
    verify(channel_ref);
    channel_ref->ref_info->state = state;
}

int knet_channel_ref_check_state(kchannel_ref_t* channel_ref, knet_channel_state_e state) {
    verify(channel_ref);
    return (channel_ref->ref_info->state == state);
}

int knet_channel_ref_check_event(kchannel_ref_t* channel_ref, knet_channel_event_e event) {
    verify(channel_ref);
    return (channel_ref->ref_info->event & event);
}

kchannel_ref_t* knet_channel_ref_accept_from_socket_fd(kchannel_ref_t* channel_ref, kloop_t* loop, socket_t client_fd, int event) {
    kchannel_t*     acceptor_channel    = 0;
    uint32_t        max_send_list_len   = 0;
    uint32_t        max_ringbuffer_size = 0;
    kchannel_t*     client_channel      = 0;
    kchannel_ref_t* client_ref          = 0;
    verify(channel_ref);
    verify(channel_ref->ref_info);
    verify(client_fd > 0);
    acceptor_channel = channel_ref->ref_info->channel;
    verify(acceptor_channel);
    max_send_list_len = knet_channel_get_max_send_list_len(acceptor_channel);
    if (!max_send_list_len) {
        max_send_list_len = INT_MAX;
    }
    max_ringbuffer_size = ringbuffer_get_max_size(knet_channel_get_ringbuffer(acceptor_channel));
    if (!max_ringbuffer_size) {
        max_ringbuffer_size = 16 * 1024; /* 默认16K */
    }
    client_channel = knet_channel_create_exist_socket_fd(client_fd, max_send_list_len, max_ringbuffer_size);
    verify(client_channel);
    client_ref = knet_channel_ref_create(loop, client_channel);
    verify(client_ref);
    if (event) {
        /* 添加到当前线程loop */
        knet_loop_add_channel_ref(channel_ref->ref_info->loop, client_ref);
        /* 创建的同时设置事件和状态 */
        knet_channel_ref_set_state(client_ref, channel_state_active);
        knet_channel_ref_set_event(client_ref, channel_event_recv);
    }
    return client_ref;
}

void knet_channel_ref_update_accept(kchannel_ref_t* channel_ref) {
    kchannel_ref_t* client_ref = 0;
    kloop_t*        loop       = 0;
    socket_t       client_fd  = 0;
    verify(channel_ref);
    /* 查看选取器是否有自定义实现 */
    client_fd = knet_impl_channel_accept(channel_ref);
    if (!client_fd) {
        /* 默认实现 */
        client_fd = socket_accept(knet_channel_get_socket_fd(channel_ref->ref_info->channel));
    }
    verify(client_fd > 0);
    knet_channel_ref_set_state(channel_ref, channel_state_accept);
    knet_channel_ref_set_event(channel_ref, channel_event_recv);
    if (client_fd) {
        loop = knet_channel_ref_choose_loop(channel_ref);
        if (loop) {
            client_ref = knet_channel_ref_accept_from_socket_fd(channel_ref, loop, client_fd, 0);
            verify(client_ref);
            knet_channel_ref_set_user_data(client_ref, channel_ref->ref_info->user_data);
            knet_channel_ref_set_ptr(client_ref, channel_ref->ref_info->user_ptr);
            /* 设置回调 */
            knet_channel_ref_set_cb(client_ref, channel_ref->ref_info->cb);
            /* 添加到其他loop */
            knet_loop_notify_accept(loop, client_ref);
        } else {
            client_ref = knet_channel_ref_accept_from_socket_fd(channel_ref, channel_ref->ref_info->loop, client_fd, 1);
            verify(client_ref);
            knet_channel_ref_set_user_data(client_ref, channel_ref->ref_info->user_data);
            knet_channel_ref_set_ptr(client_ref, channel_ref->ref_info->user_ptr);
            /* 调用回调 */
            if (channel_ref->ref_info->cb) {
                channel_ref->ref_info->cb(client_ref, channel_cb_event_accept);
            }
        }
    }
}

void knet_channel_ref_update_accept_in_loop(kloop_t* loop, kchannel_ref_t* channel_ref) {
    verify(loop);
    verify(channel_ref);
    /* 添加到当前线程loop */
    knet_loop_add_channel_ref(loop, channel_ref);
    knet_channel_ref_set_state(channel_ref, channel_state_active);
    knet_channel_ref_set_event(channel_ref, channel_event_recv);
    /* 调用回调 */
    if (channel_ref->ref_info->cb) {
        channel_ref->ref_info->cb(channel_ref, channel_cb_event_accept);
    }
}

void knet_channel_ref_update_connect(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    knet_channel_ref_set_event(channel_ref, channel_event_recv);
    knet_channel_ref_set_state(channel_ref, channel_state_active);
    /* 调用回调 */
    if (channel_ref->ref_info->cb) {
        log_verb("connected, invoke cb");
        channel_ref->ref_info->cb(channel_ref, channel_cb_event_connect);
    }
}

void knet_channel_ref_update_recv(kchannel_ref_t* channel_ref) {
    int error = 0;
    uint32_t bytes = 0;
    verify(channel_ref);
    bytes = knet_stream_available(channel_ref->ref_info->stream);
    error = knet_channel_update_recv(channel_ref->ref_info->channel);
    switch (error) {
        case error_recv_fail:
            knet_channel_ref_close_check_reconnect(channel_ref);
            break;
        case error_recv_buffer_full:
            knet_channel_ref_close_check_reconnect(channel_ref);
            break;
        default:
            break;
    }
    if (error == error_ok) {
        knet_loop_profile_add_recv_bytes(knet_loop_get_profile(channel_ref->ref_info->loop),
            knet_stream_available(channel_ref->ref_info->stream) - bytes);
        if (channel_ref->ref_info->cb) {
            channel_ref->ref_info->cb(channel_ref, channel_cb_event_recv);
        }
        knet_channel_ref_set_event(channel_ref, channel_event_recv);
    }
}

void knet_channel_ref_update_send(kchannel_ref_t* channel_ref) {
    int error = 0;
    verify(channel_ref);
    error = knet_channel_update_send(channel_ref->ref_info->channel);
    switch (error) {
        case error_send_fail:
            knet_channel_ref_close_check_reconnect(channel_ref);
            break;
        case error_send_patial:
            knet_channel_ref_set_event(channel_ref, channel_event_send);
            break;
        default:
            break;
    }
    if (error == error_ok) {
        if (channel_ref->ref_info->cb) {
            channel_ref->ref_info->cb(channel_ref, channel_cb_event_send);
        }
    }
}

void knet_channel_ref_update(kchannel_ref_t* channel_ref, knet_channel_event_e e, time_t ts) {
    verify(channel_ref);
    if (knet_channel_ref_check_state(channel_ref, channel_state_close)) {
        return;
    }
    if ((e & channel_event_recv) && knet_channel_ref_check_event(channel_ref, channel_event_recv)) {
        if (knet_channel_ref_check_state(channel_ref, channel_state_accept)) {
            /* 新连接 */
            knet_channel_ref_update_accept(channel_ref);
        } else {
            /* 最后一次读取到数据的时间戳（秒） */
            channel_ref->ref_info->last_recv_ts = ts;
            /* 读 */
            knet_channel_ref_update_recv(channel_ref);
        }
    } 
    if ((e & channel_event_send) && knet_channel_ref_check_event(channel_ref, channel_event_send)) {
        if (knet_channel_ref_check_state(channel_ref, channel_state_connect)) {
            /* 连接完成 */
            knet_channel_ref_update_connect(channel_ref);
        } else {
            /* 写 */
            knet_channel_ref_update_send(channel_ref);
        }
    }
}

kringbuffer_t* knet_channel_ref_get_ringbuffer(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return knet_channel_get_ringbuffer(channel_ref->ref_info->channel);
}

kloop_t* knet_channel_ref_choose_loop(kchannel_ref_t* channel_ref) {
    kloop_t*          loop         = 0;
    kloop_t*          current_loop = 0;
    kloop_balancer_t* balancer     = 0;
    verify(channel_ref);
    current_loop = channel_ref->ref_info->loop;
    balancer = knet_loop_get_balancer(current_loop);
    if (!balancer) {
        return 0;
    }
    /* 检查是否开启了loop_balancer_out配置 */
    if (knet_loop_check_balance_options(channel_ref->ref_info->loop, loop_balancer_out)) {
        loop = knet_loop_balancer_choose(balancer);
        if (loop == channel_ref->ref_info->loop) {
            return 0;
        }
    }
    return loop;
}

void knet_channel_ref_set_flag(kchannel_ref_t* channel_ref, int flag) {
    verify(channel_ref);
    channel_ref->ref_info->flag = flag;
}

int knet_channel_ref_get_flag(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return channel_ref->ref_info->flag;
}

void knet_channel_ref_set_data(kchannel_ref_t* channel_ref, void* data) {
    verify(channel_ref);
    channel_ref->ref_info->data = data;
}

void* knet_channel_ref_get_data(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return channel_ref->ref_info->data;
}

void knet_channel_ref_set_loop(kchannel_ref_t* channel_ref, kloop_t* loop) {
    channel_ref->ref_info->loop = loop;
}

int knet_channel_ref_check_balance(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return channel_ref->ref_info->balance;
}

void knet_channel_ref_set_timeout(kchannel_ref_t* channel_ref, int timeout) {
    verify(channel_ref); /* timeout可以为0 */
    channel_ref->ref_info->timeout = (time_t)timeout;
}

int knet_channel_ref_check_connect_timeout(kchannel_ref_t* channel_ref, time_t ts) {
    int ret = 0;
    verify(channel_ref);
    if (knet_channel_ref_check_state(channel_ref, channel_state_connect)) {
        if (channel_ref->ref_info->last_connect_timeout) {
            ret = (channel_ref->ref_info->last_connect_timeout < ts);
        }
    }
    if (ret) {
        /* 如果超时重置下次触发时间 */
        channel_ref->ref_info->last_connect_timeout =
            ts + channel_ref->ref_info->connect_timeout;
    }
    return ret;
}

int knet_channel_ref_check_timeout(kchannel_ref_t* channel_ref, time_t ts) {
    verify(channel_ref);
    if (!channel_ref->ref_info->timeout) {
        return 0;
    }
    if ((ts - channel_ref->ref_info->last_recv_ts) > channel_ref->ref_info->timeout) {
        channel_ref->ref_info->last_recv_ts = ts;
        return 1;
    }
    return 0;
}

void knet_channel_ref_set_cb(kchannel_ref_t* channel_ref, knet_channel_ref_cb_t cb) {
    verify(channel_ref);
    channel_ref->ref_info->cb = cb;
}

knet_channel_ref_cb_t knet_channel_ref_get_cb(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return channel_ref->ref_info->cb;
}

int knet_channel_ref_connect_in_loop(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    knet_loop_add_channel_ref(channel_ref->ref_info->loop, channel_ref);
    knet_channel_ref_set_state(channel_ref, channel_state_connect);
    knet_channel_ref_set_event(channel_ref, channel_event_send);
    return error_ok;
}

kaddress_t* knet_channel_ref_get_peer_address(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    if (channel_ref->ref_info->peer_address) {
        return channel_ref->ref_info->peer_address;
    }
    channel_ref->ref_info->peer_address = knet_address_create();
    socket_getpeername(channel_ref, channel_ref->ref_info->peer_address);
    return channel_ref->ref_info->peer_address;
}

kaddress_t* knet_channel_ref_get_local_address(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    if (channel_ref->ref_info->local_address) {
        return channel_ref->ref_info->local_address;
    }
    channel_ref->ref_info->local_address = knet_address_create();
    socket_getsockname(channel_ref, channel_ref->ref_info->local_address);
    return channel_ref->ref_info->local_address;
}

void knet_channel_ref_set_domain_node(kchannel_ref_t* channel_ref, kdlist_node_t* node) {
    verify(channel_ref);
    channel_ref->list_node = node;
}

kdlist_node_t* knet_channel_ref_get_domain_node(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return channel_ref->list_node;
}

int knet_channel_ref_check_share(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return channel_ref->share;
}

void knet_channel_ref_set_domain_id(kchannel_ref_t* channel_ref, uint64_t domain_id) {
    verify(channel_ref);
    channel_ref->domain_id = domain_id;
}

uint64_t knet_channel_ref_get_domain_id(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return channel_ref->domain_id;
}

int knet_channel_ref_incref(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return (int)atomic_counter_inc(&channel_ref->ref_info->ref_count);
}

int knet_channel_ref_decref(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return (int)atomic_counter_dec(&channel_ref->ref_info->ref_count);
}

int knet_channel_ref_check_ref_zero(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return !atomic_counter_zero(&channel_ref->ref_info->ref_count);
}

int knet_channel_ref_get_ref(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return channel_ref->ref_info->ref_count;
}

uint64_t knet_channel_ref_get_uuid(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return knet_channel_get_uuid(channel_ref->ref_info->channel);
}

int knet_channel_ref_equal(kchannel_ref_t* a, kchannel_ref_t* b) {
    return ((knet_channel_ref_get_uuid(a) == knet_channel_ref_get_uuid(b)) &&
        (a->ref_info == b->ref_info));
}

void knet_channel_ref_set_user_data(kchannel_ref_t* channel_ref, void* data) {
    verify(channel_ref);
    channel_ref->ref_info->user_data = data;
}

void* knet_channel_ref_get_user_data(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return channel_ref->ref_info->user_data;
}

void knet_channel_ref_set_ptr(kchannel_ref_t* channel_ref, void* ptr) {
    verify(channel_ref);
    channel_ref->ref_info->user_ptr = ptr;
}

void* knet_channel_ref_get_ptr(kchannel_ref_t* channel_ref) {
    verify(channel_ref);
    return channel_ref->ref_info->user_ptr;
}
