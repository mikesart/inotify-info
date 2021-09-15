/*
*
* BSD 2-Clause License
*
* Copyright (c) 2018, Taymindis Woon
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* * Redistributions of source code must retain the above copyright notice, this
*   list of conditions and the following disclaimer.
*
* * Redistributions in binary form must reproduce the above copyright notice,
*   this list of conditions and the following disclaimer in the documentation
*   and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/

#ifndef LFQUEUE_H
#define LFQUEUE_H

#include <stdlib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lfqueue_cas_node_s lfqueue_cas_node_t;
typedef void* (*lfqueue_malloc_fn)(void*, size_t);
typedef void (*lfqueue_free_fn)(void*, void*);

#if defined __GNUC__ || defined __CYGWIN__ || defined __MINGW32__ || defined __APPLE__
#define lfq_bool_t int
#else
#ifdef _WIN64
#define lfq_bool_t int64_t
#else
#define lfq_bool_t int
#endif
#endif

typedef struct {
	lfqueue_cas_node_t *head, *tail, *root_free, *move_free;
	volatile size_t size;
	volatile lfq_bool_t in_free_mode;
	lfqueue_malloc_fn _malloc;
	lfqueue_free_fn _free;
	void *pl;
} lfqueue_t;

extern int   lfqueue_init(lfqueue_t *lfqueue);
extern int   lfqueue_init_mf(lfqueue_t *lfqueue, void* pl, lfqueue_malloc_fn lfqueue_malloc, lfqueue_free_fn lfqueue_free);
extern int   lfqueue_enq(lfqueue_t *lfqueue, void *value);
extern void* lfqueue_deq(lfqueue_t *lfqueue);
extern void* lfqueue_single_deq(lfqueue_t *lfqueue);

/** loop until value been dequeue, it sleeps 1ms if not found to reduce cpu high usage **/
extern void* lfqueue_deq_must(lfqueue_t *lfqueue);
extern void* lfqueue_single_deq_must(lfqueue_t *lfqueue);

extern void lfqueue_destroy(lfqueue_t *lfqueue);
extern size_t lfqueue_size(lfqueue_t *lfqueue);
extern void lfqueue_sleep(unsigned int milisec);


#ifdef __cplusplus
}
#endif

#endif

