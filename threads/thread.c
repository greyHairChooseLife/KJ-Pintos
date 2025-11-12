/* threads/thread.c */
#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "threads/fixed-point.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* struct thread의 `magic` 멤버를 위한 임의의 값입니다. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본 스레드를 위한 임의의 값입니다. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태의 프로세스 리스트 (우선순위 큐) */
static struct list ready_list;

/* 잠자는 스레드들의 리스트 (wakeup_tick 순으로 정렬됨) */
static struct list sleep_list;

/* 'elem' 멤버를 기준으로 wakeup_tick을 비교하는 함수 (오름차순) */
static bool thread_wakeup_less_func(const struct list_elem *a,
                                    const struct list_elem *b,
                                    void *aux UNUSED)
{
    const struct thread *thread_a = list_entry(a, struct thread, elem);
    const struct thread *thread_b = list_entry(b, struct thread, elem);
	
    return thread_a->wakeup_tick < thread_b->wakeup_tick;
}

/* 유휴 스레드 (Idle thread). */
static struct thread *idle_thread;

/* 초기 스레드 (Initial thread), init.c:main()을 실행하는 스레드입니다. */
static struct thread *initial_thread;

/* allocate_tid()에 의해 사용되는 락입니다. */
static struct lock tid_lock;

/* 스레드 파괴 요청 리스트 */
static struct list destruction_req;

/* 통계. */
static long long idle_ticks;    /* 유휴(idle) 상태로 보낸 타이머 틱 수. */
static long long kernel_ticks;  /* 커널 스레드에서 보낸 타이머 틱 수. */
static long long user_ticks;    /* 사용자 프로그램에서 보낸 타이머 틱 수. */

/* 스케줄링. */
#define TIME_SLICE 4            /* 각 스레드에 할당할 타이머 틱 수. */
static unsigned thread_ticks;   /* 마지막 yield 이후의 타이머 틱 수. */

/* false (기본값)일 경우, 라운드 로빈 스케줄러를 사용합니다.
   true일 경우, 다단계 피드백 큐 스케줄러를 사용합니다. */
bool thread_mlfqs;

static int load_avg; // MLFQS를 위한 전역 load_avg (고정소수점)

static struct list all_threads;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule(void);
static tid_t allocate_tid(void);

/* T가 유효한 스레드를 가리키는 것으로 보이면 true를 반환합니다. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 실행 중인 스레드를 반환합니다. */
#define running_thread() ((struct thread *)(pg_round_down(rrsp())))

// thread_start를 위한 전역 디스크립터 테이블(GDT).
static uint64_t gdt[3] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff};

bool thread_wake_up_tick_less_func(const struct list_elem *a,
                               const struct list_elem *b,
                               void *aux UNUSED) 
{
    const struct thread *thread_a = list_entry(a, struct thread, elem);
    const struct thread *thread_b = list_entry(b, struct thread, elem);

    return thread_a->wakeup_tick > thread_b->wakeup_tick;
}

/* 스레딩 시스템을 초기화합니다. */
void thread_init(void)
{
    ASSERT(intr_get_level() == INTR_OFF);

    struct desc_ptr gdt_ds = {
        .size = sizeof(gdt) - 1,
        .address = (uint64_t)gdt};
    lgdt(&gdt_ds);

    /* 전역 스레드 컨텍스트 초기화 */
    lock_init(&tid_lock);
    list_init(&ready_list);
    list_init(&destruction_req);
    
    list_init(&sleep_list);

    if (thread_mlfqs) {
        load_avg = 0;
        list_init(&all_threads);
    }

    /* 실행 중인 스레드를 위한 스레드 구조체를 설정합니다. */
    initial_thread = running_thread();
    init_thread(initial_thread, "main", PRI_DEFAULT);
    initial_thread->status = THREAD_RUNNING;
    initial_thread->tid = allocate_tid();
}

/* 선점형 스레드 스케줄링을 시작합니다. */
void thread_start(void)
{
    /* 유휴 스레드 생성. */
    struct semaphore idle_started;
    sema_init(&idle_started, 0);
    thread_create("idle", PRI_MIN, idle, &idle_started);

    /* 선점형 스레드 스케줄링 시작. */
    intr_enable();

    /* 유휴 스레드가 idle_thread를 초기화할 때까지 기다립니다. */
    sema_down(&idle_started);
}

/* sleep_list를 확인하여 깨어날 시간이 된 스레드를 깨웁니다. */
void thread_wakeup(int64_t current_ticks)
{
    struct list_elem *e = list_begin(&sleep_list);

    // sleep_list는 wakeup_tick 오름차순으로 정렬되어 있습니다.
    while (e != list_end(&sleep_list))
    {
        struct thread *t = list_entry(e, struct thread, elem);

        if (t->wakeup_tick <= current_ticks) {
            // 깨어날 시간이 됨. 리스트에서 제거하고 unblock
            e = list_remove(e);
            thread_unblock(t);
        } else {
            // 리스트의 첫 스레드도 깨어날 시간이 안됨.
            // (뒤는 볼 필요 없음)
            break;
        }
    }
}

void thread_sleep_until(int64_t wakeup_tick)
{
    struct thread *curr = thread_current();
    curr->wakeup_tick = wakeup_tick;

    enum intr_level old_level;
    old_level = intr_disable();

    // sleep_list에 wakeup_tick 순서대로 정렬 삽입
    list_insert_ordered(&sleep_list, &(curr->elem), thread_wakeup_less_func, NULL);
    thread_block(); // 잠들기

    intr_set_level(old_level);
}

/* 매 타이머 틱마다 타이머 인터럽트 핸들러에 의해 호출됩니다. */
void thread_tick(void)
{
    struct thread *t = thread_current();

    /* 통계 업데이트. */
    if (t == idle_thread)
        idle_ticks++;
#ifdef USERPROG
    else if (t->pml4 != NULL)
        user_ticks++;
#endif
    else
        kernel_ticks++;

    /* 선점(Preemption) 적용. */
    if (++thread_ticks >= TIME_SLICE)
        intr_yield_on_return();
}

/* 스레드 통계를 출력합니다. */
void thread_print_stats(void)
{
    printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
           idle_ticks, kernel_ticks, user_ticks);
}

/* 새로운 커널 스레드를 생성합니다. */
tid_t thread_create(const char *name, int priority, thread_func *function, void *aux)
{
    struct thread *t;
    tid_t tid;

    ASSERT(function != NULL);

    /* 스레드 할당. */
    t = palloc_get_page(PAL_ZERO);
    if (t == NULL)
        return TID_ERROR;

    /* 스레드 초기화. */
    init_thread(t, name, priority);
    tid = t->tid = allocate_tid();

    /* 스케줄될 경우 kernel_thread를 호출합니다. */
    t->tf.rip = (uintptr_t)kernel_thread;
    t->tf.R.rdi = (uint64_t)function;
    t->tf.R.rsi = (uint64_t)aux;
    t->tf.ds = SEL_KDSEG;
    t->tf.es = SEL_KDSEG;
    t->tf.ss = SEL_KDSEG;
    t->tf.cs = SEL_KCSEG;
    t->tf.eflags = FLAG_IF;

    /* 실행 큐에 추가. (선점이 발생할 수 있음) */
    thread_unblock(t);

    return tid;
}

/* 현재 스레드를 잠들게(sleep) 합니다. */
void thread_block(void)
{
    ASSERT(!intr_context());
    ASSERT(intr_get_level() == INTR_OFF);
    thread_current()->status = THREAD_BLOCKED;
    schedule();
}

bool thread_priority_less_func(const struct list_elem *a,
                               const struct list_elem *b,
                               void *aux UNUSED) 
{
    const struct thread *thread_a = list_entry(a, struct thread, elem);
    const struct thread *thread_b = list_entry(b, struct thread, elem);

    return thread_a->priority > thread_b->priority;
}

void thread_unblock(struct thread *t)
{
    enum intr_level old_level;

    ASSERT(is_thread(t));
    old_level = intr_disable();
    ASSERT(t->status == THREAD_BLOCKED);

    /* 우선순위 큐 (ready_list)에 정렬 삽입 */
    list_insert_ordered(&ready_list, &t->elem, thread_priority_less_func, NULL);
    
    t->status = THREAD_READY;

    // 새로 깨어난 t의 우선순위가 현재 스레드보다 높은지 확인
    if (thread_current() != idle_thread &&
        t->priority > thread_current()->priority)
    {
        if (intr_context())
        {
            intr_yield_on_return(); // 인터럽트가 끝나면 스케줄링
        }
        else
        {
            thread_yield();
        }
    }
    
    intr_set_level(old_level);
}

/* 실행 중인 스레드의 이름을 반환합니다. */
const char * thread_name(void)
{
    return thread_current()->name;
}

/* 실행 중인 스레드를 반환합니다. */
struct thread * thread_current(void)
{
    struct thread *t = running_thread();

    /* 스택 오버플로 검사 */
    ASSERT(is_thread(t));
    ASSERT(t->status == THREAD_RUNNING);

    return t;
}

/* 실행 중인 스레드의 tid를 반환합니다. */
tid_t thread_tid(void)
{
    return thread_current()->tid;
}

/* 현재 스레드를 비스케줄(deschedule)하고 파괴합니다. */
void thread_exit(void)
{
    ASSERT(!intr_context());

#ifdef USERPROG
    process_exit();
#endif

    intr_disable();
    do_schedule(THREAD_DYING);
    NOT_REACHED();
}

/* CPU를 양보(yield)합니다. */
void thread_yield(void)
{
    struct thread *curr = thread_current();
    enum intr_level old_level;

    ASSERT(!intr_context());

    old_level = intr_disable();
    if (curr != idle_thread)
        /* 우선순위 큐 (ready_list)에 정렬 삽입 */
        list_insert_ordered(&ready_list, &curr->elem, thread_priority_less_func, NULL);
    do_schedule(THREAD_READY);
    intr_set_level(old_level);
}

bool thread_donation_less_func(const struct list_elem *a,
                               const struct list_elem *b,
                               void *aux UNUSED)
{
    const struct thread *thread_a = list_entry(a, struct thread, donation_elem);
    const struct thread *thread_b = list_entry(b, struct thread, donation_elem);

    return thread_a->priority > thread_b->priority;
}

/* 스레드 t의 우선순위를 (기부 리스트를 참고하여) 재계산합니다. */
void thread_recalculate_priority(struct thread *t)
{
    enum intr_level old_level = intr_disable();

    int max_donation_priority = PRI_MIN;

    // donations 리스트(기부자 목록)가 비어있지 않다면,
    // (정렬되어 있으므로) 리스트의 맨 앞(가장 높은 우선순위)을 확인합니다.
    if (!list_empty(&t->donations)) {
        struct list_elem *e = list_front(&t->donations);
        max_donation_priority = list_entry(e, struct thread, donation_elem)->priority;
    }

    // 새 유효 우선순위 = max(기본 우선순위, 기부받은 최고 우선순위)
    if (t->base_priority > max_donation_priority) {
        t->priority = t->base_priority;
    } else {
        t->priority = max_donation_priority;
    }

    intr_set_level(old_level);
}

/* 현재 스레드의 *기본* 우선순위를 NEW_PRIORITY로 설정합니다. */
void thread_set_priority(int new_priority)
{
    if (thread_mlfqs) {
        return;
    }

    /* (MLFQS가 꺼져있을 때만 기존의 기부 로직이 실행됩니다) */
    enum intr_level old_level = intr_disable();
    struct thread *curr = thread_current();
    
    curr->base_priority = new_priority;
    thread_recalculate_priority(curr);
    
    if (!list_empty(&ready_list) &&
        list_entry(list_begin(&ready_list), struct thread, elem)->priority > curr->priority)
    {
        thread_yield();
    }

    intr_set_level(old_level);
}

/* 현재 스레드의 (유효) 우선순위를 반환합니다. */
int thread_get_priority(void)
{
    enum intr_level old_level = intr_disable();
    int priority = thread_current()->priority;
    intr_set_level(old_level);
    return priority;
}

/* --- MLFQS Stubs (Project 1-3) --- */
void thread_set_nice(int nice) 
{
    /* MLFQS 모드일 때만 작동해야 합니다. */
    if (!thread_mlfqs) return;

    enum intr_level old_level = intr_disable();
    
    thread_current()->nice = nice;
    
    // 1. nice 값이 바뀌었으므로, 즉시 우선순위를 재계산합니다.
    mlfqs_calculate_priority(thread_current());

    // 2. 우선순위가 낮아졌을 수 있으므로, 선점 여부를 확인합니다.
    if (!list_empty(&ready_list) &&
        list_entry(list_begin(&ready_list), struct thread, elem)->priority > thread_current()->priority)
    {
        thread_yield();
    }
    
    intr_set_level(old_level);
}

int thread_get_nice(void)
{
    /* MLFQS 모드일 때만 작동해야 합니다. */
    if (!thread_mlfqs) return 0; // (또는 기존 priority 반환?)
    
    enum intr_level old_level = intr_disable();
    int nice = thread_current()->nice;
    intr_set_level(old_level);
    
    return nice;
}

int thread_get_load_avg(void)
{
    if (!thread_mlfqs)
        return 0;

    enum intr_level old_level = intr_disable();

    // (고정소수점 * 정수)
    int n_load_avg = load_avg * 100;

    // 반올림하여 정수로 변환
    int result;
    if (n_load_avg >= 0) {
        result = (n_load_avg + (1 << 13)) / (1 << 14);
    } else {
        result = (n_load_avg - (1 << 13)) / (1 << 14);
    }

    intr_set_level(old_level);
    return result;
}

int thread_get_recent_cpu(void)
{
    if (!thread_mlfqs)
        return 0; // 또는 thread_get_priority();

    enum intr_level old_level = intr_disable();

    // recent_cpu * 100을 계산 (둘 다 고정소수점)
    // int x = multiply_mixed(thread_current()->recent_cpu, 100);

    // (고정소수점 * 정수)
    int n_recent_cpu = thread_current()->recent_cpu * 100;

    // 반올림하여 정수로 변환
    int result;
    if (n_recent_cpu >= 0) {
        result = (n_recent_cpu + (1 << 13)) / (1 << 14); // (f / 2) / f
    } else {
        result = (n_recent_cpu - (1 << 13)) / (1 << 14);
    }

    intr_set_level(old_level);
    return result;
}

/* 유휴(Idle) 스레드. */
static void idle(void *idle_started_ UNUSED)
{
    struct semaphore *idle_started = idle_started_;

    idle_thread = thread_current();
    sema_up(idle_started);

    for (;;)
    {
        /* 다른 스레드가 실행되도록 합니다. */
        intr_disable();
        thread_block();

        /* 인터럽트를 다시 활성화하고 다음 인터럽트를 기다립니다. */
        asm volatile("sti; hlt" : : : "memory");
    }
}

/* 커널 스레드의 기반으로 사용되는 함수입니다. */
static void kernel_thread(thread_func *function, void *aux)
{
    ASSERT(function != NULL);

    intr_enable(); /* 스케줄러는 인터럽트가 꺼진 상태로 실행됩니다. */
    function(aux); /* 스레드 함수를 실행합니다. */
    thread_exit(); /* function()이 반환되면, 스레드를 종료합니다. */
}

/* T를 NAME이라는 이름의 차단된(blocked) 스레드로
   기본 초기화합니다. */
static void init_thread (struct thread *t, const char *name, int priority)
{
    ASSERT (t != NULL);
    /* priority 인자 자체는 0~63 범위인지 확인합니다. */
    ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
    ASSERT (name != NULL);

    memset (t, 0, sizeof *t);
    t->status = THREAD_BLOCKED;
    strlcpy (t->name, name, sizeof t->name);
    t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
    
    t->wakeup_tick = 0;
    t->magic = THREAD_MAGIC;

    if (thread_mlfqs)
    {
        /* 'initial_thread' (main)는 0으로 초기화합니다. */
        if (t == initial_thread) {
            t->nice = 0;
            t->recent_cpu = 0;
        } else {
            /* 다른 모든 스레드는 부모(현재 스레드)의 값을 상속받습니다. */
            struct thread *parent = thread_current();
            t->nice = parent->nice;
            t->recent_cpu = parent->recent_cpu;
        }
    
        list_push_back(&all_threads, &t->all_elem);

        mlfqs_calculate_priority(t);

        /* (기존 우선순위 스케줄러 멤버는 0 또는 NULL로 둡니다) */
        t->base_priority = 0; 
        t->waiting_for_lock = NULL;
        list_init (&t->donations);
    }
    else
    {
        t->priority = priority;
        t->base_priority = priority;
        t->waiting_for_lock = NULL;
        list_init (&t->donations);

        t->nice = 0;
        t->recent_cpu = 0;
    }
}

/* 다음에 스케줄될 스레드를 선택하여 반환합니다. */
static struct thread * next_thread_to_run(void)
{
    if (list_empty(&ready_list))
        return idle_thread;
    else
        /* ready_list가 우선순위 내림차순이므로, 맨 앞을 뽑습니다. */
        return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/* iretq를 사용하여 스레드를 실행(launch)합니다 */
void do_iret(struct intr_frame *tf)
{
    __asm __volatile(
        "movq %0, %%rsp\n"
        "movq 0(%%rsp),%%r15\n"
        "movq 8(%%rsp),%%r14\n"
        "movq 16(%%rsp),%%r13\n"
        "movq 24(%%rsp),%%r12\n"
        "movq 32(%%rsp),%%r11\n"
        "movq 40(%%rsp),%%r10\n"
        "movq 48(%%rsp),%%r9\n"
        "movq 56(%%rsp),%%r8\n"
        "movq 64(%%rsp),%%rsi\n"
        "movq 72(%%rsp),%%rdi\n"
        "movq 80(%%rsp),%%rbp\n"
        "movq 88(%%rsp),%%rdx\n"
        "movq 96(%%rsp),%%rcx\n"
        "movq 104(%%rsp),%%rbx\n"
        "movq 112(%%rsp),%%rax\n"
        "addq $120,%%rsp\n"
        "movw 8(%%rsp),%%ds\n"
        "movw (%%rsp),%%es\n"
        "addq $32, %%rsp\n"
        "iretq"
        : : "g"((uint64_t)tf) : "memory");
}

/* 스레드를 전환(Switching)합니다. */
static void thread_launch(struct thread *th)
{
    uint64_t tf_cur = (uint64_t)&running_thread()->tf;
    uint64_t tf = (uint64_t)&th->tf;
    ASSERT(intr_get_level() == INTR_OFF);

    /* 주된 전환 로직(switching logic). */
    __asm __volatile(
        /* 사용될 레지스터 저장. */
        "push %%rax\n"
        "push %%rbx\n"
        "push %%rcx\n"
        /* 입력을 한 번에 가져오기 */
        "movq %0, %%rax\n"
        "movq %1, %%rcx\n"
        "movq %%r15, 0(%%rax)\n"
        "movq %%r14, 8(%%rax)\n"
        "movq %%r13, 16(%%rax)\n"
        "movq %%r12, 24(%%rax)\n"
        "movq %%r11, 32(%%rax)\n"
        "movq %%r10, 40(%%rax)\n"
        "movq %%r9, 48(%%rax)\n"
        "movq %%r8, 56(%%rax)\n"
        "movq %%rsi, 64(%%rax)\n"
        "movq %%rdi, 72(%%rax)\n"
        "movq %%rbp, 80(%%rax)\n"
        "movq %%rdx, 88(%%rax)\n"
        "pop %%rbx\n" // 저장된 rcx
        "movq %%rbx, 96(%%rax)\n"
        "pop %%rbx\n" // 저장된 rbx
        "movq %%rbx, 104(%%rax)\n"
        "pop %%rbx\n" // 저장된 rax
        "movq %%rbx, 112(%%rax)\n"
        "addq $120, %%rax\n"
        "movw %%es, (%%rax)\n"
        "movw %%ds, 8(%%rax)\n"
        "addq $32, %%rax\n"
        "call __next\n" // 현재 rip 읽기.
        "__next:\n"
        "pop %%rbx\n"
        "addq $(out_iret -  __next), %%rbx\n"
        "movq %%rbx, 0(%%rax)\n" // rip
        "movw %%cs, 8(%%rax)\n"  // cs
        "pushfq\n"
        "popq %%rbx\n"
        "mov %%rbx, 16(%%rax)\n" // eflags
        "mov %%rsp, 24(%%rax)\n" // rsp
        "movw %%ss, 32(%%rax)\n"
        "mov %%rcx, %%rdi\n"
        "call do_iret\n"
        "out_iret:\n"
        : : "g"(tf_cur), "g"(tf) : "memory");
}

/* 새 프로세스를 스케줄합니다. */
static void do_schedule(int status)
{
    ASSERT(intr_get_level() == INTR_OFF);
    ASSERT(thread_current()->status == THREAD_RUNNING);
    while (!list_empty(&destruction_req))
    {
        struct thread *victim =
            list_entry(list_pop_front(&destruction_req), struct thread, elem);

        if (thread_mlfqs) {
            list_remove(&victim->all_elem);
        }

        palloc_free_page(victim);
    }
    thread_current()->status = status;
    schedule();
}

static void schedule(void)
{
    struct thread *curr = running_thread();
    struct thread *next = next_thread_to_run();

    ASSERT(intr_get_level() == INTR_OFF);
    ASSERT(curr->status != THREAD_RUNNING);
    ASSERT(is_thread(next));
    /* 우리를 '실행 중'으로 표시. */
    next->status = THREAD_RUNNING;

    /* 새 타임 슬라이스 시작. */
    thread_ticks = 0;

#ifdef USERPROG
    /* 새 주소 공간 활성화. */
    process_activate(next);
#endif

    if (curr != next)
    {
        /* 죽는 스레드 처리 */
        if (curr && curr->status == THREAD_DYING && curr != initial_thread)
        {
            ASSERT(curr != next);
            list_push_back(&destruction_req, &curr->elem);
        }

        /* 스레드 전환 */
        thread_launch(next);
    }
}

/* 새 스레드를 위한 tid를 반환합니다. */
static tid_t allocate_tid(void)
{
    static tid_t next_tid = 1;
    tid_t tid;

    lock_acquire(&tid_lock);
    tid = next_tid++;
    lock_release(&tid_lock);

    return tid;
}

/* [공식 1] MLFQS: 스레드 t의 우선순위를 계산합니다.*/
void mlfqs_calculate_priority(struct thread *t)
{
    if (t == idle_thread) return;

    // priority = PRI_MAX - (recent_cpu / 4) - (nice * 2)
    
    // (recent_cpu / 4) -> 고정소수점을 정수로 변환 (버림)
    int recent_cpu_term = FP_TO_INT_TRUNC(FP_DIV_MIXED(t->recent_cpu, 4));
    
    // (nice * 2)
    int nice_term = t->nice * 2;
    
    int new_priority = PRI_MAX - recent_cpu_term - nice_term;

    // 우선순위 범위를 PRI_MIN(0) ~ PRI_MAX(63)로 고정(clamp)
    if (new_priority < PRI_MIN) {
        t->priority = PRI_MIN;
    } else if (new_priority > PRI_MAX) {
        t->priority = PRI_MAX;
    } else {
        t->priority = new_priority;
    }
}

/* [공식 1] 매 4틱마다: *모든* 스레드의 우선순위를 재계산합니다.*/
void mlfqs_update_all_priorities(void)
{
    ASSERT(thread_mlfqs);
    ASSERT(intr_context());
    
    struct list_elem *e;
    for (e = list_begin(&all_threads); e != list_end(&all_threads); e = list_next(e))
    {
        struct thread *t = list_entry(e, struct thread, all_elem);
        mlfqs_calculate_priority(t);
    }
}

/* [공식 2] 매 틱마다: 현재 스레드(idle 제외)의 recent_cpu를 1 증가시킵니다.*/
void mlfqs_increment_recent_cpu(void)
{
    ASSERT(thread_mlfqs);
    ASSERT(intr_context());

    struct thread *current = thread_current();
    if (current != idle_thread) 
    {
        // recent_cpu = recent_cpu + 1 (고정소수점)
        current->recent_cpu = FP_ADD_MIXED(current->recent_cpu, 1);
    }
}

/* [공식 3] 매 1초마다: *모든* 스레드의 recent_cpu를 재계산합니다.*/
void mlfqs_update_all_recent_cpu(void)
{
    ASSERT(thread_mlfqs);
    ASSERT(intr_context());
    
    // 계수(Coefficient) = (2 * load_avg) / (2 * load_avg + 1)
    int load_avg_x2 = FP_MULT_MIXED(load_avg, 2);
    int coeff = FP_DIV(load_avg_x2, FP_ADD_MIXED(load_avg_x2, 1));
    
    struct list_elem *e;
    for (e = list_begin(&all_threads); e != list_end(&all_threads); e = list_next(e))
    {
        struct thread *t = list_entry(e, struct thread, all_elem);
        if (t == idle_thread) continue;
        
        // recent_cpu = (계수 * recent_cpu) + nice
        t->recent_cpu = FP_ADD_MIXED(FP_MULT(coeff, t->recent_cpu), t->nice);
    }
}

/* [공식 4] 매 1초마다: 시스템 load_avg를 재계산합니다.*/
void mlfqs_update_load_avg(void)
{
    ASSERT(thread_mlfqs);
    ASSERT(intr_context());

    int ready_threads;
    
    // ready_threads = ready_list 크기 + (실행 중인 스레드 (idle 제외))
    if (thread_current() == idle_thread) {
        ready_threads = list_size(&ready_list);
    } else {
        ready_threads = list_size(&ready_list) + 1;
    }

    // load_avg = (59/60) * load_avg + (1/60) * ready_threads
    
    // (59/60) * load_avg
    int term1 = FP_MULT(FP_DIV_MIXED(INT_TO_FP(59), 60), load_avg);
    
    // (1/60) * ready_threads
    int term2 = FP_MULT_MIXED(FP_DIV_MIXED(INT_TO_FP(1), 60), ready_threads);
    
    load_avg = FP_ADD(term1, term2);
}