/* threads/fixed-point.h */
#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

/* 17.14 고정소수점 형식을 사용합니다. f = 2^14 */
#define F (1 << 14)

/* 정수를 고정소수점으로 변환 */
#define INT_TO_FP(n) ((n) * F)

/* 고정소수점을 정수로 변환 (0에 가까운 쪽으로 버림) */
#define FP_TO_INT_TRUNC(x) ((x) / F)

/* 고정소수점을 정수로 변환 (가장 가까운 정수로 반올림) */
#define FP_TO_INT_ROUND(x) ((x) >= 0 ? ((x) + F / 2) / F : ((x) - F / 2) / F)

/* 고정소수점 덧셈 (x + y) */
#define FP_ADD(x, y) ((x) + (y))

/* 고정소수점 뺄셈 (x - y) */
#define FP_SUB(x, y) ((x) - (y))

/* 고정소수점 + 정수 (x + n) */
#define FP_ADD_MIXED(x, n) ((x) + (n) * F)

/* 고정소수점 - 정수 (x - n) */
#define FP_SUB_MIXED(x, n) ((x) - (n) * F)

/* 고정소수점 곱셈 (x * y) */
#define FP_MULT(x, y) (((int64_t)(x)) * (y) / F)

/* 고정소수점 * 정수 (x * n) */
#define FP_MULT_MIXED(x, n) ((x) * (n))

/* 고정소수점 나눗셈 (x / y) */
#define FP_DIV(x, y) (((int64_t)(x)) * F / (y))

/* 고정소수점 / 정수 (x / n) */
#define FP_DIV_MIXED(x, n) ((x) / (n))

#endif /* threads/fixed-point.h */