/* 이 파일은 교육용 운영 체제인 Nachos의 소스 코드에서
   파생되었습니다. Nachos 저작권 고지는 아래에
   전체 내용을 복제합니다. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* 세마포 SEMA를 VALUE로 초기화합니다. 세마포는
   음이 아닌 정수(nonnegative integer)와 이를 조작하기 위한
   두 가지 원자적 연산자(atomic operators)로 구성됩니다:

   - down 또는 "P": 값이 양수가 되기를 기다렸다가,
	 값을 1 감소시킵니다.

   - up 또는 "V": 값을 1 증가시킵니다 (그리고 만약 대기 중인
	 스레드가 있다면, 그중 하나를 깨웁니다). */
void sema_init(struct semaphore *sema, unsigned value)
{
	ASSERT(sema != NULL);

	sema->value = value;
	list_init(&sema->waiters);
}

/* 세마포에 대한 Down 또는 "P" 연산입니다. SEMA의 값이
   양수가 되기를 기다린 다음, 원자적으로 값을 1 감소시킵니다.

   이 함수는 잠들 수 있으므로(may sleep), 인터럽트 핸들러
   내에서 호출되어서는 안 됩니다. 이 함수는 인터럽트가
   비활성화된 상태에서 호출될 수 있지만, 만약 잠들게 되면
   다음에 스케줄되는 스레드가 아마도 인터럽트를 다시
   활성화할 것입니다. 이 함수는 sema_down 함수입니다. */
void sema_down(struct semaphore *sema)
{
	enum intr_level old_level;

	ASSERT(sema != NULL);
	ASSERT(!intr_context());

	old_level = intr_disable();
	while (sema->value == 0)
	{
		/* [Pintos] 프로젝트 1에서는 우선순위 스케줄링을 위해
		   list_push_back 대신 list_insert_ordered를 사용해야 합니다. */
		list_push_back(&sema->waiters, &thread_current()->elem);
		thread_block();
	}
	sema->value--;
	intr_set_level(old_level);
}

/* 세마포에 대한 Down 또는 "P" 연산이지만, 세마포가
   이미 0이 아닐 경우에만 수행합니다. 세마포가 성공적으로
   감소되면 true를 반환하고, 그렇지 않으면 false를 반환합니다.

   이 함수는 인터럽트 핸들러 내에서 호출될 수 있습니다. */
bool sema_try_down(struct semaphore *sema)
{
	enum intr_level old_level;
	bool success;

	ASSERT(sema != NULL);

	old_level = intr_disable();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level(old_level);

	return success;
}

/* 세마포에 대한 Up 또는 "V" 연산입니다. SEMA의 값을
   증가시키고, SEMA를 기다리는 스레드가 있다면 그중
   하나를 깨웁니다.

   이 함수는 인터럽트 핸들러 내에서 호출될 수 있습니다. */
void sema_up(struct semaphore *sema)
{
	enum intr_level old_level;

	ASSERT(sema != NULL);

	old_level = intr_disable();
	if (!list_empty(&sema->waiters))
		/* [Pintos] 프로젝트 1에서는 우선순위 스케줄링을 위해
		   list_pop_front 대신 list_pop_back을 사용해야 합니다.
		   (list_insert_ordered를 사용할 경우 list_pop_front) */
		thread_unblock(list_entry(list_pop_front(&sema->waiters),
								  struct thread, elem));
	sema->value++;
	intr_set_level(old_level);
}

static void sema_test_helper(void *sema_);

/* 두 스레드 간에 제어가 "핑퐁(ping-pong)"처럼 오가는
   세마포 자체 테스트(self-test)입니다.
   무슨 일이 일어나는지 보려면 printf() 호출을 삽입해 보세요. */
void sema_self_test(void)
{
	struct semaphore sema[2];
	int i;

	printf("Testing semaphores...");
	sema_init(&sema[0], 0);
	sema_init(&sema[1], 0);
	thread_create("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up(&sema[0]);
		sema_down(&sema[1]);
	}
	printf("done.\n");
}

/* sema_self_test()에 의해 사용되는 스레드 함수입니다. */
static void sema_test_helper(void *sema_)
{
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down(&sema[0]);
		sema_up(&sema[1]);
	}
}

/* LOCK을 초기화합니다. 락(lock)은 특정 시점에 최대
   하나의 스레드에 의해서만 보유될 수 있습니다. 우리의 락은
   "재귀적(recursive)"이지 않습니다. 즉, 현재 락을 보유한
   스레드가 해당 락을 다시 획득하려고 시도하면 오류입니다.

   락은 초기값이 1인 세마포를 특수화한 것입니다.
   락과 이러한 세마포의 차이점은 두 가지입니다.
   첫째, 세마포는 1보다 큰 값을 가질 수 있지만, 락은
   한 번에 하나의 스레드만 소유할 수 있습니다.
   둘째, 세마포는 소유자(owner) 개념이 없습니다.
   즉, 한 스레드가 세마포를 "down"하고 다른 스레드가
   "up"할 수 있습니다. 하지만 락은 획득(acquire)한
   스레드와 해제(release)하는 스레드가 동일해야 합니다.
   이러한 제약이 과도하다고 판단될 때는, 락 대신
   세마포를 사용해야 한다는 좋은 신호입니다. */
void lock_init(struct lock *lock)
{
	ASSERT(lock != NULL);

	lock->holder = NULL;
	sema_init(&lock->semaphore, 1);
}

/* LOCK을 획득합니다. 필요하다면 락이 사용 가능해질 때까지
   잠들면서 기다립니다. 락은 현재 스레드에 의해 이미
   보유되어서는 안 됩니다.

   이 함수는 잠들 수 있으므로, 인터럽트 핸들러 내에서
   호출되어서는 안 됩니다. 이 함수는 인터럽트가 비활성화된
   상태에서 호출될 수 있지만, 만약 잠들어야 한다면
   인터럽트가 다시 켜질 것입니다. */
void lock_acquire(struct lock *lock)
{
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(!lock_held_by_current_thread(lock));

	/* [Pintos] 프로젝트 1: 여기에 우선순위 기부 로직을 추가해야 합니다. */
	sema_down(&lock->semaphore);
	lock->holder = thread_current();
}

/* LOCK 획득을 시도하고, 성공하면 true를, 실패하면 false를
   반환합니다. 락은 현재 스레드에 의해 이미 보유되어서는
   안 됩니다.

   이 함수는 잠들지 않으므로, 인터럽트 핸들러 내에서
   호출될 수 있습니다. */
bool lock_try_acquire(struct lock *lock)
{
	bool success;

	ASSERT(lock != NULL);
	ASSERT(!lock_held_by_current_thread(lock));

	success = sema_try_down(&lock->semaphore);
	if (success)
		lock->holder = thread_current();
	return success;
}

/* 현재 스레드가 소유하고 있어야 하는 LOCK을 해제합니다.
   이 함수는 lock_release 함수입니다.

   인터럽트 핸들러는 락을 획득할 수 없으므로, 인터럽트
   핸들러 내에서 락을 해제하려는 시도도 말이 되지 않습니다. */
void lock_release(struct lock *lock)
{
	ASSERT(lock != NULL);
	ASSERT(lock_held_by_current_thread(lock));

	/* [Pintos] 프로젝트 1: 여기에 우선순위 기부 회수 로직을 추가해야 합니다. */
	lock->holder = NULL;
	sema_up(&lock->semaphore);
}

/* 현재 스레드가 LOCK을 보유하고 있으면 true를,
   그렇지 않으면 false를 반환합니다. (참고: 다른 스레드가
   락을 보유하고 있는지 테스트하는 것은 경쟁 조건(racy)을
   유발할 수 있습니다.) */
bool lock_held_by_current_thread(const struct lock *lock)
{
	ASSERT(lock != NULL);

	return lock->holder == thread_current();
}

/* 리스트 내의 세마포 하나. */
struct semaphore_elem
{
	struct list_elem elem;		/* 리스트 요소. */
	struct semaphore semaphore; /* 이 세마포. */
};

/* 조건 변수(condition variable) COND를 초기화합니다.
   조건 변수는 코드의 한 부분이 어떤 조건을 신호(signal)로
   알리고, 협력하는 다른 코드가 그 신호를 받아 동작할 수
   있도록 합니다. */
void cond_init(struct condition *cond)
{
	ASSERT(cond != NULL);

	list_init(&cond->waiters);
}

/* 원자적으로 LOCK을 해제하고, 다른 코드 조각에 의해 COND가
   신호를 받을 때까지 기다립니다. COND가 신호를 받은 후에는,
   반환하기 전에 LOCK을 다시 획득합니다.
   이 함수를 호출하기 전에 LOCK이 반드시 보유되어 있어야 합니다.

   이 함수에 의해 구현된 모니터는 "Hoare" 스타일이 아닌
   "Mesa" 스타일입니다. 즉, 신호를 보내고 받는 것이
   원자적 연산이 아닙니다. 따라서, 일반적으로 호출자는
   wait가 완료된 후 조건을 다시 확인하고, 필요하다면
   다시 기다려야 합니다.

   주어진 조건 변수는 오직 단 하나의 락과 연관되지만,
   하나의 락은 여러 개의 조건 변수와 연관될 수 있습니다.
   즉, 락에서 조건 변수로의 일대다(one-to-many) 매핑이
   있습니다.

   이 함수는 잠들 수 있으므로, 인터럽트 핸들러 내에서
   호출되어서는 안 됩니다. 이 함수는 인터럽트가 비활성화된
   상태에서 호출될 수 있지만, 만약 잠들어야 한다면
   인터럽트가 다시 켜질 것입니다. */
void cond_wait(struct condition *cond, struct lock *lock)
{
	struct semaphore_elem waiter;

	ASSERT(cond != NULL);
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(lock_held_by_current_thread(lock));

	sema_init(&waiter.semaphore, 0);
	/* [Pintos] 프로젝트 1: 우선순위 스케줄링을 위해
	   list_push_back 대신 list_insert_ordered를 사용해야 합니다. */
	list_push_back(&cond->waiters, &waiter.elem);
	lock_release(lock);
	sema_down(&waiter.semaphore);
	lock_acquire(lock);
}

/* 만약 (LOCK으로 보호되는) COND에서 대기 중인 스레드가 있다면,
   이 함수는 그들 중 하나에게 깨어나라는 신호를 보냅니다.
   이 함수를 호출하기 전에 LOCK이 반드시 보유되어 있어야 합니다.

   인터럽트 핸들러는 락을 획득할 수 없으므로, 인터럽트
   핸들러 내에서 조건 변수에 신호를 보내려는 시도도
   말이 되지 않습니다. */
void cond_signal(struct condition *cond, struct lock *lock UNUSED)
{
	ASSERT(cond != NULL);
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(lock_held_by_current_thread(lock));

	if (!list_empty(&cond->waiters))
		/* [Pintos] 프로젝트 1: 우선순위 스케줄링을 위해
		   list_pop_front 대신 list_pop_back을 사용해야 합니다.
		   (list_insert_ordered를 사용할 경우 list_pop_front) */
		sema_up(&list_entry(list_pop_front(&cond->waiters),
							struct semaphore_elem, elem)
					 ->semaphore);
}

/* (LOCK으로 보호되는) COND에서 대기 중인 모든 스레드를
   (만약 있다면) 깨웁니다.
   이 함수를 호출하기 전에 LOCK이 반드시 보유되어 있어야 합니다.

   인터럽트 핸들러는 락을 획득할 수 없으므로, 인터럽트
   핸들러 내에서 조건 변수에 신호를 보내려는 시도도
   말이 되지 않습니다. */
void cond_broadcast(struct condition *cond, struct lock *lock)
{
	ASSERT(cond != NULL);
	ASSERT(lock != NULL);

	while (!list_empty(&cond->waiters))
		cond_signal(cond, lock);
}