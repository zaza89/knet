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

#ifndef FRAMEWORK_WORKER_H
#define FRAMEWORK_WORKER_H

#include "config.h"

/**
 * ������ܹ����߳�
 * @param f framework_tʵ��
 * @return framework_worker_tʵ��
 */
framework_worker_t* framework_worker_create(framework_t* f);

/**
 * ���ٿ�ܹ����߳�
 * @param w framework_worker_tʵ��
 */
void framework_worker_destroy(framework_worker_t* w);

/**
 * ������ܹ����߳�
 * @param w framework_worker_tʵ��
 * @retval error_ok �ɹ�
 * @retval ���� ʧ��
 */
int framework_worker_start(framework_worker_t* w);

/**
 * �رտ�ܹ����߳�
 * @param w framework_worker_tʵ��
 */
void framework_worker_stop(framework_worker_t* w);

/**
 * �ȴ���ܹ����̹߳ر�
 * @param w framework_worker_tʵ��
 */
void framework_worker_wait_for_stop(framework_worker_t* w);

#endif /* FRAMEWORK_WORKER_H */