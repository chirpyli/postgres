/*-------------------------------------------------------------------------
 *
 * float.h
 *	  内建浮点类型的定义
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/include/utils/float.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FLOAT_H
#define FLOAT_H

#include <math.h>

/* X/Open (XSI) 要求 <math.h> 提供 M_PI，但核心 POSIX 并不要求 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 每度的弧度数，即 PI / 180 */
#define RADIANS_PER_DEGREE 0.0174532925199432957692

/* Visual C++ 等编译器缺少 NAN，而且不接受 0.0/0.0 这种写法。 */
#if defined(WIN32) && !defined(NAN)
static const uint32 nan[2] = {0xffffffff, 0x7fffffff};

#define NAN (*(const float8 *) nan)
#endif

extern PGDLLIMPORT int extra_float_digits;

/*
 * float.c 中的工具函数
 */
pg_noreturn extern void float_overflow_error(void);
pg_noreturn extern void float_underflow_error(void);
pg_noreturn extern void float_zero_divide_error(void);
extern int	is_infinite(float8 val);
extern float8 float8in_internal(char *num, char **endptr_p,
								const char *type_name, const char *orig_string,
								struct Node *escontext);
extern float4 float4in_internal(char *num, char **endptr_p,
								const char *type_name, const char *orig_string,
								struct Node *escontext);
extern char *float8out_internal(float8 num);
extern int	float4_cmp_internal(float4 a, float4 b);
extern int	float8_cmp_internal(float8 a, float8 b);

/*
 * 提供相对平台无关的无穷大（infinity）与 NaN 处理例程
 *
 * 我们假设 isinf() 与 isnan() 均可用，且符合规范
 * （在某些平台上，我们需要自行提供；参见 src/port）。
 * 然而，如何最开始就生成一个无穷大或 NaN，标准化程度要低得多；
 * 在 C99 之前，系统往往没有 C99 的 INFINITY 与 NaN 宏。
 * 我们把自己的变通写法集中放在这里。
 */

/*
 * 两个 #pragma 之所以放在这些奇怪的位置，是因为
 * Microsoft 编译器中一个长期存在的 bug 所致。
 * 详见 http://support.microsoft.com/kb/120968/en-us
 */
#ifdef _MSC_VER
#pragma warning(disable:4756)
#endif
static inline float4
get_float4_infinity(void)
{
#ifdef INFINITY
	/* C99 标准方式 */
	return (float4) INFINITY;
#else
#ifdef _MSC_VER
#pragma warning(default:4756)
#endif

	/*
	 * 在某些平台上，HUGE_VAL 是无穷大，而在另一些地方
	 * 它只是最大的常规 float8。我们假设强制产生一次溢出
	 * 就能得到真正的无穷大。
	 */
	return (float4) (HUGE_VAL * HUGE_VAL);
#endif
}

static inline float8
get_float8_infinity(void)
{
#ifdef INFINITY
	/* C99 标准方式 */
	return (float8) INFINITY;
#else

	/*
	 * 在某些平台上，HUGE_VAL 是无穷大，而在另一些地方
	 * 它只是最大的常规 float8。我们假设强制产生一次溢出
	 * 就能得到真正的无穷大。
	 */
	return (float8) (HUGE_VAL * HUGE_VAL);
#endif
}

static inline float4
get_float4_nan(void)
{
#ifdef NAN
	/* C99 标准方式 */
	return (float4) NAN;
#else
	/* 假设可以通过除以零得到 NaN */
	return (float4) (0.0 / 0.0);
#endif
}

static inline float8
get_float8_nan(void)
{
	/* (float8) NAN 在某些 NetBSD/MIPS 版本上不可用 */
#if defined(NAN) && !(defined(__NetBSD__) && defined(__mips__))
	/* C99 标准方式 */
	return (float8) NAN;
#else
	/* 假设可以通过除以零得到 NaN */
	return (float8) (0.0 / 0.0);
#endif
}

/*
 * 浮点算术，将溢出/下溢作为错误上报
 *
 * 对于加法/减法的下溢，没有任何办法可以检查，因为接近
 * 下溢阈值的数值已经被舍入到我们无法分辨二者原本不同的程度，
 * 例如在 x86 上，'1e-45'::float4 == '2e-45'::float4 == 1.4013e-45。
 */

static inline float4
float4_pl(const float4 val1, const float4 val2)
{
	float4		result;

	result = val1 + val2;
	if (unlikely(isinf(result)) && !isinf(val1) && !isinf(val2))
		float_overflow_error();

	return result;
}

static inline float8
float8_pl(const float8 val1, const float8 val2)
{
	float8		result;

	result = val1 + val2;
	if (unlikely(isinf(result)) && !isinf(val1) && !isinf(val2))
		float_overflow_error();

	return result;
}

static inline float4
float4_mi(const float4 val1, const float4 val2)
{
	float4		result;

	result = val1 - val2;
	if (unlikely(isinf(result)) && !isinf(val1) && !isinf(val2))
		float_overflow_error();

	return result;
}

static inline float8
float8_mi(const float8 val1, const float8 val2)
{
	float8		result;

	result = val1 - val2;
	if (unlikely(isinf(result)) && !isinf(val1) && !isinf(val2))
		float_overflow_error();

	return result;
}

static inline float4
float4_mul(const float4 val1, const float4 val2)
{
	float4		result;

	result = val1 * val2;
	if (unlikely(isinf(result)) && !isinf(val1) && !isinf(val2))
		float_overflow_error();
	if (unlikely(result == 0.0f) && val1 != 0.0f && val2 != 0.0f)
		float_underflow_error();

	return result;
}

static inline float8
float8_mul(const float8 val1, const float8 val2)
{
	float8		result;

	result = val1 * val2;
	if (unlikely(isinf(result)) && !isinf(val1) && !isinf(val2))
		float_overflow_error();
	if (unlikely(result == 0.0) && val1 != 0.0 && val2 != 0.0)
		float_underflow_error();

	return result;
}

static inline float4
float4_div(const float4 val1, const float4 val2)
{
	float4		result;

	if (unlikely(val2 == 0.0f) && !isnan(val1))
		float_zero_divide_error();
	result = val1 / val2;
	if (unlikely(isinf(result)) && !isinf(val1))
		float_overflow_error();
	if (unlikely(result == 0.0f) && val1 != 0.0f && !isinf(val2))
		float_underflow_error();

	return result;
}

static inline float8
float8_div(const float8 val1, const float8 val2)
{
	float8		result;

	if (unlikely(val2 == 0.0) && !isnan(val1))
		float_zero_divide_error();
	result = val1 / val2;
	if (unlikely(isinf(result)) && !isinf(val1))
		float_overflow_error();
	if (unlikely(result == 0.0) && val1 != 0.0 && !isinf(val2))
		float_underflow_error();

	return result;
}

/*
 * 识别 NaN 的比较例程
 *
 * 我们认为所有 NaN 彼此相等，并且都大于任何非 NaN 值。
 * 这在一定程度上是任意的；重要的是要保持一致的排序
 * 顺序。
 */

static inline bool
float4_eq(const float4 val1, const float4 val2)
{
	return isnan(val1) ? isnan(val2) : !isnan(val2) && val1 == val2;
}

static inline bool
float8_eq(const float8 val1, const float8 val2)
{
	return isnan(val1) ? isnan(val2) : !isnan(val2) && val1 == val2;
}

static inline bool
float4_ne(const float4 val1, const float4 val2)
{
	return isnan(val1) ? !isnan(val2) : isnan(val2) || val1 != val2;
}

static inline bool
float8_ne(const float8 val1, const float8 val2)
{
	return isnan(val1) ? !isnan(val2) : isnan(val2) || val1 != val2;
}

static inline bool
float4_lt(const float4 val1, const float4 val2)
{
	return !isnan(val1) && (isnan(val2) || val1 < val2);
}

static inline bool
float8_lt(const float8 val1, const float8 val2)
{
	return !isnan(val1) && (isnan(val2) || val1 < val2);
}

static inline bool
float4_le(const float4 val1, const float4 val2)
{
	return isnan(val2) || (!isnan(val1) && val1 <= val2);
}

static inline bool
float8_le(const float8 val1, const float8 val2)
{
	return isnan(val2) || (!isnan(val1) && val1 <= val2);
}

static inline bool
float4_gt(const float4 val1, const float4 val2)
{
	return !isnan(val2) && (isnan(val1) || val1 > val2);
}

static inline bool
float8_gt(const float8 val1, const float8 val2)
{
	return !isnan(val2) && (isnan(val1) || val1 > val2);
}

static inline bool
float4_ge(const float4 val1, const float4 val2)
{
	return isnan(val1) || (!isnan(val2) && val1 >= val2);
}

static inline bool
float8_ge(const float8 val1, const float8 val2)
{
	return isnan(val1) || (!isnan(val2) && val1 >= val2);
}

static inline float4
float4_min(const float4 val1, const float4 val2)
{
	return float4_lt(val1, val2) ? val1 : val2;
}

static inline float8
float8_min(const float8 val1, const float8 val2)
{
	return float8_lt(val1, val2) ? val1 : val2;
}

static inline float4
float4_max(const float4 val1, const float4 val2)
{
	return float4_gt(val1, val2) ? val1 : val2;
}

static inline float8
float8_max(const float8 val1, const float8 val2)
{
	return float8_gt(val1, val2) ? val1 : val2;
}

#endif							/* FLOAT_H */
