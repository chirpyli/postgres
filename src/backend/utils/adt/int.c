/*-------------------------------------------------------------------------
 *
 * int.c
 *	  内置整数类型（除 int8 外）的函数。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/int.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 旧注释
 *		输入输出例程：
 *		 int2in, int2out, int2recv, int2send
 *		 int4in, int4out, int4recv, int4send
 *		 int2vectorin, int2vectorout, int2vectorrecv, int2vectorsend
 *		布尔操作符：
 *		 inteq, intne, intlt, intle, intgt, intge
 *		算术操作符：
 *		 intpl, intmi, int4mul, intdiv
 *
 *		算术操作符：
 *		 intmod
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>

#include "catalog/pg_type.h"
#include "common/int.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/builtins.h"

#define Int2VectorSize(n)	(offsetof(int2vector, values) + (n) * sizeof(int16))

typedef struct
{
	int32		current;
	int32		finish;
	int32		step;
} generate_series_fctx;


/*****************************************************************************
 *	 用户输入输出例程														 *
 *****************************************************************************/

/*
 *		int2in			- 将 "num" 转换为 short
 */
Datum
int2in(PG_FUNCTION_ARGS)
{
	char	   *num = PG_GETARG_CSTRING(0);

	PG_RETURN_INT16(pg_strtoint16_safe(num, fcinfo->context));
}

/*
 *		int2out			- 将 short 转换为 "num"
 */
Datum
int2out(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	char	   *result = (char *) palloc(7);	/* 符号位、5 位数字、'\0' */

	pg_itoa(arg1, result);
	PG_RETURN_CSTRING(result);
}

/*
 *		int2recv			- 将外部二进制格式转换为 int2
 */
Datum
int2recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_INT16((int16) pq_getmsgint(buf, sizeof(int16)));
}

/*
 *		int2send			- 将 int2 转换为二进制格式
 */
Datum
int2send(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint16(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * 根据原始 int2 数组构建 int2vector
 *
 * 如果 int2s 为 NULL，则调用者必须在之后自己填充 values[]
 */
int2vector *
buildint2vector(const int16 *int2s, int n)
{
	int2vector *result;

	result = (int2vector *) palloc0(Int2VectorSize(n));

	if (n > 0 && int2s)
		memcpy(result->values, int2s, n * sizeof(int16));

	/*
	 * 附加标准数组头。出于历史原因，我们将索引下界设为 0 而非 1。
	 */
	SET_VARSIZE(result, Int2VectorSize(n));
	result->ndim = 1;
	result->dataoffset = 0;		/* 永远没有 null */
	result->elemtype = INT2OID;
	result->dim1 = n;
	result->lbound1 = 0;

	return result;
}

/*
 * 验证数组对象满足 int2vector 的限制
 *
 * 我们需要此函数，因为存在某些路径可以将通用的 int2[] 数组转换为
 * int2vector，从而可能违反该类型的限制。所有接收 int2vector 作为
 * SQL 参数的代码都应检查这一点。
 */
static void
check_valid_int2vector(const int2vector *int2Array)
{
	/*
	 * 我们坚持要求 ndim == 1 且 dataoffset == 0（即无 null 值），
	 * 否则数组布局将不符合调用代码的预期。不过，我们不必对索引下界
	 * 过于挑剔。检查 elemtype 仅是出于谨慎。
	 */
	if (int2Array->ndim != 1 ||
		int2Array->dataoffset != 0 ||
		int2Array->elemtype != INT2OID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("array is not a valid int2vector")));
}

/*
 *		int2vectorin			- 将 "num num ..." 转换为内部形式
 */
Datum
int2vectorin(PG_FUNCTION_ARGS)
{
	char	   *intString = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	int2vector *result;
	int			nalloc;
	int			n;

	nalloc = 32;				/* 任意初始大小猜测值 */
	result = (int2vector *) palloc0(Int2VectorSize(nalloc));

	for (n = 0;; n++)
	{
		long		l;
		char	   *endp;

		while (*intString && isspace((unsigned char) *intString))
			intString++;
		if (*intString == '\0')
			break;

		if (n >= nalloc)
		{
			nalloc *= 2;
			result = (int2vector *) repalloc(result, Int2VectorSize(nalloc));
		}

		errno = 0;
		l = strtol(intString, &endp, 10);

		if (intString == endp)
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s: \"%s\"",
							"smallint", intString)));

		if (errno == ERANGE || l < SHRT_MIN || l > SHRT_MAX)
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("value \"%s\" is out of range for type %s", intString,
							"smallint")));

		if (*endp && *endp != ' ')
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s: \"%s\"",
							"smallint", intString)));

		result->values[n] = l;
		intString = endp;
	}

	SET_VARSIZE(result, Int2VectorSize(n));
	result->ndim = 1;
	result->dataoffset = 0;		/* 永远没有 null */
	result->elemtype = INT2OID;
	result->dim1 = n;
	result->lbound1 = 0;

	PG_RETURN_POINTER(result);
}

/*
 *		int2vectorout		- 将内部形式转换为 "num num ..."
 */
Datum
int2vectorout(PG_FUNCTION_ARGS)
{
	int2vector *int2Array = (int2vector *) PG_GETARG_POINTER(0);
	int			num,
				nnums;
	char	   *rp;
	char	   *result;

	/* 在获取 dim1 之前校验输入 */
	check_valid_int2vector(int2Array);
	nnums = int2Array->dim1;

	/* 假定符号位、5 位数字、空格 */
	rp = result = (char *) palloc(nnums * 7 + 1);
	for (num = 0; num < nnums; num++)
	{
		if (num != 0)
			*rp++ = ' ';
		rp += pg_itoa(int2Array->values[num], rp);
	}
	*rp = '\0';
	PG_RETURN_CSTRING(result);
}

/*
 *		int2vectorrecv			- 将外部二进制格式转换为 int2vector
 */
Datum
int2vectorrecv(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(locfcinfo, 3);
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int2vector *result;

	/*
	 * 通常可以通过 DirectFunctionCall3 调用 array_recv()，但这行不通，
	 * 因为 array_recv 需要使用 fcinfo->flinfo->fn_extra 来缓存一些数据。
	 * 因此，我们需要向其传递自己的 flinfo 参数。
	 */
	InitFunctionCallInfoData(*locfcinfo, fcinfo->flinfo, 3,
							 InvalidOid, NULL, NULL);

	locfcinfo->args[0].value = PointerGetDatum(buf);
	locfcinfo->args[0].isnull = false;
	locfcinfo->args[1].value = ObjectIdGetDatum(INT2OID);
	locfcinfo->args[1].isnull = false;
	locfcinfo->args[2].value = Int32GetDatum(-1);
	locfcinfo->args[2].isnull = false;

	result = (int2vector *) DatumGetPointer(array_recv(locfcinfo));

	Assert(!locfcinfo->isnull);

	/* 合理性校验：int2vector 必须为 1 维、0 起始、无 null */
	if (ARR_NDIM(result) != 1 ||
		ARR_HASNULL(result) ||
		ARR_ELEMTYPE(result) != INT2OID ||
		ARR_LBOUND(result)[0] != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid int2vector data")));

	PG_RETURN_POINTER(result);
}

/*
 *		int2vectorsend			- 将 int2vector 转换为二进制格式
 */
Datum
int2vectorsend(PG_FUNCTION_ARGS)
{
	/* 我们不调用 check_valid_int2vector，因为 array_send 不关心这些 */
	return array_send(fcinfo);
}


/*****************************************************************************
 *	 公共例程																 *
 *****************************************************************************/

/*
 *		int4in			- 将 "num" 转换为 int4
 */
Datum
int4in(PG_FUNCTION_ARGS)
{
	char	   *num = PG_GETARG_CSTRING(0);

	PG_RETURN_INT32(pg_strtoint32_safe(num, fcinfo->context));
}

/*
 *		int4out			- 将 int4 转换为 "num"
 */
Datum
int4out(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	char	   *result = (char *) palloc(12);	/* 符号位、10 位数字、'\0' */

	pg_ltoa(arg1, result);
	PG_RETURN_CSTRING(result);
}

/*
 *		int4recv			- 将外部二进制格式转换为 int4
 */
Datum
int4recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_INT32((int32) pq_getmsgint(buf, sizeof(int32)));
}

/*
 *		int4send			- 将 int4 转换为二进制格式
 */
Datum
int4send(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint32(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*
 *		===================
 *		类型转换例程
 *		===================
 */

Datum
i2toi4(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);

	PG_RETURN_INT32((int32) arg1);
}

Datum
i4toi2(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);

	if (unlikely(arg1 < SHRT_MIN) || unlikely(arg1 > SHRT_MAX))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	PG_RETURN_INT16((int16) arg1);
}

/* 将 int4 转换为 bool */
Datum
int4_bool(PG_FUNCTION_ARGS)
{
	if (PG_GETARG_INT32(0) == 0)
		PG_RETURN_BOOL(false);
	else
		PG_RETURN_BOOL(true);
}

/* 将 bool 转换为 int4 */
Datum
bool_int4(PG_FUNCTION_ARGS)
{
	if (PG_GETARG_BOOL(0) == false)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(1);
}

/*
 *		============================
 *		比较操作符例程
 *		============================
 */

/*
 *		inteq			- 当 arg1 == arg2 时返回 1
 *		intne			- 当 arg1 != arg2 时返回 1
 *		intlt			- 当 arg1 < arg2 时返回 1
 *		intle			- 当 arg1 <= arg2 时返回 1
 *		intgt			- 当 arg1 > arg2 时返回 1
 *		intge			- 当 arg1 >= arg2 时返回 1
 */

Datum
int4eq(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
int4ne(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
int4lt(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 < arg2);
}

Datum
int4le(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 <= arg2);
}

Datum
int4gt(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 > arg2);
}

Datum
int4ge(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 >= arg2);
}

Datum
int2eq(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
int2ne(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
int2lt(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 < arg2);
}

Datum
int2le(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 <= arg2);
}

Datum
int2gt(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 > arg2);
}

Datum
int2ge(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 >= arg2);
}

Datum
int24eq(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
int24ne(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
int24lt(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 < arg2);
}

Datum
int24le(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 <= arg2);
}

Datum
int24gt(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 > arg2);
}

Datum
int24ge(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_BOOL(arg1 >= arg2);
}

Datum
int42eq(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 == arg2);
}

Datum
int42ne(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 != arg2);
}

Datum
int42lt(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 < arg2);
}

Datum
int42le(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 <= arg2);
}

Datum
int42gt(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 > arg2);
}

Datum
int42ge(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_BOOL(arg1 >= arg2);
}


/*----------------------------------------------------------
 *	int4 和 int2 的 in_range 函数，包括跨数据类型比较。
 *
 *	注：出于性能原因，我们单独提供 intN_int8 函数。这也强制
 *	我们必须提供 intN_int2，否则使用 smallint 偏移值的情况将
 *	无法确定应使用哪个函数。但这是不太可能出现的情况，
 *	因此不为此重复代码。
 *---------------------------------------------------------*/

Datum
in_range_int4_int4(PG_FUNCTION_ARGS)
{
	int32		val = PG_GETARG_INT32(0);
	int32		base = PG_GETARG_INT32(1);
	int32		offset = PG_GETARG_INT32(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	int32		sum;

	if (offset < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	if (sub)
		offset = -offset;		/* 不可能溢出 */

	if (unlikely(pg_add_s32_overflow(base, offset, &sum)))
	{
		/*
		 * 如果 sub 为 false，则真实的和一定大于 val，因此正确答案
		 * 与 "less" 相同。如果 sub 为 true，则真实的和一定小于 val，
		 * 因此答案为 "!less"。
		 */
		PG_RETURN_BOOL(sub ? !less : less);
	}

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}

Datum
in_range_int4_int2(PG_FUNCTION_ARGS)
{
	/* 似乎不值得为此重复代码，直接调用 int4_int4 */
	return DirectFunctionCall5(in_range_int4_int4,
							   PG_GETARG_DATUM(0),
							   PG_GETARG_DATUM(1),
							   Int32GetDatum((int32) PG_GETARG_INT16(2)),
							   PG_GETARG_DATUM(3),
							   PG_GETARG_DATUM(4));
}

Datum
in_range_int4_int8(PG_FUNCTION_ARGS)
{
	/* 我们必须使用 int64 来完成所有数学运算 */
	int64		val = (int64) PG_GETARG_INT32(0);
	int64		base = (int64) PG_GETARG_INT32(1);
	int64		offset = PG_GETARG_INT64(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	int64		sum;

	if (offset < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	if (sub)
		offset = -offset;		/* 不可能溢出 */

	if (unlikely(pg_add_s64_overflow(base, offset, &sum)))
	{
		/*
		 * 如果 sub 为 false，则真实的和一定大于 val，因此正确答案
		 * 与 "less" 相同。如果 sub 为 true，则真实的和一定小于 val，
		 * 因此答案为 "!less"。
		 */
		PG_RETURN_BOOL(sub ? !less : less);
	}

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}

Datum
in_range_int2_int4(PG_FUNCTION_ARGS)
{
	/* 我们必须使用 int32 来完成所有数学运算 */
	int32		val = (int32) PG_GETARG_INT16(0);
	int32		base = (int32) PG_GETARG_INT16(1);
	int32		offset = PG_GETARG_INT32(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	int32		sum;

	if (offset < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	if (sub)
		offset = -offset;		/* 不可能溢出 */

	if (unlikely(pg_add_s32_overflow(base, offset, &sum)))
	{
		/*
		 * 如果 sub 为 false，则真实的和一定大于 val，因此正确答案
		 * 与 "less" 相同。如果 sub 为 true，则真实的和一定小于 val，
		 * 因此答案为 "!less"。
		 */
		PG_RETURN_BOOL(sub ? !less : less);
	}

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}

Datum
in_range_int2_int2(PG_FUNCTION_ARGS)
{
	/* 似乎不值得为此重复代码，直接调用 int2_int4 */
	return DirectFunctionCall5(in_range_int2_int4,
							   PG_GETARG_DATUM(0),
							   PG_GETARG_DATUM(1),
							   Int32GetDatum((int32) PG_GETARG_INT16(2)),
							   PG_GETARG_DATUM(3),
							   PG_GETARG_DATUM(4));
}

Datum
in_range_int2_int8(PG_FUNCTION_ARGS)
{
	/* 似乎不值得为此重复代码，直接调用 int4_int8 */
	return DirectFunctionCall5(in_range_int4_int8,
							   Int32GetDatum((int32) PG_GETARG_INT16(0)),
							   Int32GetDatum((int32) PG_GETARG_INT16(1)),
							   PG_GETARG_DATUM(2),
							   PG_GETARG_DATUM(3),
							   PG_GETARG_DATUM(4));
}


/*
 *		int[24]pl		- 返回 arg1 + arg2
 *		int[24]mi		- 返回 arg1 - arg2
 *		int[24]mul		- 返回 arg1 * arg2
 *		int[24]div		- 返回 arg1 / arg2
 */

Datum
int4um(PG_FUNCTION_ARGS)
{
	int32		arg = PG_GETARG_INT32(0);

	if (unlikely(arg == PG_INT32_MIN))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(-arg);
}

Datum
int4up(PG_FUNCTION_ARGS)
{
	int32		arg = PG_GETARG_INT32(0);

	PG_RETURN_INT32(arg);
}

Datum
int4pl(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	if (unlikely(pg_add_s32_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int4mi(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	if (unlikely(pg_sub_s32_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int4mul(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	if (unlikely(pg_mul_s32_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int4div(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	if (arg2 == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器知道我们不会到达除法语句（gcc bug） */
		PG_RETURN_NULL();
	}

	/*
	 * INT_MIN / -1 是个问题，因为其结果无法在补码机器上表示。
	 * 有些机器会返回 INT_MIN，有些返回零，有些会抛出异常。
	 * 我们可以通过认识到除以 -1 等同于取负来规避这个问题。
	 */
	if (arg2 == -1)
	{
		if (unlikely(arg1 == PG_INT32_MIN))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
		result = -arg1;
		PG_RETURN_INT32(result);
	}

	/* 不可能溢出 */

	result = arg1 / arg2;

	PG_RETURN_INT32(result);
}

Datum
int4inc(PG_FUNCTION_ARGS)
{
	int32		arg = PG_GETARG_INT32(0);
	int32		result;

	if (unlikely(pg_add_s32_overflow(arg, 1, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT32(result);
}

Datum
int2um(PG_FUNCTION_ARGS)
{
	int16		arg = PG_GETARG_INT16(0);

	if (unlikely(arg == PG_INT16_MIN))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));
	PG_RETURN_INT16(-arg);
}

Datum
int2up(PG_FUNCTION_ARGS)
{
	int16		arg = PG_GETARG_INT16(0);

	PG_RETURN_INT16(arg);
}

Datum
int2pl(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int16		result;

	if (unlikely(pg_add_s16_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));
	PG_RETURN_INT16(result);
}

Datum
int2mi(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int16		result;

	if (unlikely(pg_sub_s16_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));
	PG_RETURN_INT16(result);
}

Datum
int2mul(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int16		result;

	if (unlikely(pg_mul_s16_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	PG_RETURN_INT16(result);
}

Datum
int2div(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int16		result;

	if (arg2 == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器知道我们不会到达除法语句（gcc bug） */
		PG_RETURN_NULL();
	}

	/*
	 * SHRT_MIN / -1 是个问题，因为其结果无法在补码机器上表示。
	 * 有些机器会返回 SHRT_MIN，有些返回零，有些会抛出异常。
	 * 我们可以通过认识到除以 -1 等同于取负来规避这个问题。
	 */
	if (arg2 == -1)
	{
		if (unlikely(arg1 == PG_INT16_MIN))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("smallint out of range")));
		result = -arg1;
		PG_RETURN_INT16(result);
	}

	/* 不可能溢出 */

	result = arg1 / arg2;

	PG_RETURN_INT16(result);
}

Datum
int24pl(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	if (unlikely(pg_add_s32_overflow((int32) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int24mi(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	if (unlikely(pg_sub_s32_overflow((int32) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int24mul(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	if (unlikely(pg_mul_s32_overflow((int32) arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int24div(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	if (unlikely(arg2 == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器知道我们不会到达除法语句（gcc bug） */
		PG_RETURN_NULL();
	}

	/* 不可能溢出 */
	PG_RETURN_INT32((int32) arg1 / arg2);
}

Datum
int42pl(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int32		result;

	if (unlikely(pg_add_s32_overflow(arg1, (int32) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int42mi(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int32		result;

	if (unlikely(pg_sub_s32_overflow(arg1, (int32) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int42mul(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int32		result;

	if (unlikely(pg_mul_s32_overflow(arg1, (int32) arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	PG_RETURN_INT32(result);
}

Datum
int42div(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16		arg2 = PG_GETARG_INT16(1);
	int32		result;

	if (unlikely(arg2 == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器知道我们不会到达除法语句（gcc bug） */
		PG_RETURN_NULL();
	}

	/*
	 * INT_MIN / -1 是个问题，因为其结果无法在补码机器上表示。
	 * 有些机器会返回 INT_MIN，有些返回零，有些会抛出异常。
	 * 我们可以通过认识到除以 -1 等同于取负来规避这个问题。
	 */
	if (arg2 == -1)
	{
		if (unlikely(arg1 == PG_INT32_MIN))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
		result = -arg1;
		PG_RETURN_INT32(result);
	}

	/* 不可能溢出 */

	result = arg1 / arg2;

	PG_RETURN_INT32(result);
}

Datum
int4mod(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	if (unlikely(arg2 == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器知道我们不会到达除法语句（gcc bug） */
		PG_RETURN_NULL();
	}

	/*
	 * 某些机器对 INT_MIN % -1 会抛出浮点异常，这有点多余，
	 * 因为正确答案——零——是完全明确定义的。
	 */
	if (arg2 == -1)
		PG_RETURN_INT32(0);

	/* 不可能溢出 */

	PG_RETURN_INT32(arg1 % arg2);
}

Datum
int2mod(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	if (unlikely(arg2 == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
		/* 确保编译器知道我们不会到达除法语句（gcc bug） */
		PG_RETURN_NULL();
	}

	/*
	 * 某些机器对 INT_MIN % -1 会抛出浮点异常，这有点多余，
	 * 因为正确答案——零——是完全明确定义的。
	 * （在处理 int16 时不太清楚是否真的会发生这种情况，
	 * 但为安全起见，我们还是加上这个检测。）
	 */
	if (arg2 == -1)
		PG_RETURN_INT16(0);

	/* 不可能溢出 */

	PG_RETURN_INT16(arg1 % arg2);
}


/* int[24]abs()
 * 绝对值
 */
Datum
int4abs(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		result;

	if (unlikely(arg1 == PG_INT32_MIN))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));
	result = (arg1 < 0) ? -arg1 : arg1;
	PG_RETURN_INT32(result);
}

Datum
int2abs(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		result;

	if (unlikely(arg1 == PG_INT16_MIN))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));
	result = (arg1 < 0) ? -arg1 : arg1;
	PG_RETURN_INT16(result);
}

/*
 * 最大公约数
 *
 * 返回能同时整除两个输入的最大正整数。
 * 特殊情况：
 *   - gcd(x, 0) = gcd(0, x) = abs(x)
 *   		因为 0 可以被任何数整除
 *   - gcd(0, 0) = 0
 *   		遵循前一条定义，并且是一个通用约定
 *
 * 如果任一输入为 INT_MIN，则需特别小心——gcd(0, INT_MIN)、
 * gcd(INT_MIN, 0) 和 gcd(INT_MIN, INT_MIN) 都等于 abs(INT_MIN)，
 * 但该值无法表示为 32 位有符号整数。
 */
static int32
int4gcd_internal(int32 arg1, int32 arg2)
{
	int32		swap;
	int32		a1,
				a2;

	/*
	 * 将绝对值较大的数放在 arg1 中。
	 *
	 * 这在下面的循环中会自动发生，但此处提前处理可以避免昂贵的
	 * 取模运算，并简化下面对 INT_MIN 的特殊情况处理。
	 *
	 * 我们在负数空间中完成此操作，以便能够处理 INT_MIN。
	 */
	a1 = (arg1 < 0) ? arg1 : -arg1;
	a2 = (arg2 < 0) ? arg2 : -arg2;
	if (a1 > a2)
	{
		swap = arg1;
		arg1 = arg2;
		arg2 = swap;
	}

	/* 需要特别小心 INT_MIN。参见上方注释。 */
	if (arg1 == PG_INT32_MIN)
	{
		if (arg2 == 0 || arg2 == PG_INT32_MIN)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));

		/*
		 * 某些机器对 INT_MIN % -1 会抛出浮点异常，这有点多余，
		 * 因为正确答案——零——是完全明确定义的。
		 * 防范此情况，直接返回结果 gcd(INT_MIN, -1) = 1。
		 */
		if (arg2 == -1)
			return 1;
	}

	/* 使用欧几里得算法求 GCD */
	while (arg2 != 0)
	{
		swap = arg2;
		arg2 = arg1 % arg2;
		arg1 = swap;
	}

	/*
	 * 确保结果为正数。（我们知道此时已不再有 INT_MIN）。
	 */
	if (arg1 < 0)
		arg1 = -arg1;

	return arg1;
}

Datum
int4gcd(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		result;

	result = int4gcd_internal(arg1, arg2);

	PG_RETURN_INT32(result);
}

/*
 * 最小公倍数
 */
Datum
int4lcm(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);
	int32		gcd;
	int32		result;

	/*
	 * 将 lcm(x, 0) = lcm(0, x) = 0 作为特殊情况处理。
	 * 这可以避免 x 为零时下面的除零错误，以及 x = INT_MIN 时
	 * GCD 计算中的溢出错误。
	 */
	if (arg1 == 0 || arg2 == 0)
		PG_RETURN_INT32(0);

	/* lcm(x, y) = abs(x / gcd(x, y) * y) */
	gcd = int4gcd_internal(arg1, arg2);
	arg1 = arg1 / gcd;

	if (unlikely(pg_mul_s32_overflow(arg1, arg2, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	/* 如果结果为 INT_MIN，则无法表示。 */
	if (unlikely(result == PG_INT32_MIN))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	if (result < 0)
		result = -result;

	PG_RETURN_INT32(result);
}

Datum
int2larger(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_INT16((arg1 > arg2) ? arg1 : arg2);
}

Datum
int2smaller(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_INT16((arg1 < arg2) ? arg1 : arg2);
}

Datum
int4larger(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT32((arg1 > arg2) ? arg1 : arg2);
}

Datum
int4smaller(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT32((arg1 < arg2) ? arg1 : arg2);
}

/*
 * 位操作操作符
 *
 *		int[24]and		- 返回 arg1 & arg2
 *		int[24]or		- 返回 arg1 | arg2
 *		int[24]xor		- 返回 arg1 ^ arg2
 *		int[24]not		- 返回 ~arg1
 *		int[24]shl		- 返回 arg1 << arg2
 *		int[24]shr		- 返回 arg1 >> arg2
 */

Datum
int4and(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT32(arg1 & arg2);
}

Datum
int4or(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT32(arg1 | arg2);
}

Datum
int4xor(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT32(arg1 ^ arg2);
}

Datum
int4shl(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT32(arg1 << arg2);
}

Datum
int4shr(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT32(arg1 >> arg2);
}

Datum
int4not(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);

	PG_RETURN_INT32(~arg1);
}

Datum
int2and(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_INT16(arg1 & arg2);
}

Datum
int2or(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_INT16(arg1 | arg2);
}

Datum
int2xor(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16		arg2 = PG_GETARG_INT16(1);

	PG_RETURN_INT16(arg1 ^ arg2);
}

Datum
int2not(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);

	PG_RETURN_INT16(~arg1);
}


Datum
int2shl(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT16(arg1 << arg2);
}

Datum
int2shr(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int32		arg2 = PG_GETARG_INT32(1);

	PG_RETURN_INT16(arg1 >> arg2);
}

/*
 * 非持久化数值序列生成器
 */
Datum
generate_series_int4(PG_FUNCTION_ARGS)
{
	return generate_series_step_int4(fcinfo);
}

Datum
generate_series_step_int4(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	generate_series_fctx *fctx;
	int32		result;
	MemoryContext oldcontext;

	/* 仅在函数首次调用时执行的工作 */
	if (SRF_IS_FIRSTCALL())
	{
		int32		start = PG_GETARG_INT32(0);
		int32		finish = PG_GETARG_INT32(1);
		int32		step = 1;

		/* 检查是否给出了显式的步长 */
		if (PG_NARGS() == 3)
			step = PG_GETARG_INT32(2);
		if (step == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("step size cannot equal zero")));

		/* 创建函数上下文以支持跨调用持久化 */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * 切换到适合多次函数调用的内存上下文
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* 为用户上下文分配内存 */
		fctx = (generate_series_fctx *) palloc(sizeof(generate_series_fctx));

		/*
		 * 使用 fctx 在多次调用间保持状态。将 current 初始化为
		 * 原始的起始值
		 */
		fctx->current = start;
		fctx->finish = finish;
		fctx->step = step;

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* 每次函数调用都执行的工作 */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * 获取保存的状态，并使用 current 作为本轮迭代的结果
	 */
	fctx = funcctx->user_fctx;
	result = fctx->current;

	if ((fctx->step > 0 && fctx->current <= fctx->finish) ||
		(fctx->step < 0 && fctx->current >= fctx->finish))
	{
		/*
		 * 递增 current 以为下一次迭代做准备。如果下一次值的计算溢出，
		 * 则本轮即为最终结果。
		 */
		if (pg_add_s32_overflow(fctx->current, fctx->step, &fctx->current))
			fctx->step = 0;

		/* 当还有更多结果要返回时执行 */
		SRF_RETURN_NEXT(funcctx, Int32GetDatum(result));
	}
	else
		/* 当没有更多结果时执行 */
		SRF_RETURN_DONE(funcctx);
}

/*
 * generate_series(int4, int4 [, int4]) 的规划器支持函数
 */
Datum
generate_series_int4_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestRows))
	{
		/* 尝试估算返回的行数 */
		SupportRequestRows *req = (SupportRequestRows *) rawreq;

		if (is_funcclause(req->node))	/* 谨慎起见 */
		{
			List	   *args = ((FuncExpr *) req->node)->args;
			Node	   *arg1,
					   *arg2,
					   *arg3;

			/* 这里可以使用估算的参数值 */
			arg1 = estimate_expression_value(req->root, linitial(args));
			arg2 = estimate_expression_value(req->root, lsecond(args));
			if (list_length(args) >= 3)
				arg3 = estimate_expression_value(req->root, lthird(args));
			else
				arg3 = NULL;

			/*
			 * 如果任何参数是常量 NULL，可以安全地假设返回零行。
			 * 否则，如果所有参数都是非 NULL 常量，可以计算返回的行数。
			 * 使用 double 算术以避免溢出风险。
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

				start = DatumGetInt32(((Const *) arg1)->constvalue);
				finish = DatumGetInt32(((Const *) arg2)->constvalue);
				step = arg3 ? DatumGetInt32(((Const *) arg3)->constvalue) : 1;

				/* 这个公式对 step 的正负号均适用 */
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
