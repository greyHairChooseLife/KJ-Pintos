/* threads/synch.c */
#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* 세마포 SEMA를 VALUE로 초기화합니다. */
void sema_init(struct semaphore* sema, unsigned value) {
    ASSERT(sema != NULL);

    sema->value = value;
    list_init(&sema->waiters);
}

/* 세마포에 대한 Down 또는 "P" 연산입니다. */
void sema_down(struct semaphore* sema) {
    enum intr_level old_level;

    ASSERT(sema != NULL);
    ASSERT(!intr_context());

    old_level = intr_disable();
    while (sema->value == 0)
    {
        /* 우선순위 큐(waiters)에 정렬 삽입 */
        list_insert_ordered(&sema->waiters, &thread_current()->elem,
                            thread_priority_less_func, NULL);
        thread_block();
    }
    sema->value--;
    intr_set_level(old_level);
}

/* 세마포 값을 0이 아닐 때만 'down' 시도 (인터럽트 핸들러용) */
bool sema_try_down(struct semaphore* sema) {
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

/* 세마포에 대한 Up 또는 "V" 연산입니다. */
void sema_up(struct semaphore* sema) {
    enum intr_level old_level;

    ASSERT(sema != NULL);

    old_level = intr_disable();

    sema->value++;

    if (!list_empty(&sema->waiters))
    {
        struct list_elem* e =
            list_min(&sema->waiters, thread_priority_less_func, NULL);
        list_remove(e);
        struct thread* t = list_entry(e, struct thread, elem);
        thread_unblock(t);  // thread_unblock이 선점을 처리함
    }

    intr_set_level(old_level);
}

/* 세마포 자체 테스트 (이하 동일) */
static void sema_test_helper(void* sema_);
void sema_self_test(void) {
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
static void sema_test_helper(void* sema_) {
    struct semaphore* sema = sema_;
    int i;
    for (i = 0; i < 10; i++)
    {
        sema_down(&sema[0]);
        sema_up(&sema[1]);
    }
}

/* LOCK을 초기화합니다. */
void lock_init(struct lock* lock) {
    ASSERT(lock != NULL);

    lock->holder = NULL;
    sema_init(&lock->semaphore, 0);
    list_init(&lock->semaphore.waiters);
}

/* 'donation_elem'을 기준 내림차순 */
static bool donation_priority_less_func(const struct list_elem* a,
                                        const struct list_elem* b,
                                        void* aux UNUSED) {
    const struct thread* thread_a = list_entry(a, struct thread, donation_elem);
    const struct thread* thread_b = list_entry(b, struct thread, donation_elem);
    return thread_a->priority > thread_b->priority;
}

/* LOCK을 획득합니다. (sema_down을 사용하지 않고 재구현) */
void lock_acquire(struct lock* lock) {
    ASSERT(lock != NULL);
    ASSERT(!intr_context());
    ASSERT(!lock_held_by_current_thread(lock));

    struct thread* current = thread_current();
    enum intr_level old_level;

    old_level = intr_disable();

    if (lock->holder == NULL)
    {
        lock->holder = current;
    }
    else
    {
        if (!thread_mlfqs)
        {
            // 1. 내가 이 락을 기다린다고 기록
            current->waiting_for_lock = lock;

            // 2. 락 소유자의 'donations' 리스트에 나를 정렬 삽입
            list_insert_ordered(&lock->holder->donations,
                                &current->donation_elem,
                                thread_donation_less_func, NULL);

            // 3. 락 소유자(L)부터 연쇄적으로 우선순위 재계산(전파)
            struct thread* donee = lock->holder;
            while (donee != NULL)
            {
                thread_recalculate_priority(donee);
                donee = donee->waiting_for_lock
                            ? donee->waiting_for_lock->holder
                            : NULL;
            }
        }

        // 4. (공통) 락의 대기열에 정렬 삽입 후 잠들기
        list_insert_ordered(&lock->semaphore.waiters, &current->elem,
                            thread_priority_less_func, NULL);
        thread_block();

        lock->holder = current;

        if (!thread_mlfqs)
        {
            current->waiting_for_lock = NULL;
        }
    }

    intr_set_level(old_level);
}

/* LOCK 획득을 시도합니다 (non-blocking). */
bool lock_try_acquire(struct lock* lock) {
    bool success;
    enum intr_level old_level;

    ASSERT(lock != NULL);
    ASSERT(!lock_held_by_current_thread(lock));

    old_level = intr_disable();
    if (lock->holder == NULL)
    {
        lock->holder = thread_current();
        success = true;
    }
    else
    {
        success = false;
    }
    intr_set_level(old_level);

    return success;
}

/* LOCK을 해제합니다. (sema_up을 사용하지 않고 재구현) */
void lock_release(struct lock* lock) {
    ASSERT(lock != NULL);
    ASSERT(lock_held_by_current_thread(lock));

    struct thread* current = thread_current();
    enum intr_level old_level = intr_disable();

    if (!thread_mlfqs)
    {
        // 1. 기부자(donor) 리스트에서 이 락을 기다리던 애들 제거
        struct list_elem* e = list_begin(&current->donations);
        while (e != list_end(&current->donations))
        {
            struct thread* donor = list_entry(e, struct thread, donation_elem);
            if (donor->waiting_for_lock == lock)
            {
                e = list_remove(e);
            }
            else
            {
                e = list_next(e);
            }
        }

        // 2. 우선순위 재계산
        thread_recalculate_priority(current);
    }

    // 3. (공통) 다음 스레드 깨우기
    if (!list_empty(&lock->semaphore.waiters))
    {
        struct thread* t = list_entry(list_pop_front(&lock->semaphore.waiters),
                                      struct thread, elem);
        thread_unblock(t);
    }
    else
    {
        lock->holder = NULL;
    }

    intr_set_level(old_level);
}

/* 현재 스레드가 LOCK을 보유하고 있는지 확인합니다. */
bool lock_held_by_current_thread(const struct lock* lock) {
    ASSERT(lock != NULL);
    return lock->holder == thread_current();
}

/* 리스트 내의 세마포 하나. (cond_wait 정렬을 위해 priority 멤버 추가) */
struct semaphore_elem {
    struct list_elem elem;
    struct semaphore semaphore;
    int priority;
};

/* semaphore_elem의 우선순위 비교 */
static bool cond_waiter_less_func(const struct list_elem* a,
                                  const struct list_elem* b,
                                  void* aux UNUSED) {
    const struct semaphore_elem* se_a =
        list_entry(a, struct semaphore_elem, elem);
    const struct semaphore_elem* se_b =
        list_entry(b, struct semaphore_elem, elem);

    return se_a->priority > se_b->priority;
}

/* 조건 변수(condition variable) COND를 초기화합니다. */
void cond_init(struct condition* cond) {
    ASSERT(cond != NULL);

    list_init(&cond->waiters);
}

/* 원자적으로 LOCK을 해제하고 COND SIGNAL를 기다립니다. */
void cond_wait(struct condition* cond, struct lock* lock) {
    struct semaphore_elem waiter;

    ASSERT(cond != NULL);
    ASSERT(lock != NULL);
    ASSERT(!intr_context());
    ASSERT(lock_held_by_current_thread(lock));

    sema_init(&waiter.semaphore, 0);

    waiter.priority = thread_current()->priority;
    list_insert_ordered(&cond->waiters, &waiter.elem, cond_waiter_less_func,
                        NULL);

    /* * lock_release()는 내부에서 우선순위가 낮아질 경우
     * thread_yield()를 호출할 수 있으므로,
     * 그 전에 스레드가 대기열에 들어가 있어야 합니다.
     */
    lock_release(lock);
    sema_down(&waiter.semaphore);
    lock_acquire(lock);
}

/* COND에서 대기 중인 스레드 중 하나(가장 우선순위가 높은)에게 신호를 보냅니다.
 */
void cond_signal(struct condition* cond, struct lock* lock UNUSED) {
    ASSERT(cond != NULL);
    ASSERT(lock != NULL);
    ASSERT(!intr_context());
    ASSERT(lock_held_by_current_thread(lock));

    if (!list_empty(&cond->waiters))
    {
        struct list_elem* e = list_pop_front(&cond->waiters);

        sema_up(&list_entry(e, struct semaphore_elem, elem)->semaphore);
    }
}

/* COND에서 대기 중인 모든 스레드를 깨웁니다. (우선순위 순으로) */
void cond_broadcast(struct condition* cond, struct lock* lock) {
    ASSERT(cond != NULL);
    ASSERT(lock != NULL);

    while (!list_empty(&cond->waiters)) cond_signal(cond, lock);
}