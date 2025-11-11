#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* 8254 타이머 칩에 대한 하드웨어 세부 정보는 [8254] 문서를 참조하세요. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* OS 부팅 이후 경과한 타이머 틱 수. */
static int64_t ticks;

/* 타이머 틱당 루프 수.
   timer_calibrate()에 의해 초기화됩니다. */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops(unsigned loops);
static void busy_wait(int64_t loops);
static void real_time_sleep(int64_t num, int32_t denom);


/* 8254 프로그래머블 인터벌 타이머(PIT)를 설정하여
   초당 TIMER_FREQ 횟수만큼 인터럽트를 발생시키고,
   해당 인터럽트를 등록합니다. */
void timer_init(void)
{
	/* 8254 입력 주파수를 TIMER_FREQ로 나눈 값 (가장 가까운 값으로 반올림). */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb(0x43, 0x34); /* CW(Control Word): 카운터 0, LSB 다음 MSB, 모드 2, 바이너리. */
	outb(0x40, count & 0xff);
	outb(0x40, count >> 8);

	intr_register_ext(0x20, timer_interrupt, "8254 Timer");
}

/* 짧은 지연(delay)을 구현하는 데 사용되는 loops_per_tick을 보정(calibrate)합니다. */
void timer_calibrate(void)
{
	unsigned high_bit, test_bit;

	ASSERT(intr_get_level() == INTR_ON);
	printf("Calibrating timer...  ");

	/* loops_per_tick을 1 타이머 틱보다 작은
	   가장 큰 2의 거듭제곱 값으로 근사화합니다. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops(loops_per_tick << 1))
	{
		loops_per_tick <<= 1;
		ASSERT(loops_per_tick != 0);
	}

	/* loops_per_tick의 다음 8비트를 세밀하게 조정합니다. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops(high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf("%'" PRIu64 " loops/s.\n", (uint64_t)loops_per_tick * TIMER_FREQ);
}

/* OS 부팅 이후 경과한 타이머 틱 수를 반환합니다. */
int64_t timer_ticks(void)
{
	enum intr_level old_level = intr_disable();
	int64_t t = ticks;
	intr_set_level(old_level);
	barrier();
	return t;
}

/* THEN (이전에 timer_ticks()가 반환했던 값) 이후로
   경과한 타이머 틱 수를 반환합니다. */
int64_t timer_elapsed(int64_t then)
{
	return timer_ticks() - then;
}

/* 약 TICKS (횟수)의 타이머 틱 동안 실행을 일시 중단합니다. */
void timer_sleep (int64_t ticks) {
    if (ticks <= 0) {
        return;
    }

    thread_sleep_until(timer_ticks() + ticks);
}

/* 약 MS 밀리초 동안 실행을 일시 중단합니다. */
void timer_msleep(int64_t ms)
{
	real_time_sleep(ms, 1000);
}

/* 약 US 마이크로초 동안 실행을 일시 중단합니다. */
void timer_usleep(int64_t us)
{
	real_time_sleep(us, 1000 * 1000);
}

/* 약 NS 나노초 동안 실행을 일시 중단합니다. */
void timer_nsleep(int64_t ns)
{
	real_time_sleep(ns, 1000 * 1000 * 1000);
}

/* 타이머 통계를 출력합니다. */
void timer_print_stats(void)
{
	printf("Timer: %" PRId64 " ticks\n", timer_ticks());
}

/* 타이머 인터럽트 핸들러. */
static void timer_interrupt(struct intr_frame *args UNUSED)
{
	ticks++;
	
    thread_tick();

    thread_wakeup(ticks);
}


/* LOOPS 횟수만큼 반복(iteration)하는 것이 1 타이머 틱보다
    오래 걸리면 true를, 그렇지 않으면 false를 반환합니다. */
static bool too_many_loops(unsigned loops)
{
	/* 타이머 틱을 기다립니다. */
	int64_t start = ticks;
	while (ticks == start)
		barrier();

	/* LOOPS 횟수만큼 루프를 실행합니다. */
	start = ticks;
	busy_wait(loops);

	/* 틱 카운트가 변경되었다면, 너무 오래 반복한 것입니다. */
	barrier();
	return start != ticks;
}

/* 짧은 지연(delay)을 구현하기 위해 단순한 루프를
   LOOPS 횟수만큼 반복합니다.

   NO_INLINE으로 표시된 이유: 코드 정렬(alignment)이 타이밍에
   큰 영향을 미칠 수 있으므로, 만약 이 함수가 다른 위치에서
   다르게 인라인(inline)된다면 결과를 예측하기
   어려울 것입니다. */
static void NO_INLINE busy_wait(int64_t loops)
{
	while (loops-- > 0)
		barrier();
}

/* 약 NUM/DENOM 초 동안 잠듭니다(sleep). */
static void real_time_sleep(int64_t num, int32_t denom)
{
	/* NUM/DENOM 초를 타이머 틱으로 변환합니다 (버림).

		  (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM 틱.
	   1 s / TIMER_FREQ 틱
	*/
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT(intr_get_level() == INTR_ON);
	if (ticks > 0)
	{
		/* 적어도 1 풀(full) 타이머 틱을 기다리는 경우입니다.
		   timer_sleep()을 사용하면 CPU를 다른 프로세스에게
		   양보(yield)할 것입니다. */
		timer_sleep(ticks);
	}
	else
	{
		/* 그렇지 않다면 (1틱 미만이라면), 더 정확한 서브틱(sub-tick)
		   타이밍을 위해 busy-wait 루프를 사용합니다.
		   오버플로 가능성을 피하기 위해 분자(numerator)와
		   분모(denominator)를 1000으로 나눕니다(scale down). */
		ASSERT(denom % 1000 == 0);
		busy_wait(loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}