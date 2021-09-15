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
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#if defined __GNUC__ || defined __CYGWIN__ || defined __MINGW32__ || defined __APPLE__

#include <sys/time.h>
#include <unistd.h> // for usleep
#include <sched.h>

#define __LFQ_VAL_COMPARE_AND_SWAP __sync_val_compare_and_swap
#define __LFQ_BOOL_COMPARE_AND_SWAP __sync_bool_compare_and_swap
#define __LFQ_FETCH_AND_ADD __sync_fetch_and_add
#define __LFQ_ADD_AND_FETCH __sync_add_and_fetch
#define __LFQ_YIELD_THREAD sched_yield
#define __LFQ_SYNC_MEMORY __sync_synchronize

#else

#include <Windows.h>
#include <time.h>
#ifdef _WIN64
inline BOOL __SYNC_BOOL_CAS(LONG64 volatile *dest, LONG64 input, LONG64 comparand) {
	return InterlockedCompareExchangeNoFence64(dest, input, comparand) == comparand;
}
#define __LFQ_VAL_COMPARE_AND_SWAP(dest, comparand, input) \
    InterlockedCompareExchangeNoFence64((LONG64 volatile *)dest, (LONG64)input, (LONG64)comparand)
#define __LFQ_BOOL_COMPARE_AND_SWAP(dest, comparand, input) \
    __SYNC_BOOL_CAS((LONG64 volatile *)dest, (LONG64)input, (LONG64)comparand)
#define __LFQ_FETCH_AND_ADD InterlockedExchangeAddNoFence64
#define __LFQ_ADD_AND_FETCH InterlockedAddNoFence64
#define __LFQ_SYNC_MEMORY MemoryBarrier

#else
#ifndef asm
#define asm __asm
#endif
inline BOOL __SYNC_BOOL_CAS(LONG volatile *dest, LONG input, LONG comparand) {
	return InterlockedCompareExchangeNoFence(dest, input, comparand) == comparand;
}
#define __LFQ_VAL_COMPARE_AND_SWAP(dest, comparand, input) \
    InterlockedCompareExchangeNoFence((LONG volatile *)dest, (LONG)input, (LONG)comparand)
#define __LFQ_BOOL_COMPARE_AND_SWAP(dest, comparand, input) \
    __SYNC_BOOL_CAS((LONG volatile *)dest, (LONG)input, (LONG)comparand)
#define __LFQ_FETCH_AND_ADD InterlockedExchangeAddNoFence
#define __LFQ_ADD_AND_FETCH InterlockedAddNoFence
#define __LFQ_SYNC_MEMORY() asm mfence

#endif
#include <windows.h>
#define __LFQ_YIELD_THREAD SwitchToThread
#endif

#include "lfqueue.h"
#define DEF_LFQ_ASSIGNED_SPIN 2048

#if defined __GNUC__ || defined __CYGWIN__ || defined __MINGW32__ || defined __APPLE__
#define lfq_time_t long
#define lfq_get_curr_time(_time_sec) \
struct timeval _time_; \
gettimeofday(&_time_, NULL); \
*_time_sec = _time_.tv_sec
#define lfq_diff_time(_etime_, _stime_) _etime_ - _stime_
#else
#define lfq_time_t time_t
#define lfq_get_curr_time(_time_sec) time(_time_sec)
#define lfq_diff_time(_etime_, _stime_) difftime(_etime_, _stime_)
#endif

struct lfqueue_cas_node_s {
	void * value;
	struct lfqueue_cas_node_s *next, *nextfree;
	lfq_time_t _deactivate_tm;
};

//static lfqueue_cas_node_t* __lfq_assigned(lfqueue_t *);
static void __lfq_recycle_free(lfqueue_t *, lfqueue_cas_node_t*);
static void __lfq_check_free(lfqueue_t *);
static void *_dequeue(lfqueue_t *);
static void *_single_dequeue(lfqueue_t *);
static int _enqueue(lfqueue_t *, void* );
static inline void* _lfqueue_malloc(void* pl, size_t sz) {
	return malloc(sz);
}
static inline void _lfqueue_free(void* pl, void* ptr) {
	free(ptr);
}

static void *
_dequeue(lfqueue_t *lfqueue) {
	lfqueue_cas_node_t *head, *next;
	void *val;

	for (;;) {
		head = lfqueue->head;
		if (__LFQ_BOOL_COMPARE_AND_SWAP(&lfqueue->head, head, head)) {
			next = head->next;
			if (__LFQ_BOOL_COMPARE_AND_SWAP(&lfqueue->tail, head, head)) {
				if (next == NULL) {
					val = NULL;
					goto _done;
				}
			}
			else {
				if (next) {
					val = next->value;
					if (__LFQ_BOOL_COMPARE_AND_SWAP(&lfqueue->head, head, next)) {
						break;
					}
				} else {
					val = NULL;
					goto _done;
				}
			}
		}
	}

	__lfq_recycle_free(lfqueue, head);
_done:
	// __asm volatile("" ::: "memory");
	__LFQ_SYNC_MEMORY();
	__lfq_check_free(lfqueue);
	return val;
}

static void *
_single_dequeue(lfqueue_t *lfqueue) {
	lfqueue_cas_node_t *head, *next;
	void *val;

	for (;;) {
		head = lfqueue->head;
		if (__LFQ_BOOL_COMPARE_AND_SWAP(&lfqueue->head, head, head)) {
			next = head->next;
			if (__LFQ_BOOL_COMPARE_AND_SWAP(&lfqueue->tail, head, head)) {
				if (next == NULL) {
					return NULL;
				}
			}
			else {
				if (next) {
					val = next->value;
					if (__LFQ_BOOL_COMPARE_AND_SWAP(&lfqueue->head, head, next)) {
						lfqueue->_free(lfqueue->pl, head);
						break;
					}
				} else {
					return NULL;
				}
			}
		}
	}
	return val;
}

static int
_enqueue(lfqueue_t *lfqueue, void* value) {
	lfqueue_cas_node_t *tail, *node;
	node = (lfqueue_cas_node_t*) lfqueue->_malloc(lfqueue->pl, sizeof(lfqueue_cas_node_t));
	if (node == NULL) {
		perror("malloc");
		return errno;
	}
	node->value = value;
	node->next = NULL;
	node->nextfree = NULL;
	for (;;) {
		__LFQ_SYNC_MEMORY();
		tail = lfqueue->tail;
		if (__LFQ_BOOL_COMPARE_AND_SWAP(&tail->next, NULL, node)) {
			// compulsory swap as tail->next is no NULL anymore, it has fenced on other thread
			__LFQ_BOOL_COMPARE_AND_SWAP(&lfqueue->tail, tail, node);
			__lfq_check_free(lfqueue);
			return 0;
		}
	}

	/*It never be here*/
	return -1;
}

static void
__lfq_recycle_free(lfqueue_t *lfqueue, lfqueue_cas_node_t* freenode) {
	lfqueue_cas_node_t *freed;
	do {
		freed = lfqueue->move_free;
	} while (!__LFQ_BOOL_COMPARE_AND_SWAP(&freed->nextfree, NULL, freenode) );

	lfq_get_curr_time(&freenode->_deactivate_tm);

	__LFQ_BOOL_COMPARE_AND_SWAP(&lfqueue->move_free, freed, freenode);
}

static void
__lfq_check_free(lfqueue_t *lfqueue) {
	lfq_time_t curr_time;
	if (__LFQ_BOOL_COMPARE_AND_SWAP(&lfqueue->in_free_mode, 0, 1)) {
		lfq_get_curr_time(&curr_time);
		lfqueue_cas_node_t *rtfree = lfqueue->root_free, *nextfree;
		while ( rtfree && (rtfree != lfqueue->move_free) ) {
			nextfree = rtfree->nextfree;
			if ( lfq_diff_time(curr_time, rtfree->_deactivate_tm) > 2) {
				//	printf("%p\n", rtfree);
				lfqueue->_free(lfqueue->pl, rtfree);
				rtfree = nextfree;
			} else {
				break;
			}
		}
		lfqueue->root_free = rtfree;
		__LFQ_BOOL_COMPARE_AND_SWAP(&lfqueue->in_free_mode, 1, 0);
	}
	__LFQ_SYNC_MEMORY();
}

int
lfqueue_init(lfqueue_t *lfqueue) {
	return lfqueue_init_mf(lfqueue, NULL, _lfqueue_malloc, _lfqueue_free);
}

int
lfqueue_init_mf(lfqueue_t *lfqueue, void* pl, lfqueue_malloc_fn lfqueue_malloc, lfqueue_free_fn lfqueue_free) {
	lfqueue->_malloc = lfqueue_malloc;
	lfqueue->_free = lfqueue_free;
	lfqueue->pl = pl;

	lfqueue_cas_node_t *base = lfqueue_malloc(pl, sizeof(lfqueue_cas_node_t));
	lfqueue_cas_node_t *freebase = lfqueue_malloc(pl, sizeof(lfqueue_cas_node_t));
	if (base == NULL || freebase == NULL) {
		perror("malloc");
		return errno;
	}
	base->value = NULL;
	base->next = NULL;
	base->nextfree = NULL;
	base->_deactivate_tm = 0;

	freebase->value = NULL;
	freebase->next = NULL;
	freebase->nextfree = NULL;
	freebase->_deactivate_tm = 0;

	lfqueue->head = lfqueue->tail = base; // Not yet to be free for first node only
	lfqueue->root_free = lfqueue->move_free = freebase; // Not yet to be free for first node only
	lfqueue->size = 0;
	lfqueue->in_free_mode = 0;

	return 0;
}

void
lfqueue_destroy(lfqueue_t *lfqueue) {
	void* p;
	while ((p = lfqueue_deq(lfqueue))) {
		lfqueue->_free(lfqueue->pl, p);
	}
	// Clear the recycle chain nodes
	lfqueue_cas_node_t *rtfree = lfqueue->root_free, *nextfree;
	while (rtfree && (rtfree != lfqueue->move_free) ) {
		nextfree = rtfree->nextfree;
		lfqueue->_free(lfqueue->pl, rtfree);
		rtfree = nextfree;
	}
	if (rtfree) {
		lfqueue->_free(lfqueue->pl, rtfree);
	}

	lfqueue->_free(lfqueue->pl, lfqueue->tail); // Last free

	lfqueue->size = 0;
}

int
lfqueue_enq(lfqueue_t *lfqueue, void *value) {
	if (_enqueue(lfqueue, value)) {
		return -1;
	}
	__LFQ_ADD_AND_FETCH(&lfqueue->size, 1);
	return 0;
}

void*
lfqueue_deq(lfqueue_t *lfqueue) {
	void *v;
	if (//__LFQ_ADD_AND_FETCH(&lfqueue->size, 0) &&
	    (v = _dequeue(lfqueue))
	) {

		__LFQ_FETCH_AND_ADD(&lfqueue->size, -1);
		return v;
	}
	return NULL;
}

void*
lfqueue_deq_must(lfqueue_t *lfqueue) {
	void *v;
	while ( !(v = _dequeue(lfqueue)) ) {
		// Rest the thread for other thread, to avoid keep looping force
		lfqueue_sleep(1);
	}
	__LFQ_FETCH_AND_ADD(&lfqueue->size, -1);
	return v;
}

/**This is only applicable when only single thread consume only**/
void*
lfqueue_single_deq(lfqueue_t *lfqueue) {
	void *v;
	if (//__LFQ_ADD_AND_FETCH(&lfqueue->size, 0) &&
	    (v = _single_dequeue(lfqueue))
	) {

		__LFQ_FETCH_AND_ADD(&lfqueue->size, -1);
		return v;
	}
	return NULL;
}

/**This is only applicable when only single thread consume only**/
void*
lfqueue_single_deq_must(lfqueue_t *lfqueue) {
	void *v;
	while ( !(v = _single_dequeue(lfqueue)) ) {
		// Rest the thread for other thread, to avoid keep looping force
		lfqueue_sleep(1);
	}
	__LFQ_FETCH_AND_ADD(&lfqueue->size, -1);
	return v;
}

size_t
lfqueue_size(lfqueue_t *lfqueue) {
	return __LFQ_ADD_AND_FETCH(&lfqueue->size, 0);
}

void
lfqueue_sleep(unsigned int milisec) {
#if defined __GNUC__ || defined __CYGWIN__ || defined __MINGW32__ || defined __APPLE__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
	usleep(milisec * 1000);
#pragma GCC diagnostic pop
#else
	Sleep(milisec);
#endif
}

#ifdef __cplusplus
}
#endif
