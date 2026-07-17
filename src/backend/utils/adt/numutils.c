/*-------------------------------------------------------------------------
 *
 * numutils.c
 *	  内置数值类型 I/O 的实用工具函数。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/numutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <limits.h>
#include <ctype.h>

#include "common/int.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"

/*
 * 一张包含所有两位数字的表。通过将成对的数字复制到最终输出中，
 * 来加速十进制数字的生成。
 */
static const char DIGIT_TABLE[200] =
"00" "01" "02" "03" "04" "05" "06" "07" "08" "09"
"10" "11" "12" "13" "14" "15" "16" "17" "18" "19"
"20" "21" "22" "23" "24" "25" "26" "27" "28" "29"
"30" "31" "32" "33" "34" "35" "36" "37" "38" "39"
"40" "41" "42" "43" "44" "45" "46" "47" "48" "49"
"50" "51" "52" "53" "54" "55" "56" "57" "58" "59"
"60" "61" "62" "63" "64" "65" "66" "67" "68" "69"
"70" "71" "72" "73" "74" "75" "76" "77" "78" "79"
"80" "81" "82" "83" "84" "85" "86" "87" "88" "89"
"90" "91" "92" "93" "94" "95" "96" "97" "98" "99";

/*
 * 改编自 http://graphics.stanford.edu/~seander/bithacks.html#IntegerLog10
 */
static inline int
decimalLength32(const uint32 v)
{
	int			t;
	static const uint32 PowersOfTen[] = {
		1, 10, 100,
		1000, 10000, 100000,
		1000000, 10000000, 100000000,
		1000000000
	};

	/*
	 * 用 2 为底的对数除以一个对 10 的 2 为底对数足够好的近似值，
	 * 来计算 10 为底的对数。
	 */
	t = (pg_leftmost_one_pos32(v) + 1) * 1233 / 4096;
	return t + (v >= PowersOfTen[t]);
}

static inline int
decimalLength64(const uint64 v)
{
	int			t;
	static const uint64 PowersOfTen[] = {
		UINT64CONST(1), UINT64CONST(10),
		UINT64CONST(100), UINT64CONST(1000),
		UINT64CONST(10000), UINT64CONST(100000),
		UINT64CONST(1000000), UINT64CONST(10000000),
		UINT64CONST(100000000), UINT64CONST(1000000000),
		UINT64CONST(10000000000), UINT64CONST(100000000000),
		UINT64CONST(1000000000000), UINT64CONST(10000000000000),
		UINT64CONST(100000000000000), UINT64CONST(1000000000000000),
		UINT64CONST(10000000000000000), UINT64CONST(100000000000000000),
		UINT64CONST(1000000000000000000), UINT64CONST(10000000000000000000)
	};

	/*
	 * 用 2 为底的对数除以一个对 10 的 2 为底对数足够好的近似值，
	 * 来计算 10 为底的对数。
	 */
	t = (pg_leftmost_one_pos64(v) + 1) * 1233 / 4096;
	return t + (v >= PowersOfTen[t]);
}

static const int8 hexlookup[128] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

/*
 * 将输入字符串转换为有符号的 16 位整数。输入字符串可以用十进制、
 * 十六进制、八进制或二进制格式表达，所有这些格式都可以带一个可选的符号
 * 字符前缀，'\+'（默认）表示正数，'-' 表示负数。十六进制字符串通过数字
 * 前缀 0x 或 0X 来识别，八进制字符串通过 0o 或 0O 前缀识别，二进制表示
 * 通过 0b 或 0B 前缀识别。
 *
 * 允许任意数量的前导或尾随空白字符。数字之间可以用单个下划线字符分隔。
 * 下划线只能出现在数字之间，不能出现在数字之前或之后。下划线对返回值
 * 没有影响，仅用于提高输入字符串的可读性。
 *
 * 遇到输入格式错误或溢出时，pg_strtoint16() 会抛出 ereport()；而
 * pg_strtoint16_safe() 则会将这些错误通过 *escontext 返回（如果它是
 * ErrorSaveContext 的话）。
 *
 * 注意：以无符号数形式累加输入，以应对最小负数采用补码表示、
 * 无法用有符号正数表示的情况。
 */
int16
pg_strtoint16(const char *s)
{
	return pg_strtoint16_safe(s, NULL);
}

int16
pg_strtoint16_safe(const char *s, Node *escontext)
{
	const char *ptr = s;
	const char *firstdigit;
	uint16		tmp = 0;
	bool		neg = false;
	unsigned char digit;
	int16		result;

	/*
	 * 多数情况下很可能是不带任何下划线分隔符的十进制数字。我们将首先
	 * 假设是这种情况来尝试解析字符串，只有当快速路径版本无法解析字符串时，
	 * 才退回到能处理十六进制、八进制、二进制字符串以及下划线的较慢实现。
	 */

	/* 前导空格的查找交给慢速路径处理 */

	if (*ptr == '-')
	{
		ptr++;
		neg = true;
	}

	/* 前导 '\+' 不常见，交给慢速路径处理 */

	/* 处理第一个数字 */
	digit = (*ptr - '0');

	/*
	 * 利用无符号算术，省去同时检查数字上下界的需要。
	 */
	if (likely(digit < 10))
	{
		ptr++;
		tmp = digit;
	}
	else
	{
		/* 至少需要一个数字 */
		goto slow;
	}

	/* 处理剩余数字 */
	for (;;)
	{
		digit = (*ptr - '0');

		if (digit >= 10)
			break;

		ptr++;

		if (unlikely(tmp > -(PG_INT16_MIN / 10)))
			goto out_of_range;

		tmp = tmp * 10 + digit;
	}

	/* 当字符串不是以数字结尾时，交给慢速路径处理 */
	if (unlikely(*ptr != '\0'))
		goto slow;

	if (neg)
	{
		if (unlikely(pg_neg_u16_overflow(tmp, &result)))
			goto out_of_range;
		return result;
	}

	if (unlikely(tmp > PG_INT16_MAX))
		goto out_of_range;

	return (int16) tmp;

slow:
	tmp = 0;
	ptr = s;
	/* 无需重置 neg */

	/* 跳过前导空格 */
	while (isspace((unsigned char) *ptr))
		ptr++;

	/* 处理符号 */
	if (*ptr == '-')
	{
		ptr++;
		neg = true;
	}
	else if (*ptr == '+')
		ptr++;

	/* 处理数字 */
	if (ptr[0] == '0' && (ptr[1] == 'x' || ptr[1] == 'X'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (isxdigit((unsigned char) *ptr))
			{
				if (unlikely(tmp > -(PG_INT16_MIN / 16)))
					goto out_of_range;

				tmp = tmp * 16 + hexlookup[(unsigned char) *ptr++];
			}
			else if (*ptr == '_')
			{
				/* 下划线后面必须跟随更多数字 */
				ptr++;
				if (*ptr == '\0' || !isxdigit((unsigned char) *ptr))
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else if (ptr[0] == '0' && (ptr[1] == 'o' || ptr[1] == 'O'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '7')
			{
				if (unlikely(tmp > -(PG_INT16_MIN / 8)))
					goto out_of_range;

				tmp = tmp * 8 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线后面必须跟随更多数字 */
				ptr++;
				if (*ptr == '\0' || *ptr < '0' || *ptr > '7')
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else if (ptr[0] == '0' && (ptr[1] == 'b' || ptr[1] == 'B'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '1')
			{
				if (unlikely(tmp > -(PG_INT16_MIN / 2)))
					goto out_of_range;

				tmp = tmp * 2 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线后面必须跟随更多数字 */
				ptr++;
				if (*ptr == '\0' || *ptr < '0' || *ptr > '1')
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else
	{
		firstdigit = ptr;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '9')
			{
				if (unlikely(tmp > -(PG_INT16_MIN / 10)))
					goto out_of_range;

				tmp = tmp * 10 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线不能出现在最前面 */
				if (unlikely(ptr == firstdigit))
					goto invalid_syntax;
				/* 且其后必须跟随更多数字 */
				ptr++;
				if (*ptr == '\0' || !isdigit((unsigned char) *ptr))
					goto invalid_syntax;
			}
			else
				break;
		}
	}

	/* 至少需要一个数字 */
	if (unlikely(ptr == firstdigit))
		goto invalid_syntax;

	/* 允许尾随空白，但不允许其他尾随字符 */
	while (isspace((unsigned char) *ptr))
		ptr++;

	if (unlikely(*ptr != '\0'))
		goto invalid_syntax;

	if (neg)
	{
		if (unlikely(pg_neg_u16_overflow(tmp, &result)))
			goto out_of_range;
		return result;
	}

	if (tmp > PG_INT16_MAX)
		goto out_of_range;

	return (int16) tmp;

out_of_range:
	ereturn(escontext, 0,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("value \"%s\" is out of range for type %s",
					s, "smallint")));

invalid_syntax:
	ereturn(escontext, 0,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"smallint", s)));
}

/*
 * 将输入字符串转换为有符号的 32 位整数。输入字符串可以用十进制、
 * 十六进制、八进制或二进制格式表达，所有这些格式都可以带一个可选的符号
 * 字符前缀，'\+'（默认）表示正数，'-' 表示负数。十六进制字符串通过数字
 * 前缀 0x 或 0X 来识别，八进制字符串通过 0o 或 0O 前缀识别，二进制表示
 * 通过 0b 或 0B 前缀识别。
 *
 * 允许任意数量的前导或尾随空白字符。数字之间可以用单个下划线字符分隔。
 * 下划线只能出现在数字之间，不能出现在数字之前或之后。下划线对返回值
 * 没有影响，仅用于提高输入字符串的可读性。
 *
 * 遇到输入格式错误或溢出时，pg_strtoint32() 会抛出 ereport()；而
 * pg_strtoint32_safe() 则会将这些错误通过 *escontext 返回（如果它是
 * ErrorSaveContext 的话）。
 *
 * 注意：以无符号数形式累加输入，以应对最小负数采用补码表示、
 * 无法用有符号正数表示的情况。
 */
int32
pg_strtoint32(const char *s)
{
	return pg_strtoint32_safe(s, NULL);
}

int32
pg_strtoint32_safe(const char *s, Node *escontext)
{
	const char *ptr = s;
	const char *firstdigit;
	uint32		tmp = 0;
	bool		neg = false;
	unsigned char digit;
	int32		result;

	/*
	 * 多数情况下很可能是不带任何下划线分隔符的十进制数字。我们将首先
	 * 假设是这种情况来尝试解析字符串，只有当快速路径版本无法解析字符串时，
	 * 才退回到能处理十六进制、八进制、二进制字符串以及下划线的较慢实现。
	 */

	/* 前导空格的查找交给慢速路径处理 */

	if (*ptr == '-')
	{
		ptr++;
		neg = true;
	}

	/* 前导 '\+' 不常见，交给慢速路径处理 */

	/* 处理第一个数字 */
	digit = (*ptr - '0');

	/*
	 * 利用无符号算术，省去同时检查数字上下界的需要。
	 */
	if (likely(digit < 10))
	{
		ptr++;
		tmp = digit;
	}
	else
	{
		/* 至少需要一个数字 */
		goto slow;
	}

	/* 处理剩余数字 */
	for (;;)
	{
		digit = (*ptr - '0');

		if (digit >= 10)
			break;

		ptr++;

		if (unlikely(tmp > -(PG_INT32_MIN / 10)))
			goto out_of_range;

		tmp = tmp * 10 + digit;
	}

	/* 当字符串不是以数字结尾时，交给慢速路径处理 */
	if (unlikely(*ptr != '\0'))
		goto slow;

	if (neg)
	{
		if (unlikely(pg_neg_u32_overflow(tmp, &result)))
			goto out_of_range;
		return result;
	}

	if (unlikely(tmp > PG_INT32_MAX))
		goto out_of_range;

	return (int32) tmp;

slow:
	tmp = 0;
	ptr = s;
	/* 无需重置 neg */

	/* 跳过前导空格 */
	while (isspace((unsigned char) *ptr))
		ptr++;

	/* 处理符号 */
	if (*ptr == '-')
	{
		ptr++;
		neg = true;
	}
	else if (*ptr == '+')
		ptr++;

	/* 处理数字 */
	if (ptr[0] == '0' && (ptr[1] == 'x' || ptr[1] == 'X'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (isxdigit((unsigned char) *ptr))
			{
				if (unlikely(tmp > -(PG_INT32_MIN / 16)))
					goto out_of_range;

				tmp = tmp * 16 + hexlookup[(unsigned char) *ptr++];
			}
			else if (*ptr == '_')
			{
				/* 下划线后面必须跟随更多数字 */
				ptr++;
				if (*ptr == '\0' || !isxdigit((unsigned char) *ptr))
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else if (ptr[0] == '0' && (ptr[1] == 'o' || ptr[1] == 'O'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '7')
			{
				if (unlikely(tmp > -(PG_INT32_MIN / 8)))
					goto out_of_range;

				tmp = tmp * 8 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线后面必须跟随更多数字 */
				ptr++;
				if (*ptr == '\0' || *ptr < '0' || *ptr > '7')
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else if (ptr[0] == '0' && (ptr[1] == 'b' || ptr[1] == 'B'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '1')
			{
				if (unlikely(tmp > -(PG_INT32_MIN / 2)))
					goto out_of_range;

				tmp = tmp * 2 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线后面必须跟随更多数字 */
				ptr++;
				if (*ptr == '\0' || *ptr < '0' || *ptr > '1')
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else
	{
		firstdigit = ptr;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '9')
			{
				if (unlikely(tmp > -(PG_INT32_MIN / 10)))
					goto out_of_range;

				tmp = tmp * 10 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线不能出现在最前面 */
				if (unlikely(ptr == firstdigit))
					goto invalid_syntax;
				/* 且其后必须跟随更多数字 */
				ptr++;
				if (*ptr == '\0' || !isdigit((unsigned char) *ptr))
					goto invalid_syntax;
			}
			else
				break;
		}
	}

	/* 至少需要一个数字 */
	if (unlikely(ptr == firstdigit))
		goto invalid_syntax;

	/* 允许尾随空白，但不允许其他尾随字符 */
	while (isspace((unsigned char) *ptr))
		ptr++;

	if (unlikely(*ptr != '\0'))
		goto invalid_syntax;

	if (neg)
	{
		if (unlikely(pg_neg_u32_overflow(tmp, &result)))
			goto out_of_range;
		return result;
	}

	if (tmp > PG_INT32_MAX)
		goto out_of_range;

	return (int32) tmp;

out_of_range:
	ereturn(escontext, 0,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("value \"%s\" is out of range for type %s",
					s, "integer")));

invalid_syntax:
	ereturn(escontext, 0,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"integer", s)));
}

/*
 * 将输入字符串转换为有符号的 64 位整数。输入字符串可以用十进制、
 * 十六进制、八进制或二进制格式表达，所有这些格式都可以带一个可选的符号
 * 字符前缀，'\+'（默认）表示正数，'-' 表示负数。十六进制字符串通过数字
 * 前缀 0x 或 0X 来识别，八进制字符串通过 0o 或 0O 前缀识别，二进制表示
 * 通过 0b 或 0B 前缀识别。
 *
 * 允许任意数量的前导或尾随空白字符。数字之间可以用单个下划线字符分隔。
 * 下划线只能出现在数字之间，不能出现在数字之前或之后。下划线对返回值
 * 没有影响，仅用于提高输入字符串的可读性。
 *
 * 遇到输入格式错误或溢出时，pg_strtoint64() 会抛出 ereport()；而
 * pg_strtoint64_safe() 则会将这些错误通过 *escontext 返回（如果它是
 * ErrorSaveContext 的话）。
 *
 * 注意：以无符号数形式累加输入，以应对最小负数采用补码表示、
 * 无法用有符号正数表示的情况。
 */
int64
pg_strtoint64(const char *s)
{
	return pg_strtoint64_safe(s, NULL);
}

int64
pg_strtoint64_safe(const char *s, Node *escontext)
{
	const char *ptr = s;
	const char *firstdigit;
	uint64		tmp = 0;
	bool		neg = false;
	unsigned char digit;
	int64		result;

	/*
	 * 多数情况下很可能是不带任何下划线分隔符的十进制数字。我们将首先
	 * 假设是这种情况来尝试解析字符串，只有当快速路径版本无法解析字符串时，
	 * 才退回到能处理十六进制、八进制、二进制字符串以及下划线的较慢实现。
	 */

	/* 前导空格的查找交给慢速路径处理 */

	if (*ptr == '-')
	{
		ptr++;
		neg = true;
	}

	/* 前导 '\+' 不常见，交给慢速路径处理 */

	/* 处理第一个数字 */
	digit = (*ptr - '0');

	/*
	 * 利用无符号算术，省去同时检查数字上下界的需要。
	 */
	if (likely(digit < 10))
	{
		ptr++;
		tmp = digit;
	}
	else
	{
		/* 至少需要一个数字 */
		goto slow;
	}

	/* 处理剩余数字 */
	for (;;)
	{
		digit = (*ptr - '0');

		if (digit >= 10)
			break;

		ptr++;

		if (unlikely(tmp > -(PG_INT64_MIN / 10)))
			goto out_of_range;

		tmp = tmp * 10 + digit;
	}

	/* 当字符串不是以数字结尾时，交给慢速路径处理 */
	if (unlikely(*ptr != '\0'))
		goto slow;

	if (neg)
	{
		if (unlikely(pg_neg_u64_overflow(tmp, &result)))
			goto out_of_range;
		return result;
	}

	if (unlikely(tmp > PG_INT64_MAX))
		goto out_of_range;

	return (int64) tmp;

slow:
	tmp = 0;
	ptr = s;
	/* 无需重置 neg */

	/* 跳过前导空格 */
	while (isspace((unsigned char) *ptr))
		ptr++;

	/* 处理符号 */
	if (*ptr == '-')
	{
		ptr++;
		neg = true;
	}
	else if (*ptr == '+')
		ptr++;

	/* 处理数字 */
	if (ptr[0] == '0' && (ptr[1] == 'x' || ptr[1] == 'X'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (isxdigit((unsigned char) *ptr))
			{
				if (unlikely(tmp > -(PG_INT64_MIN / 16)))
					goto out_of_range;

				tmp = tmp * 16 + hexlookup[(unsigned char) *ptr++];
			}
			else if (*ptr == '_')
			{
				/* 下划线后面必须跟随更多数字 */
				ptr++;
				if (*ptr == '\0' || !isxdigit((unsigned char) *ptr))
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else if (ptr[0] == '0' && (ptr[1] == 'o' || ptr[1] == 'O'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '7')
			{
				if (unlikely(tmp > -(PG_INT64_MIN / 8)))
					goto out_of_range;

				tmp = tmp * 8 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线后面必须跟随更多数字 */
				ptr++;
				if (*ptr == '\0' || *ptr < '0' || *ptr > '7')
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else if (ptr[0] == '0' && (ptr[1] == 'b' || ptr[1] == 'B'))
	{
		firstdigit = ptr += 2;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '1')
			{
				if (unlikely(tmp > -(PG_INT64_MIN / 2)))
					goto out_of_range;

				tmp = tmp * 2 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线后面必须跟随更多数字 */
				ptr++;
				if (*ptr == '\0' || *ptr < '0' || *ptr > '1')
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else
	{
		firstdigit = ptr;

		for (;;)
		{
			if (*ptr >= '0' && *ptr <= '9')
			{
				if (unlikely(tmp > -(PG_INT64_MIN / 10)))
					goto out_of_range;

				tmp = tmp * 10 + (*ptr++ - '0');
			}
			else if (*ptr == '_')
			{
				/* 下划线不能出现在最前面 */
				if (unlikely(ptr == firstdigit))
					goto invalid_syntax;
				/* 且其后必须跟随更多数字 */
				ptr++;
				if (*ptr == '\0' || !isdigit((unsigned char) *ptr))
					goto invalid_syntax;
			}
			else
				break;
		}
	}

	/* 至少需要一个数字 */
	if (unlikely(ptr == firstdigit))
		goto invalid_syntax;

	/* 允许尾随空白，但不允许其他尾随字符 */
	while (isspace((unsigned char) *ptr))
		ptr++;

	if (unlikely(*ptr != '\0'))
		goto invalid_syntax;

	if (neg)
	{
		if (unlikely(pg_neg_u64_overflow(tmp, &result)))
			goto out_of_range;
		return result;
	}

	if (tmp > PG_INT64_MAX)
		goto out_of_range;

	return (int64) tmp;

out_of_range:
	ereturn(escontext, 0,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("value \"%s\" is out of range for type %s",
					s, "bigint")));

invalid_syntax:
	ereturn(escontext, 0,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"bigint", s)));
}

/*
 * 将输入字符串转换为无符号的 32 位整数。
 *
 * 允许任意数量的前导或尾随空白字符。
 *
 * 如果 endloc 不为 NULL，则将指向字符串剩余部分的指针存储在那里，
 * 以便调用者解析剩余部分。否则，若数字之后存在非空白字符则报错。
 *
 * typname 是在错误消息中报告的类型名。
 *
 * 如果 escontext 指向一个 ErrorSaveContext 节点，则填入该节点而非
 * 抛出错误；调用者必须检查 SOFT_ERROR_OCCURRED() 来检测错误。
 */
uint32
uint32in_subr(const char *s, char **endloc,
			  const char *typname, Node *escontext)
{
	uint32		result;
	unsigned long cvt;
	char	   *endptr;

	errno = 0;
	cvt = strtoul(s, &endptr, 0);

	/*
	 * strtoul() 通常只会设置 ERANGE。在某些系统上它可能还会设置 EINVAL，
	 * 那只是表示它无法解析输入字符串。务必以与标准错误指示
	 * （即 endptr == s）相同的方式报告该情况。
	 */
	if ((errno && errno != ERANGE) || endptr == s)
		ereturn(escontext, 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						typname, s)));

	if (errno == ERANGE)
		ereturn(escontext, 0,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value \"%s\" is out of range for type %s",
						s, typname)));

	if (endloc)
	{
		/* 调用者想要处理字符串剩余部分 */
		*endloc = endptr;
	}
	else
	{
		/* 数字之后只允许空白 */
		while (*endptr && isspace((unsigned char) *endptr))
			endptr++;
		if (*endptr)
			ereturn(escontext, 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s: \"%s\"",
							typname, s)));
	}

	result = (uint32) cvt;

	/*
	 * 考虑 unsigned long 比 uint32 更宽的情况，此时 strtoul 不会对某些
	 * 超出 uint32 范围的值报错。
	 *
	 * 出于向后兼容性的考虑，我们希望接受带负号给出的输入，因此若输入值
	 * 在以有符号或无符号方式扩展到 long 后都匹配，则允许该输入值。
	 *
	 * 为确保在 32 位和 64 位平台上结果一致，应使错误消息与
	 * strtoul() 返回 ERANGE 时相同。
	 */
#if PG_UINT32_MAX != ULONG_MAX
	if (cvt != (unsigned long) result &&
		cvt != (unsigned long) ((int) result))
		ereturn(escontext, 0,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value \"%s\" is out of range for type %s",
						s, typname)));
#endif

	return result;
}

/*
 * 将输入字符串转换为无符号的 64 位整数。
 *
 * 允许任意数量的前导或尾随空白字符。
 *
 * 如果 endloc 不为 NULL，则将指向字符串剩余部分的指针存储在那里，
 * 以便调用者解析剩余部分。否则，若数字之后存在非空白字符则报错。
 *
 * typname 是在错误消息中报告的类型名。
 *
 * 如果 escontext 指向一个 ErrorSaveContext 节点，则填入该节点而非
 * 抛出错误；调用者必须检查 SOFT_ERROR_OCCURRED() 来检测错误。
 */
uint64
uint64in_subr(const char *s, char **endloc,
			  const char *typname, Node *escontext)
{
	uint64		result;
	char	   *endptr;

	errno = 0;
	result = strtou64(s, &endptr, 0);

	/*
	 * strtoul[l] 通常只会设置 ERANGE。在某些系统上它可能还会设置 EINVAL，
	 * 那只是表示它无法解析输入字符串。务必以与标准错误指示
	 * （即 endptr == s）相同的方式报告该情况。
	 */
	if ((errno && errno != ERANGE) || endptr == s)
		ereturn(escontext, 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						typname, s)));

	if (errno == ERANGE)
		ereturn(escontext, 0,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value \"%s\" is out of range for type %s",
						s, typname)));

	if (endloc)
	{
		/* 调用者想要处理字符串剩余部分 */
		*endloc = endptr;
	}
	else
	{
		/* 数字之后只允许空白 */
		while (*endptr && isspace((unsigned char) *endptr))
			endptr++;
		if (*endptr)
			ereturn(escontext, 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s: \"%s\"",
							typname, s)));
	}

	return result;
}

/*
 * pg_itoa：将有符号 16 位整数转换为其字符串表示，并返回 strlen(a)。
 *
 * 调用者必须确保 'a' 指向足够容纳结果的内存（至少 7 字节，
 * 含一个前导符号和一个结尾 NUL）。
 *
 * 似乎不值得单独实现这个函数。
 */
int
pg_itoa(int16 i, char *a)
{
	return pg_ltoa((int32) i, a);
}

/*
 * pg_ultoa_n：将一个无符号 32 位整数转换为其字符串表示（不以 NUL 结尾），
 * 并返回该字符串表示的长度。
 *
 * 调用者必须确保 'a' 指向足够容纳结果的内存（至少 10 字节）。
 */
int
pg_ultoa_n(uint32 value, char *a)
{
	int			olength,
				i = 0;

	/* 退化情形 */
	if (value == 0)
	{
		*a = '0';
		return 1;
	}

	olength = decimalLength32(value);

	/* 计算结果字符串。 */
	while (value >= 10000)
	{
		const uint32 c = value - 10000 * (value / 10000);
		const uint32 c0 = (c % 100) << 1;
		const uint32 c1 = (c / 100) << 1;

		char	   *pos = a + olength - i;

		value /= 10000;

		memcpy(pos - 2, DIGIT_TABLE + c0, 2);
		memcpy(pos - 4, DIGIT_TABLE + c1, 2);
		i += 4;
	}
	if (value >= 100)
	{
		const uint32 c = (value % 100) << 1;

		char	   *pos = a + olength - i;

		value /= 100;

		memcpy(pos - 2, DIGIT_TABLE + c, 2);
		i += 2;
	}
	if (value >= 10)
	{
		const uint32 c = value << 1;

		char	   *pos = a + olength - i;

		memcpy(pos - 2, DIGIT_TABLE + c, 2);
	}
	else
	{
		*a = (char) ('0' + value);
	}

	return olength;
}

/*
 * pg_ltoa：将有符号 32 位整数转换为其字符串表示，并返回 strlen(a)。
 *
 * 调用者有责任确保 a 至少 12 字节长，这足以容纳一个负号、
 * 最长的 int32 以及上面的结尾 NUL。
 */
int
pg_ltoa(int32 value, char *a)
{
	uint32		uvalue = (uint32) value;
	int			len = 0;

	if (value < 0)
	{
		uvalue = (uint32) 0 - uvalue;
		a[len++] = '-';
	}
	len += pg_ultoa_n(uvalue, a + len);
	a[len] = '\0';
	return len;
}

/*
 * 获取十进制表示（不以 NUL 结尾），并返回其长度。调用者必须确保
 * a 至少指向 MAXINT8LEN 字节。
 */
int
pg_ulltoa_n(uint64 value, char *a)
{
	int			olength,
				i = 0;
	uint32		value2;

	/* 退化情形 */
	if (value == 0)
	{
		*a = '0';
		return 1;
	}

	olength = decimalLength64(value);

	/* 计算结果字符串。 */
	while (value >= 100000000)
	{
		const uint64 q = value / 100000000;
		uint32		value3 = (uint32) (value - 100000000 * q);

		const uint32 c = value3 % 10000;
		const uint32 d = value3 / 10000;
		const uint32 c0 = (c % 100) << 1;
		const uint32 c1 = (c / 100) << 1;
		const uint32 d0 = (d % 100) << 1;
		const uint32 d1 = (d / 100) << 1;

		char	   *pos = a + olength - i;

		value = q;

		memcpy(pos - 2, DIGIT_TABLE + c0, 2);
		memcpy(pos - 4, DIGIT_TABLE + c1, 2);
		memcpy(pos - 6, DIGIT_TABLE + d0, 2);
		memcpy(pos - 8, DIGIT_TABLE + d1, 2);
		i += 8;
	}

	/* 切换到 32 位以提升速度 */
	value2 = (uint32) value;

	if (value2 >= 10000)
	{
		const uint32 c = value2 - 10000 * (value2 / 10000);
		const uint32 c0 = (c % 100) << 1;
		const uint32 c1 = (c / 100) << 1;

		char	   *pos = a + olength - i;

		value2 /= 10000;

		memcpy(pos - 2, DIGIT_TABLE + c0, 2);
		memcpy(pos - 4, DIGIT_TABLE + c1, 2);
		i += 4;
	}
	if (value2 >= 100)
	{
		const uint32 c = (value2 % 100) << 1;
		char	   *pos = a + olength - i;

		value2 /= 100;

		memcpy(pos - 2, DIGIT_TABLE + c, 2);
		i += 2;
	}
	if (value2 >= 10)
	{
		const uint32 c = value2 << 1;
		char	   *pos = a + olength - i;

		memcpy(pos - 2, DIGIT_TABLE + c, 2);
	}
	else
		*a = (char) ('0' + value2);

	return olength;
}

/*
 * pg_lltoa：将有符号 64 位整数转换为其字符串表示，并返回 strlen(a)。
 *
 * 调用者必须确保 'a' 指向足够容纳结果的内存（至少 MAXINT8LEN + 1 字节，
 * 含一个前导符号和一个结尾 NUL）。
 */
int
pg_lltoa(int64 value, char *a)
{
	uint64		uvalue = value;
	int			len = 0;

	if (value < 0)
	{
		uvalue = (uint64) 0 - uvalue;
		a[len++] = '-';
	}

	len += pg_ulltoa_n(uvalue, a + len);
	a[len] = '\0';
	return len;
}


/*
 * pg_ultostr_zeropad
 *		将 'value' 转换为十进制字符串表示，存储到 'str' 中。
 *		'minwidth' 指定结果的最小宽度；多余的空位通过在数字前
 *		补零来填充。
 *
 * 返回字符串结果的结束地址（最后一个写入字符的下一个位置）。
 * 注意不会写入 NUL 结束符。
 *
 * 这个函数的预期用途是构建包含多个独立数字的字符串，例如：
 *
 *	str = pg_ultostr_zeropad(str, hours, 2);
 *	*str++ = ':';
 *	str = pg_ultostr_zeropad(str, mins, 2);
 *	*str++ = ':';
 *	str = pg_ultostr_zeropad(str, secs, 2);
 *	*str = '\0';
 *
 * 注意：调用者必须确保 'str' 指向足够容纳结果的内存。
 */
char *
pg_ultostr_zeropad(char *str, uint32 value, int32 minwidth)
{
	int			len;

	Assert(minwidth > 0);

	if (value < 100 && minwidth == 2)	/* 常见情形的快捷处理 */
	{
		memcpy(str, DIGIT_TABLE + value * 2, 2);
		return str + 2;
	}

	len = pg_ultoa_n(value, str);
	if (len >= minwidth)
		return str + len;

	memmove(str + minwidth - len, str, len);
	memset(str, '0', minwidth - len);
	return str + minwidth;
}

/*
 * pg_ultostr
 *		将 'value' 转换为十进制字符串表示，存储到 'str' 中。
 *
 * 返回字符串结果的结束地址（最后一个写入字符的下一个位置）。
 * 注意不会写入 NUL 结束符。
 *
 * 这个函数的预期用途是构建包含多个独立数字的字符串，例如：
 *
 *	str = pg_ultostr(str, a);
 *	*str++ = ' ';
 *	str = pg_ultostr(str, b);
 *	*str = '\0';
 *
 * 注意：调用者必须确保 'str' 指向足够容纳结果的内存。
 */
char *
pg_ultostr(char *str, uint32 value)
{
	int			len = pg_ultoa_n(value, str);

	return str + len;
}
