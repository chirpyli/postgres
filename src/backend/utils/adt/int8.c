/*-------------------------------------------------------------------------
 *
 * int8.c
 *	  内部 64 位整数运算
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/int8.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>

#include "common/int.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"


typedef struct
{
	int64		current;
	int64		finish;
	int64		step;
} generate_series_fctx;


/***********************************************************************
 **
 **		64 位整数相关例程。
 **
 ***********************************************************************/

/*----------------------------------------------------------
 * 格式化与转换例程。
 *---------------------------------------------------------*/

/* int8in()
 */
Datum
int8in(PG_FUNCTION_ARGS)
{
	char	   *num = PG_GETARG_CSTRING(0);

	PG_RETURN_INT64(pg_strtoint64_safe(num, fcinfo->context));
}


/* int8out()
 */
Datum
int8out(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);
	char		buf[MAXINT8LEN + 1];
	char	   *result;
	int			len;

	len = pg_lltoa(val, buf) + 1;

	/*
	 * 由于长度已经已知，我们手动调用 palloc() 和 memcpy()，
	 * 以避免 pstrdup() 中本会被执行的 strlen() 调用。
	 */
	result = palloc(len);
	memcpy(result, buf, len);
	PG_RETURN_CSTRING(result);
}

/*
 *		int8recv			- 将外部二进制格式转换为 int8
 */
Datum
int8recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_INT64(pq_getmsgint64(buf));
}

/*
 *		int8send			- 将 int8 转换为二进制格式
 */
Datum
int8send(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*----------------------------------------------------------
 *	int8 的关系运算符，包括跨数据类型的比较。
 *---------------------------------------------------------*/

/* int8relop()
 * 判断 val1 relop val2 是否为真。
 */
Datum
int8eq(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 == val2);
}

Datum
int8ne(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 != val2);
}

Datum
int8lt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 < val2);
}

Datum
int8gt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 > val2);
}

Datum
int8le(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 <= val2);
}

Datum
int8ge(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 >= val2);
}

/* int84relop()
 * 判断 64 位的 val1 relop 32 位的 val2 是否为真。
 */
Datum
int84eq(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 == val2);
}

Datum
int84ne(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 != val2);
}

Datum
int84lt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 < val2);
}

Datum
int84gt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 > val2);
}

Datum
int84le(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 <= val2);
}

Datum
int84ge(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int32		val2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(val1 >= val2);
}

/* int48relop()
 * 判断 32 位的 val1 relop 64 位的 val2 是否为真。
 */
Datum
int48eq(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 == val2);
}

Datum
int48ne(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 != val2);
}

Datum
int48lt(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 < val2);
}

Datum
int48gt(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 > val2);
}

Datum
int48le(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 <= val2);
}

Datum
int48ge(PG_FUNCTION_ARGS)
{
	int32		val1 = PG_GETARG_INT32(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 >= val2);
}

/* int82relop()
 * 判断 64 位的 val1 relop 16 位的 val2 是否为真。
 */
Datum
int82eq(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 == val2);
}

Datum
int82ne(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 != val2);
}

Datum
int82lt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 < val2);
}

Datum
int82gt(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 > val2);
}

Datum
int82le(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 <= val2);
}

Datum
int82ge(PG_FUNCTION_ARGS)
{
	int64		val1 = PG_GETARG_INT64(0);
	int16		val2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(val1 >= val2);
}

/* int28relop()
 * 判断 16 位的 val1 relop 64 位的 val2 是否为真。
 */
Datum
int28eq(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 == val2);
}

Datum
int28ne(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 != val2);
}

Datum
int28lt(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 < val2);
}

Datum
int28gt(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 > val2);
}

Datum
int28le(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 <= val2);
}

Datum
int28ge(PG_FUNCTION_ARGS)
{
	int16		val1 = PG_GETARG_INT16(0);
	int64		val2 = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(val1 >= val2);
}

/*
 * int8 的 in_range 支持函数。
 *
 * 注意：我们无需提供 int8_int4 或 int8_int2 的变体，因为偏移量值的
 * 隐式类型转换同样能妥善处理这些场景。
 */
Datum
in_range_int8_int8(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);
	int64		base = PG_GETARG_INT64(1);
	int64		offset = PG_GETARG_INT64(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	int64		sum;

	if (offset < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	if (sub)
		offset = -offset;		/* 不会溢出 */

	if (unlikely(pg_add_s64_overflow(base, offset, &sum)))
	{
		/*
		 * 如果 sub 为假，真实的和必然大于 val，因此正确答案与"less"相同。
		 * 如果 sub 为真，真实的和必然小于 val，因此答案为"!less"。
		 */
		PG_RETURN_BOOL(sub ? !less : less);
	}

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}


/*----------------------------------------------------------
 *	64 位整数上的算术运算符。
 *---------------------------------------------------------*/

Datum
int8um(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);
	int64		result;

	if (unlikely(arg == PG_INT64_MIN))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	result = -arg;
	PG_RETURN_INT64(result);
}

Datum
int8up(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);

	PG_RETURN_INT64(arg);
}

Datum
int8pl(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_add_s64_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int8mi(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_sub_s64_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int8mul(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_mul_s64_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int8div(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (arg2 == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器意识到我们不能到达除法处（gcc 的一个 bug） */
		PG_RETURN_NULL();
	}

	/*
	 * INT64_MIN / -1 是有问题的，因为结果在补码机器上无法表示。
	 * 有些机器会产生 INT64_MIN，有些会产生零，还有些会抛出异常。
	 * 我们可以通过认识到"除以 -1"等价于取负来规避这个问题。
	 */
	if (arg2 == -1)
	{
		if (unlikely(arg1 == PG_INT64_MIN))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));
		result = -arg1;
		PG_RETURN_INT64(result);
	}

	/* 不可能发生溢出 */

	result = arg1 / arg2;

	PG_RETURN_INT64(result);
}

/* int8abs()
 * 绝对值
 */
Datum
int8abs(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		result;

	if (unlikely(arg1 == PG_INT64_MIN))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	result = (arg1 < 0) ? -arg1 : arg1;
	PG_RETURN_INT64(result);
}

/* int8mod()
 * 取模运算。
 */
Datum
int8mod(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);

	if (unlikely(arg2 == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器意识到我们不能到达除法处（gcc 的一个 bug） */
		PG_RETURN_NULL();
	}

	/*
	 * 某些机器在 INT64_MIN % -1 时会抛出浮点异常，这有些荒谬，
	 * 因为正确答案其实非常明确，就是零。
	 */
	if (arg2 == -1)
		PG_RETURN_INT64(0);

	/* 不可能发生溢出 */

	PG_RETURN_INT64(arg1 % arg2);
}

/*
 * 最大公约数（Greatest Common Divisor）
 *
 * 返回能同时整除两个输入的最大正整数。
 * 特殊情况：
 *   - gcd(x, 0) = gcd(0, x) = abs(x)
 *   		因为 0 可以被任何数整除
 *   - gcd(0, 0) = 0
 *   		符合上面的定义，且是一种通用约定
 *
 * 如果任一输入是 INT64_MIN，则必须特别小心 ---
 * gcd(0, INT64_MIN)、gcd(INT64_MIN, 0) 以及 gcd(INT64_MIN, INT64_MIN)
 * 都等于 abs(INT64_MIN)，而它无法用 64 位有符号整数表示。
 */
static int64
int8gcd_internal(int64 arg1, int64 arg2)
{
	int64		swap;
	int64		a1,
				a2;

	/*
	 * 将绝对值较大的放入 arg1。
	 *
	 * 这在下面循环中会自动发生，但这里先做一次可以避免
	 * 一次代价高昂的取模运算，并简化后面针对 INT64_MIN 的特殊处理。
	 *
	 * 我们在负数空间中进行处理，以便能正确处理 INT64_MIN。
	 */
	a1 = (arg1 < 0) ? arg1 : -arg1;
	a2 = (arg2 < 0) ? arg2 : -arg2;
	if (a1 > a2)
	{
		swap = arg1;
		arg1 = arg2;
		arg2 = swap;
	}

	/* 对于 INT64_MIN 需要特别小心。参见上面的注释。 */
	if (arg1 == PG_INT64_MIN)
	{
		if (arg2 == 0 || arg2 == PG_INT64_MIN)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));

		/*
		 * 某些机器在 INT64_MIN % -1 时会抛出浮点异常，这有些荒谬，
		 * 因为正确答案其实非常明确，就是零。这里加以防范并直接
		 * 返回结果，即 gcd(INT64_MIN, -1) = 1。
		 */
		if (arg2 == -1)
			return 1;
	}

	/* 使用欧几里得算法求最大公约数 */
	while (arg2 != 0)
	{
		swap = arg2;
		arg2 = arg1 % arg2;
		arg1 = swap;
	}

	/*
	 * 确保结果为正数。（我们知道此时已经不可能再是 INT64_MIN 了。）
	 */
	if (arg1 < 0)
		arg1 = -arg1;

	return arg1;
}

Datum
int8gcd(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	result = int8gcd_internal(arg1, arg2);

	PG_RETURN_INT64(result);
}

/*
 * 最小公倍数（Least Common Multiple）
 */
Datum
int8lcm(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		gcd;
	int64		result;

	/*
	 * 将 lcm(x, 0) = lcm(0, x) = 0 作为特殊情况处理。这可以避免
	 * 当 x 为零时下面出现除零错误，以及当 x = INT64_MIN 时
	 * 在 GCD 计算中出现溢出错误。
	 */
	if (arg1 == 0 || arg2 == 0)
		PG_RETURN_INT64(0);

	/* lcm(x, y) = abs(x / gcd(x, y) * y) */
	gcd = int8gcd_internal(arg1, arg2);
	arg1 = arg1 / gcd;

	if (unlikely(pg_mul_s64_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	/* 如果结果是 INT64_MIN，则它无法被表示。 */
	if (unlikely(result == PG_INT64_MIN))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	if (result < 0)
		result = -result;

	PG_RETURN_INT64(result);
}

Datum
int8inc(PG_FUNCTION_ARGS)
{
	/*
	 * 当 int8 是按引用传递时，我们提供这个特例以避免 COUNT() 的
	 * palloc 开销：当它作为聚合函数被调用时，我们知道参数是可修改的
	 * 本地存储，因此直接就地更新即可。
	 *（如果 int8 是按值传递的，那么这当然是既无用又错误的，
	 * 所以直接用 ifdef 将其排除。）
	 */
#ifndef USE_FLOAT8_BYVAL		/* 此处宏同时也控制着 int8 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		int64	   *arg = (int64 *) PG_GETARG_POINTER(0);

		if (unlikely(pg_add_s64_overflow(*arg, 1, arg)))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));

		PG_RETURN_POINTER(arg);
	}
	else
#endif
	{
		/* 不是作为聚合函数被调用，所以就用最朴素的方式处理 */
		int64		arg = PG_GETARG_INT64(0);
		int64		result;

		if (unlikely(pg_add_s64_overflow(arg, 1, &result)))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));

		PG_RETURN_INT64(result);
	}
}

Datum
int8dec(PG_FUNCTION_ARGS)
{
	/*
	 * 当 int8 是按引用传递时，我们提供这个特例以避免 COUNT() 的
	 * palloc 开销：当它作为聚合函数被调用时，我们知道参数是可修改的
	 * 本地存储，因此直接就地更新即可。
	 *（如果 int8 是按值传递的，那么这当然是既无用又错误的，
	 * 所以直接用 ifdef 将其排除。）
	 */
#ifndef USE_FLOAT8_BYVAL		/* 此处宏同时也控制着 int8 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		int64	   *arg = (int64 *) PG_GETARG_POINTER(0);

		if (unlikely(pg_sub_s64_overflow(*arg, 1, arg)))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));
		PG_RETURN_POINTER(arg);
	}
	else
#endif
	{
		/* 不是作为聚合函数被调用，所以就用最朴素的方式处理 */
		int64		arg = PG_GETARG_INT64(0);
		int64		result;

		if (unlikely(pg_sub_s64_overflow(arg, 1, &result)))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));

		PG_RETURN_INT64(result);
	}
}


/*
 * 这些函数与 int8inc/int8dec 完全相同，但用于只统计非 null 值的聚合。
 * 由于这些函数被声明为严格（strict）函数，对 null 的检查在我们到达这里
 * 之前就已经完成，我们只需要对状态值进行自增即可。实际上我们本可以让
 * 这些 pg_proc 条目直接指向 int8inc/int8dec，但那样的话 opr_sanity
 * 回归测试会抱怨内置函数的条目不匹配。
 */

Datum
int8inc_any(PG_FUNCTION_ARGS)
{
	return int8inc(fcinfo);
}

Datum
int8inc_float8_float8(PG_FUNCTION_ARGS)
{
	return int8inc(fcinfo);
}

Datum
int8dec_any(PG_FUNCTION_ARGS)
{
	return int8dec(fcinfo);
}

/*
 * int8inc_support
 *		int8inc() 与 int8inc_any() 的 prosupport 支持函数
 */
Datum
int8inc_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestWFuncMonotonic))
	{
		SupportRequestWFuncMonotonic *req = (SupportRequestWFuncMonotonic *) rawreq;
		MonotonicFunction monotonic = MONOTONICFUNC_NONE;
		int			frameOptions = req->window_clause->frameOptions;

		/* 没有 ORDER BY 子句时，所有行都是同辈 */
		if (req->window_clause->orderClause == NIL)
			monotonic = MONOTONICFUNC_BOTH;
		else
		{
			/*
			 * 否则要考虑窗口帧选项。当帧边界是窗口的起始位置时，
			 * 结果值永远不会减小，因此是单调递增的
			 */
			if (frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING)
				monotonic |= MONOTONICFUNC_INCREASING;

			/*
			 * 类似地，如果帧边界是窗口的结束位置，那么
			 * 结果值永远不会减小。
			 */
			if (frameOptions & FRAMEOPTION_END_UNBOUNDED_FOLLOWING)
				monotonic |= MONOTONICFUNC_DECREASING;
		}

		req->monotonic = monotonic;
		PG_RETURN_POINTER(req);
	}

	PG_RETURN_POINTER(NULL);
}


Datum
int8larger(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	result = ((arg1 > arg2) ? arg1 : arg2);

	PG_RETURN_INT64(result);
}

Datum
int8smaller(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	result = ((arg1 < arg2) ? arg1 : arg2);

	PG_RETURN_INT64(result);
}

Datum
int84pl(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int64		result;

	if (unlikely(pg_add_s64_overflow(arg1, (int64) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int84mi(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int64		result;

	if (unlikely(pg_sub_s64_overflow(arg1, (int64) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int84mul(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int64		result;

	if (unlikely(pg_mul_s64_overflow(arg1, (int64) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int84div(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int64		result;

	if (arg2 == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器意识到我们不能到达除法处（gcc 的一个 bug） */
		PG_RETURN_NULL();
	}

	/*
	 * INT64_MIN / -1 是有问题的，因为结果在补码机器上无法表示。
	 * 有些机器会产生 INT64_MIN，有些会产生零，还有些会抛出异常。
	 * 我们可以通过认识到"除以 -1"等价于取负来规避这个问题。
	 */
	if (arg2 == -1)
	{
		if (unlikely(arg1 == PG_INT64_MIN))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));
		result = -arg1;
		PG_RETURN_INT64(result);
	}

	/* 不可能发生溢出 */

	result = arg1 / arg2;

	PG_RETURN_INT64(result);
}

Datum
int48pl(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_add_s64_overflow((int64) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int48mi(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_sub_s64_overflow((int64) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int48mul(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_mul_s64_overflow((int64) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int48div(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int64		arg2 = PG_GETARG_INT64(1);

	if (unlikely(arg2 == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器意识到我们不能到达除法处（gcc 的一个 bug） */
		PG_RETURN_NULL();
	}

	/* 不可能发生溢出 */
	PG_RETURN_INT64((int64) arg1 / arg2);
}

Datum
int82pl(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int64		result;

	if (unlikely(pg_add_s64_overflow(arg1, (int64) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int82mi(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int64		result;

	if (unlikely(pg_sub_s64_overflow(arg1, (int64) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int82mul(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int64		result;

	if (unlikely(pg_mul_s64_overflow(arg1, (int64) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int82div(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int64		result;

	if (unlikely(arg2 == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器意识到我们不能到达除法处（gcc 的一个 bug） */
		PG_RETURN_NULL();
	}

	/*
	 * INT64_MIN / -1 是有问题的，因为结果在补码机器上无法表示。
	 * 有些机器会产生 INT64_MIN，有些会产生零，还有些会抛出异常。
	 * 我们可以通过认识到"除以 -1"等价于取负来规避这个问题。
	 */
	if (arg2 == -1)
	{
		if (unlikely(arg1 == PG_INT64_MIN))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));
		result = -arg1;
		PG_RETURN_INT64(result);
	}

	/* 不可能发生溢出 */

	result = arg1 / arg2;

	PG_RETURN_INT64(result);
}

Datum
int28pl(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_add_s64_overflow((int64) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int28mi(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_sub_s64_overflow((int64) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int28mul(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int64		arg2 = PG_GETARG_INT64(1);
	int64		result;

	if (unlikely(pg_mul_s64_overflow((int64) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(result);
}

Datum
int28div(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int64		arg2 = PG_GETARG_INT64(1);

	if (unlikely(arg2 == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器意识到我们不能到达除法处（gcc 的一个 bug） */
		PG_RETURN_NULL();
	}

	/* 不可能发生溢出 */
	PG_RETURN_INT64((int64) arg1 / arg2);
}

/* 二进制算术运算
 *
 *		int8and		- 返回 arg1 & arg2
 *		int8or		- 返回 arg1 | arg2
 *		int8xor		- 返回 arg1 # arg2
 *		int8not		- 返回 ~arg1
 *		int8shl		- 返回 arg1 << arg2
 *		int8shr		- 返回 arg1 >> arg2
 */

Datum
int8and(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);

	PG_RETURN_INT64(arg1 & arg2);
}

Datum
int8or(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);

	PG_RETURN_INT64(arg1 | arg2);
}

Datum
int8xor(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int64		arg2 = PG_GETARG_INT64(1);

	PG_RETURN_INT64(arg1 ^ arg2);
}

Datum
int8not(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);

	PG_RETURN_INT64(~arg1);
}

Datum
int8shl(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT64(arg1 << arg2);
}

Datum
int8shr(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT64(arg1 >> arg2);
}

/*----------------------------------------------------------
 *	转换运算符。
 *---------------------------------------------------------*/

Datum
int48(PG_FUNCTION_ARGS)
{
	int32		arg = PG_GETARG_INT32(0);

	PG_RETURN_INT64((int64) arg);
}

Datum
int84(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);

	if (unlikely(arg < PG_INT32_MIN) || unlikely(arg > PG_INT32_MAX))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT32((int32) arg);
}

Datum
int28(PG_FUNCTION_ARGS)
{
	int16		arg = PG_GETARG_INT16(0);

	PG_RETURN_INT64((int64) arg);
}

Datum
int82(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);

	if (unlikely(arg < PG_INT16_MIN) || unlikely(arg > PG_INT16_MAX))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	PG_RETURN_INT16((int16) arg);
}

Datum
i8tod(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);
	float8		result;

	result = arg;

	PG_RETURN_FLOAT8(result);
}

/* dtoi8()
 * 将 float8 转换为 8 字节整数。
 */
Datum
dtoi8(PG_FUNCTION_ARGS)
{
	float8		num = PG_GETARG_FLOAT8(0);

	/*
	 * 去掉输入中的小数部分。这样做是为了不让那些本可四舍五入进入
	 * 范围的、刚好超出范围的值失败。注意：这里假定 rint() 会
	 * 原样透传 NaN 或 Inf。
	 */
	num = rint(num);

	/* 范围检查 */
	if (unlikely(isnan(num) || !FLOAT8_FITS_IN_INT64(num)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	PG_RETURN_INT64((int64) num);
}

Datum
i8tof(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);
	float4		result;

	result = arg;

	PG_RETURN_FLOAT4(result);
}

/* ftoi8()
 * 将 float4 转换为 8 字节整数。
 */
Datum
ftoi8(PG_FUNCTION_ARGS)
{
	float4		num = PG_GETARG_FLOAT4(0);

	/*
	 * 去掉输入中的小数部分。这样做是为了不让那些本可四舍五入进入
	 * 范围的、刚好超出范围的值失败。注意：这里假定 rint() 会
	 * 原样透传 NaN 或 Inf。
	 */
	num = rint(num);

	/* 范围检查 */
	if (unlikely(isnan(num) || !FLOAT4_FITS_IN_INT64(num)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	PG_RETURN_INT64((int64) num);
}

Datum
i8tooid(PG_FUNCTION_ARGS)
{
	int64		arg = PG_GETARG_INT64(0);

	if (unlikely(arg < 0) || unlikely(arg > PG_UINT32_MAX))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("OID out of range")));

	PG_RETURN_OID((Oid) arg);
}

Datum
oidtoi8(PG_FUNCTION_ARGS)
{
	Oid			arg = PG_GETARG_OID(0);

	PG_RETURN_INT64((int64) arg);
}

/*
 * 非持久化的数值序列生成器
 */
Datum
generate_series_int8(PG_FUNCTION_ARGS)
{
	return generate_series_step_int8(fcinfo);
}

Datum
generate_series_step_int8(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	generate_series_fctx *fctx;
	int64		result;
	MemoryContext oldcontext;

	/* 仅在函数第一次被调用时执行的操作 */
	if (SRF_IS_FIRSTCALL())
	{
		int64		start = PG_GETARG_INT64(0);
		int64		finish = PG_GETARG_INT64(1);
		int64		step = 1;

		/* 检查是否显式指定了步长 */
		if (PG_NARGS() == 3)
			step = PG_GETARG_INT64(2);
		if (step == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("step size cannot equal zero")));

		/* 创建用于跨调用持久化的函数上下文 */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * 切换到适合多次函数调用的内存上下文
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* 为用户上下文分配内存 */
		fctx = (generate_series_fctx *) palloc(sizeof(generate_series_fctx));

		/*
		 * 用 fctx 在多次调用之间保存状态。将 current 初始化为
		 * 原始的起始值
		 */
		fctx->current = start;
		fctx->finish = finish;
		fctx->step = step;

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* 在函数的每一次调用中都要执行的操作 */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * 取出已保存的状态，并将 current 作为本次迭代的结果
	 */
	fctx = funcctx->user_fctx;
	result = fctx->current;

	if ((fctx->step > 0 && fctx->current <= fctx->finish) ||
		(fctx->step < 0 && fctx->current >= fctx->finish))
	{
		/*
		 * 自增 current 以准备下一次迭代。如果下一个值的
		 * 计算发生溢出，则这就是最终结果。
		 */
		if (pg_add_s64_overflow(fctx->current, fctx->step, &fctx->current))
			fctx->step = 0;

		/* 还有更多数据要发送时执行 */
		SRF_RETURN_NEXT(funcctx, Int64GetDatum(result));
	}
	else
		/* 没有更多数据要发送时执行 */
		SRF_RETURN_DONE(funcctx);
}

/*
 * generate_series(int8, int8 [, int8]) 的优化器支持函数
 */
Datum
generate_series_int8_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestRows))
	{
		/* 尝试估算返回的行的数量 */
		SupportRequestRows *req = (SupportRequestRows *) rawreq;

		if (is_funcclause(req->node))	/* 保持谨慎（be paranoid） */
		{
			List	   *args = ((FuncExpr *) req->node)->args;
			Node	   *arg1,
					   *arg2,
					   *arg3;

			/* 这里可以使用对参数的估值 */
			arg1 = estimate_expression_value(req->root, linitial(args));
			arg2 = estimate_expression_value(req->root, lsecond(args));
			if (list_length(args) >= 3)
				arg3 = estimate_expression_value(req->root, lthird(args));
			else
				arg3 = NULL;

			/*
			 * 如果任一参数是常量 NULL，我们可以安全地认为返回零行。
			 * 否则，如果它们都是非 NULL 的常量，我们就可以计算出
			 * 将要返回的行的数量。使用 double 算术以避免溢出风险。
			 */
			if ((IsA(arg1, Const) &&
				 ((Const *) arg1)->constisnull) ||
				(IsA(arg2, Const) &&
				 ((Const *) arg2)->constisnull) ||
				(arg3 != NULL && IsA(arg3, Const) &&
				 ((Const *) arg3)->constisnull))
			{
				req->rows = 0;
				ret = (Node *) req;
			}
			else if (IsA(arg1, Const) &&
					 IsA(arg2, Const) &&
					 (arg3 == NULL || IsA(arg3, Const)))
			{
				double		start,
							finish,
							step;

				start = DatumGetInt64(((Const *) arg1)->constvalue);
				finish = DatumGetInt64(((Const *) arg2)->constvalue);
				step = arg3 ? DatumGetInt64(((Const *) arg3)->constvalue) : 1;

				/* 这个等式对 step 的正负两种情况都适用 */
				if (step != 0)
				{
					req->rows = floor((finish - start + step) / step);
					ret = (Node *) req;
				}
			}
		}
	}

	PG_RETURN_POINTER(ret);
}
