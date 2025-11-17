/* threads/thread.h */
#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#ifdef VM
#include "vm/vm.h"
#endif

/* States in a thread's life cycle. */
enum thread_status {
    THREAD_RUNNING, /* Running thread. */
    THREAD_READY,   /* Not running but ready to run. */
    THREAD_BLOCKED, /* Waiting for an event to trigger. */
    THREAD_DYING    /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) - 1) /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0      /* Lowest priority. */
#define PRI_DEFAULT 31 /* Default priority. */
#define PRI_MAX 63     /* Highest priority. */

/* A kernel thread or user process. */
struct thread {
    /* Owned by thread.c. */
    tid_t tid;                 /* Thread identifier. */
    enum thread_status status; /* Thread state. */
    char name[16];             /* Name (for debugging purposes). */
    int priority;              /* 유효(Effective) 우선순위 */

    int64_t wakeup_tick; /* 스레드가 깨어나야 할 틱 */

    int base_priority; /* 기부받기 전 '기본' 우선순위 */

    struct lock* waiting_for_lock; /* 현재 대기 중인 락 (없으면 NULL) */

    struct list donations;
    struct list_elem donation_elem;

    /* Shared between thread.c and synch.c. */
    struct list_elem elem; /* Ready list, Sleep list 등에 사용 */

    int nice;
    int recent_cpu;
    struct list_elem all_elem;

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint64_t* pml4; /* Page map level 4 */
#endif
#ifdef VM
    /* Table for whole virtual memory owned by thread. */
    struct supplemental_page_table spt;
#endif

    /* Owned by thread.c. */
    struct intr_frame tf; /* Information for switching */
    unsigned magic;       /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void* aux);
tid_t thread_create(const char* name, int priority, thread_func*, void*);

void thread_block(void);
void thread_unblock(struct thread*);

struct thread* thread_current(void);
tid_t thread_tid(void);
const char* thread_name(void);

void thread_exit(void) NO_RETURN;
void thread_yield(void);

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

void do_iret(struct intr_frame* tf);

/* 'elem' 멤버를 기준으로 우선순위를 비교하는 함수 (내림차순) */
bool thread_priority_less_func(const struct list_elem* a,
                               const struct list_elem* b,
                               void* aux UNUSED);

/* 스레드의 우선순위를 (기부 리스트를 참고하여) 재계산합니다. */
void thread_recalculate_priority(struct thread* t);

/* 'donation_elem'을 기준으로 우선순위를 비교하는 함수 (내림차순) */
bool thread_donation_less_func(const struct list_elem* a,
                               const struct list_elem* b,
                               void* aux UNUSED);

/* sleep_list를 확인하여 깨어날 시간이 된 스레드를 깨웁니다. */
void thread_wakeup(int64_t current_ticks);

bool thread_wake_up_tick_less_func(const struct list_elem* a,
                                   const struct list_elem* b,
                                   void* aux UNUSED);

void mlfqs_increment_recent_cpu(void);
void mlfqs_update_load_avg(void);
void mlfqs_update_all_recent_cpu(void);
void mlfqs_update_all_priorities(void);
void mlfqs_calculate_priority(struct thread* t);

#endif /* threads/thread.h */