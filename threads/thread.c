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
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* struct thread의 `magic` 멤버를 위한 임의의 값입니다.
   스택 오버플로를 감지하는 데 사용됩니다. 자세한 내용은
   thread.h 상단의 큰 주석을 참조하세요. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본 스레드를 위한 임의의 값입니다.
   이 값을 수정하지 마세요. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태의 프로세스 리스트, 즉 실행 준비는 되었지만
   실제로 실행되고 있지는 않은 프로세스들의 리스트입니다. */
static struct list ready_list;

/* 유휴 스레드 (Idle thread). */
static struct thread *idle_thread;

/* 초기 스레드 (Initial thread), init.c:main()을 실행하는 스레드입니다. */
static struct thread *initial_thread;

/* allocate_tid()에 의해 사용되는 락입니다. */
static struct lock tid_lock;

/* 스레드 파괴 요청 리스트 */
static struct list destruction_req;

/* 통계. */
static long long idle_ticks;   /* 유휴(idle) 상태로 보낸 타이머 틱 수. */
static long long kernel_ticks; /* 커널 스레드에서 보낸 타이머 틱 수. */
static long long user_ticks;   /* 사용자 프로그램에서 보낸 타이머 틱 수. */

/* 스케줄링. */
#define TIME_SLICE 4		  /* 각 스레드에 할당할 타이머 틱 수. */
static unsigned thread_ticks; /* 마지막 yield 이후의 타이머 틱 수. */

/* false (기본값)일 경우, 라운드 로빈 스케줄러를 사용합니다.
   true일 경우, 다단계 피드백 큐 스케줄러를 사용합니다.
   커널 명령줄 옵션 "-o mlqs"에 의해 제어됩니다. */
bool thread_mlfqs;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule(void);
static tid_t allocate_tid(void);

/* T가 유효한 스레드를 가리키는 것으로 보이면 true를 반환합니다. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 실행 중인 스레드를 반환합니다.
 * CPU의 스택 포인터 `rsp`를 읽은 다음,
 * 그 값을 페이지의 시작 주소로 내림(round down)합니다.
 * `struct thread`는 항상 페이지의 시작 부분에 위치하고
 * 스택 포인터는 그 중간 어딘가에 있으므로,
 * 이 연산은 현재 스레드를 찾습니다. */
#define running_thread() ((struct thread *)(pg_round_down(rrsp())))

// thread_start를 위한 전역 디스크립터 테이블(GDT).
// gdt는 thread_init 이후에 설정될 것이므로,
// 먼저 임시 gdt를 설정해야 합니다.
static uint64_t gdt[3] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff};

/* 현재 실행 중인 코드를 스레드로 변환하여
   스레딩 시스템을 초기화합니다. 이 작업은 일반적인 경우에는
   작동할 수 없지만, 이 경우에는 loader.S가 스택의 바닥을
   페이지 경계에 신중하게 배치했기 때문에 가능합니다.

   또한 실행 큐(run queue)와 tid 락을 초기화합니다.

   이 함수를 호출한 후에는, thread_create()로
   다른 스레드를 생성하기 전에 반드시
   페이지 할당자(page allocator)를 초기화해야 합니다.

   이 함수가 완료될 때까지 thread_current()를
   호출하는 것은 안전하지 않습니다. */
void thread_init(void)
{
	ASSERT(intr_get_level() == INTR_OFF);

	/* 커널을 위한 임시 gdt를 다시 로드합니다.
	 * 이 gdt는 유저 컨텍스트를 포함하지 않습니다.
	 * 커널은 gdt_init()에서 유저 컨텍스트를 포함하여
	 * gdt를 다시 빌드할 것입니다. */
	struct desc_ptr gdt_ds = {
		.size = sizeof(gdt) - 1,
		.address = (uint64_t)gdt};
	lgdt(&gdt_ds);

	/* 전역 스레드 컨텍스트 초기화 */
	lock_init(&tid_lock);
	list_init(&ready_list);
	list_init(&destruction_req);

	/* 실행 중인 스레드를 위한 스레드 구조체를 설정합니다. */
	initial_thread = running_thread();
	init_thread(initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid();
}

/* 인터럽트를 활성화하여 선점형 스레드 스케줄링을 시작합니다.
   또한 유휴(idle) 스레드를 생성합니다. */
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

/* 매 타이머 틱마다 타이머 인터럽트 핸들러에 의해 호출됩니다.
   따라서, 이 함수는 외부 인터럽트 컨텍스트에서 실행됩니다. */
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

/* NAME이라는 이름과 주어진 초기 PRIORITY를 가진 새로운 커널 스레드를
   생성합니다. 이 스레드는 AUX를 인자로 하여 FUNCTION을 실행하며,
   준비 큐(ready queue)에 추가됩니다. 새 스레드의 스레드 식별자(TID)를
   반환하며, 생성에 실패하면 TID_ERROR를 반환합니다.

   만약 thread_start()가 이미 호출되었다면, 새 스레드는
   thread_create()가 반환되기 전에 스케줄될 수 있습니다.
   심지어 thread_create()가 반환되기 전에 종료될 수도 있습니다.
   반대로, 원래 스레드는 새 스레드가 스케줄되기 전에
   얼마든지 오랫동안 실행될 수 있습니다. 실행 순서를 보장해야 한다면
   세마포어나 다른 형태의 동기화 기법을 사용하세요.

   제공된 코드는 새 스레드의 `priority` 멤버를 PRIORITY로
   설정하지만, 실제 우선순위 스케줄링은 구현되어 있지 않습니다.
   우선순위 스케줄링은 문제 1-3의 목표입니다. */
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

	/* 스케줄될 경우 kernel_thread를 호출합니다.
	 * 참고) rdi는 첫 번째 인자, rsi는 두 번째 인자입니다. */
	t->tf.rip = (uintptr_t)kernel_thread;
	t->tf.R.rdi = (uint64_t)function;
	t->tf.R.rsi = (uint64_t)aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* 실행 큐에 추가. */
	thread_unblock(t);

	return tid;
}

/* 현재 스레드를 잠들게(sleep) 합니다.
   thread_unblock()에 의해 깨어날 때까지 다시 스케줄되지 않습니다.

   이 함수는 반드시 인터럽트가 꺼진 상태(off)에서 호출되어야 합니다.
   보통은 synch.h에 있는 동기화 프리미티브 중 하나를
   사용하는 것이 더 좋은 생각입니다. */
void thread_block(void)
{
	ASSERT(!intr_context());
	ASSERT(intr_get_level() == INTR_OFF);
	thread_current()->status = THREAD_BLOCKED;
	schedule();
}

/* 차단된(blocked) 스레드 T를 실행 준비(ready-to-run) 상태로 전환합니다.
   T가 차단된 상태가 아니라면 오류입니다. (실행 중인 스레드를
   준비 상태로 만들려면 thread_yield()를 사용하세요.)

   이 함수는 실행 중인 스레드를 선점하지 않습니다. 이는 중요할 수 있습니다:
   만약 호출자가 직접 인터럽트를 비활성화했다면,
   자신이 원자적으로 스레드의 차단을 해제하고 다른 데이터를
   업데이트할 수 있다고 예상할 수 있기 때문입니다. */
void thread_unblock(struct thread *t)
{
	enum intr_level old_level;

	ASSERT(is_thread(t));

	old_level = intr_disable();
	ASSERT(t->status == THREAD_BLOCKED);
	/* [Pintos] 프로젝트 1: 우선순위 스케줄링을 위해
	   list_push_back 대신 list_insert_ordered를 사용하고
	   선점(preemption) 로직을 추가해야 합니다. */
	list_push_back(&ready_list, &t->elem);
	t->status = THREAD_READY;
	intr_set_level(old_level);
}

/* 실행 중인 스레드의 이름을 반환합니다. */
const char * thread_name(void)
{
	return thread_current()->name;
}

/* 실행 중인 스레드를 반환합니다.
   running_thread()에 몇 가지 온전성 검사(sanity checks)를 더한 것입니다.
   자세한 내용은 thread.h 상단의 큰 주석을 참조하세요. */
struct thread * thread_current(void)
{
	struct thread *t = running_thread();

	/* T가 정말 스레드인지 확인합니다.
	   만약 이 단언(assertion) 중 하나라도 실패한다면,
	   스레드가 스택을 오버플로했을 수 있습니다.
	   각 스레드는 4KB 미만의 스택을 가지므로,
	   몇 개의 큰 자동 배열(automatic arrays)이나 중간 정도의
	   재귀(recursion)만으로도 스택 오버플로가 발생할 수 있습니다. */
	ASSERT(is_thread(t));
	ASSERT(t->status == THREAD_RUNNING);

	return t;
}

/* 실행 중인 스레드의 tid를 반환합니다. */
tid_t thread_tid(void)
{
	return thread_current()->tid;
}

/* 현재 스레드를 비스케줄(deschedule)하고 파괴합니다.
   절대 호출자에게 반환되지 않습니다. */
void thread_exit(void)
{
	ASSERT(!intr_context());

#ifdef USERPROG
	process_exit();
#endif

	/* 단순히 우리 상태를 'dying'으로 설정하고 다른 프로세스를
	   스케줄합니다. 우리는 schedule_tail() 호출 중에
	   파괴될 것입니다. */
	intr_disable();
	do_schedule(THREAD_DYING);
	NOT_REACHED();
}

/* CPU를 양보(yield)합니다. 현재 스레드는 잠들지 않으며(sleep)
   스케줄러의 판단에 따라 즉시 다시 스케줄될 수 있습니다. */
void thread_yield(void)
{
	struct thread *curr = thread_current();
	enum intr_level old_level;

	ASSERT(!intr_context());

	old_level = intr_disable();
	if (curr != idle_thread)
		/* [Pintos] 프로젝트 1: 우선순위 스케줄링을 위해
		   list_push_back 대신 list_insert_ordered를 사용해야 합니다. */
		list_push_back(&ready_list, &curr->elem);
	do_schedule(THREAD_READY);
	intr_set_level(old_level);
}

/* 현재 스레드의 우선순위를 NEW_PRIORITY로 설정합니다. */
void thread_set_priority(int new_priority)
{
	/* [Pintos] 프로젝트 1: 우선순위 기부(donation)를 고려하고
	   우선순위 변경 시 선점(preemption)을 처리해야 합니다. */
	thread_current()->priority = new_priority;
}

/* 현재 스레드의 우선순위를 반환합니다. */
int thread_get_priority(void)
{
	/* [Pintos] 프로젝트 1: 우선순위 기부(donation)를 고려하여
	   실제(effective) 우선순위를 반환해야 합니다. */
	return thread_current()->priority;
}

/* 현재 스레드의 nice 값을 NICE로 설정합니다. */
void thread_set_nice(int nice UNUSED)
{
	/* TODO: 여기에 구현을 추가하세요 */
}

/* 현재 스레드의 nice 값을 반환합니다. */
int thread_get_nice(void)
{
	/* TODO: 여기에 구현을 추가하세요 */
	return 0;
}

/* 시스템 로드 에버리지(load average)의 100배를 반환합니다. */
int thread_get_load_avg(void)
{
	/* TODO: 여기에 구현을 추가하세요 */
	return 0;
}

/* 현재 스레드의 recent_cpu 값의 100배를 반환합니다. */
int thread_get_recent_cpu(void)
{
	/* TODO: 여기에 구현을 추가하세요 */
	return 0;
}

/* 유휴(Idle) 스레드. 실행 준비가 된 다른 스레드가 없을 때 실행됩니다.

   유휴 스레드는 처음에 thread_start()에 의해 준비 리스트에
   추가됩니다. 처음 한 번 스케줄되면, idle_thread를 초기화하고,
   thread_start()가 계속 진행할 수 있도록 전달받은 세마포어를
   "up"한 다음, 즉시 차단(block)됩니다. 그 이후로
   유휴 스레드는 절대 준비 리스트에 나타나지 않습니다.
   준비 리스트가 비어 있을 때 next_thread_to_run()에 의해
   특별한 경우로 반환됩니다. */
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

		/* 인터럽트를 다시 활성화하고 다음 인터럽트를 기다립니다.

		   `sti` 명령어는 다음 명령어가 완료될 때까지
		   인터럽트를 비활성화하므로, 이 두 명령어는
		   원자적으로(atomically) 실행됩니다. 이 원자성은
		   중요합니다; 그렇지 않으면, 인터럽트 재활성화와
		   다음 인터럽트 대기 사이에 인터럽트가 처리될 수
		   있으며, 이 경우 최대 한 클럭 틱만큼의 시간을
		   낭비할 수 있습니다.

		   [IA32-v2a] "HLT", [IA32-v2b] "STI", 그리고 [IA32-v3a]
		   7.11.1 "HLT Instruction"을 참조하세요. */
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
static void init_thread(struct thread *t, const char *name, int priority)
{
	ASSERT(t != NULL);
	ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT(name != NULL);

	memset(t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy(t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t)t + PGSIZE - sizeof(void *);
	t->priority = priority;
	/* [Pintos] 프로젝트 1: 우선순위 스케줄링을 위해
	   base_priority 및 기부 관련 필드를 초기화해야 합니다. */
	/* [Pintos] 프로젝트 1: MLFQS를 위해
	   nice와 recent_cpu 필드를 초기화해야 합니다. */
	t->magic = THREAD_MAGIC;
}

/* 다음에 스케줄될 스레드를 선택하여 반환합니다.
   실행 큐(run queue)에서 스레드를 반환해야 하며, 실행 큐가
   비어있지 않은 이상 그렇게 해야 합니다. (만약 실행 중인 스레드가
   계속 실행될 수 있다면, 그 스레드는 실행 큐에 있을 것입니다.)
   실행 큐가 비어있다면, idle_thread를 반환합니다. */
static struct thread * next_thread_to_run(void)
{
	if (list_empty(&ready_list))
		return idle_thread;
	else
		/* [Pintos] 프로젝트 1: 우선순위 스케줄링을 위해
		   list_pop_front 대신 list_pop_back을 사용해야 합니다.
		   (list_insert_ordered를 사용할 경우 list_pop_front) */
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

/* 새 스레드의 페이지 테이블을 활성화하여 스레드를 전환하고,
   만약 이전 스레드가 죽는 중(dying)이라면, 파괴합니다.

   이 함수가 호출될 때, 우리는 방금 PREV 스레드에서
   전환(switch)했으며, 새 스레드는 이미 실행 중이고,
   인터럽트는 여전히 비활성화된 상태입니다.

   스레드 전환이 완료될 때까지 printf()를 호출하는 것은
   안전하지 않습니다. 실제로는 이 함수의 끝 부분에
   printf()를 추가해야 함을 의미합니다. */
static void thread_launch(struct thread *th)
{
	uint64_t tf_cur = (uint64_t)&running_thread()->tf;
	uint64_t tf = (uint64_t)&th->tf;
	ASSERT(intr_get_level() == INTR_OFF);

	/* 주된 전환 로직(switching logic).
	 * 먼저 전체 실행 컨텍스트를 intr_frame으로 복원한 다음
	 * do_iret을 호출하여 다음 스레드로 전환합니다.
	 * 참고로, 전환이 완료될 때까지 여기부터는 어떠한
	 * 스택도 사용해서는 안 됩니다. */
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
		"movw %%cs, 8(%%rax)\n"	 // cs
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

/* 새 프로세스를 스케줄합니다. 진입 시, 인터럽트는 반드시 꺼져(off) 있어야 합니다.
 * 이 함수는 현재 스레드의 상태를 status로 수정한 다음
 * 실행할 다른 스레드를 찾아 그 스레드로 전환합니다.
 * schedule() 안에서 printf()를 호출하는 것은 안전하지 않습니다. */
static void do_schedule(int status)
{
	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(thread_current()->status == THREAD_RUNNING);
	while (!list_empty(&destruction_req))
	{
		struct thread *victim =
			list_entry(list_pop_front(&destruction_req), struct thread, elem);
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
		/* 만약 우리가 전환해 나온 스레드가 죽는 중(dying)이라면,
		   그 struct thread를 파괴합니다. 이 작업은
		   thread_exit()이 스스로를 곤경에 빠뜨리지 않도록
		   늦게 일어나야 합니다.
		   페이지가 현재 스택으로 사용되고 있기 때문에
		   우리는 여기서 페이지 해제 요청을 큐에 넣기만 합니다.
		   실제 파괴 로직은 schedule()의 시작 부분에서
		   호출될 것입니다. */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread)
		{
			ASSERT(curr != next);
			list_push_back(&destruction_req, &curr->elem);
		}

		/* 스레드를 전환하기 전에, 먼저 현재 실행 중인 정보의
		   정보를 저장합니다. */
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