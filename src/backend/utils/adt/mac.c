/*-------------------------------------------------------------------------
 *
 * mac.c
 *	  PostgreSQL 中 6 字节 EUI-48 MAC 地址的类型定义。
 *
 * Portions Copyright (c) 1998-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/backend/utils/adt/mac.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/hashfn.h"
#include "lib/hyperloglog.h"
#include "libpq/pqformat.h"
#include "port/pg_bswap.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/inet.h"
#include "utils/sortsupport.h"


/*
 *	用于排序和比较的实用宏：
 */

#define hibits(addr) \
  ((unsigned long)(((addr)->a<<16)|((addr)->b<<8)|((addr)->c)))

#define lobits(addr) \
  ((unsigned long)(((addr)->d<<16)|((addr)->e<<8)|((addr)->f)))

/* macaddr 的 sortsupport 支持 */
typedef struct
{
	int64		input_count;	/* 已见到的非 NULL 值的数量 */
	bool		estimating;		/* 如果正在估算基数则为 true */

	hyperLogLogState abbr_card; /* 基数估算器 */
} macaddr_sortsupport_state;

static int	macaddr_cmp_internal(macaddr *a1, macaddr *a2);
static int	macaddr_fast_cmp(Datum x, Datum y, SortSupport ssup);
static bool macaddr_abbrev_abort(int memtupcount, SortSupport ssup);
static Datum macaddr_abbrev_convert(Datum original, SortSupport ssup);

/*
 *	MAC 地址读取函数。接受几种常见的表示法。
 */

Datum
macaddr_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	macaddr    *result;
	int			a,
				b,
				c,
				d,
				e,
				f;
	char		junk[2];
	int			count;

	/* %1s 只有在存在末尾非空白垃圾字符时才匹配 */

	count = sscanf(str, "%x:%x:%x:%x:%x:%x%1s",
				   &a, &b, &c, &d, &e, &f, junk);
	if (count != 6)
		count = sscanf(str, "%x-%x-%x-%x-%x-%x%1s",
					   &a, &b, &c, &d, &e, &f, junk);
	if (count != 6)
		count = sscanf(str, "%2x%2x%2x:%2x%2x%2x%1s",
					   &a, &b, &c, &d, &e, &f, junk);
	if (count != 6)
		count = sscanf(str, "%2x%2x%2x-%2x%2x%2x%1s",
					   &a, &b, &c, &d, &e, &f, junk);
	if (count != 6)
		count = sscanf(str, "%2x%2x.%2x%2x.%2x%2x%1s",
					   &a, &b, &c, &d, &e, &f, junk);
	if (count != 6)
		count = sscanf(str, "%2x%2x-%2x%2x-%2x%2x%1s",
					   &a, &b, &c, &d, &e, &f, junk);
	if (count != 6)
		count = sscanf(str, "%2x%2x%2x%2x%2x%2x%1s",
					   &a, &b, &c, &d, &e, &f, junk);
	if (count != 6)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"", "macaddr",
						str)));

	if ((a < 0) || (a > 255) || (b < 0) || (b > 255) ||
		(c < 0) || (c > 255) || (d < 0) || (d > 255) ||
		(e < 0) || (e > 255) || (f < 0) || (f > 255))
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("invalid octet value in \"macaddr\" value: \"%s\"", str)));

	result = (macaddr *) palloc(sizeof(macaddr));

	result->a = a;
	result->b = b;
	result->c = c;
	result->d = d;
	result->e = e;
	result->f = f;

	PG_RETURN_MACADDR_P(result);
}

/*
 *	MAC 地址输出函数。固定格式。
 */

Datum
macaddr_out(PG_FUNCTION_ARGS)
{
	macaddr    *addr = PG_GETARG_MACADDR_P(0);
	char	   *result;

	result = (char *) palloc(32);

	snprintf(result, 32, "%02x:%02x:%02x:%02x:%02x:%02x",
			 addr->a, addr->b, addr->c, addr->d, addr->e, addr->f);

	PG_RETURN_CSTRING(result);
}

/*
 *		macaddr_recv			- 将外部二进制格式转换为 macaddr
 *
 * 外部表示即为这六个字节，高位字节在前（MSB first）。
 */
Datum
macaddr_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	macaddr    *addr;

	addr = (macaddr *) palloc(sizeof(macaddr));

	addr->a = pq_getmsgbyte(buf);
	addr->b = pq_getmsgbyte(buf);
	addr->c = pq_getmsgbyte(buf);
	addr->d = pq_getmsgbyte(buf);
	addr->e = pq_getmsgbyte(buf);
	addr->f = pq_getmsgbyte(buf);

	PG_RETURN_MACADDR_P(addr);
}

/*
 *		macaddr_send			- 将 macaddr 转换为二进制格式
 */
Datum
macaddr_send(PG_FUNCTION_ARGS)
{
	macaddr    *addr = PG_GETARG_MACADDR_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, addr->a);
	pq_sendbyte(&buf, addr->b);
	pq_sendbyte(&buf, addr->c);
	pq_sendbyte(&buf, addr->d);
	pq_sendbyte(&buf, addr->e);
	pq_sendbyte(&buf, addr->f);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*
 *	用于排序的比较函数：
 */

static int
macaddr_cmp_internal(macaddr *a1, macaddr *a2)
{
	if (hibits(a1) < hibits(a2))
		return -1;
	else if (hibits(a1) > hibits(a2))
		return 1;
	else if (lobits(a1) < lobits(a2))
		return -1;
	else if (lobits(a1) > lobits(a2))
		return 1;
	else
		return 0;
}

Datum
macaddr_cmp(PG_FUNCTION_ARGS)
{
	macaddr    *a1 = PG_GETARG_MACADDR_P(0);
	macaddr    *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_INT32(macaddr_cmp_internal(a1, a2));
}

/*
 *	布尔比较。
 */

Datum
macaddr_lt(PG_FUNCTION_ARGS)
{
	macaddr    *a1 = PG_GETARG_MACADDR_P(0);
	macaddr    *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) < 0);
}

Datum
macaddr_le(PG_FUNCTION_ARGS)
{
	macaddr    *a1 = PG_GETARG_MACADDR_P(0);
	macaddr    *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) <= 0);
}

Datum
macaddr_eq(PG_FUNCTION_ARGS)
{
	macaddr    *a1 = PG_GETARG_MACADDR_P(0);
	macaddr    *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) == 0);
}

Datum
macaddr_ge(PG_FUNCTION_ARGS)
{
	macaddr    *a1 = PG_GETARG_MACADDR_P(0);
	macaddr    *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) >= 0);
}

Datum
macaddr_gt(PG_FUNCTION_ARGS)
{
	macaddr    *a1 = PG_GETARG_MACADDR_P(0);
	macaddr    *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) > 0);
}

Datum
macaddr_ne(PG_FUNCTION_ARGS)
{
	macaddr    *a1 = PG_GETARG_MACADDR_P(0);
	macaddr    *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) != 0);
}

/*
 * 用于 macaddr 哈希索引的支持函数。
 */
Datum
hashmacaddr(PG_FUNCTION_ARGS)
{
	macaddr    *key = PG_GETARG_MACADDR_P(0);

	return hash_any((unsigned char *) key, sizeof(macaddr));
}

Datum
hashmacaddrextended(PG_FUNCTION_ARGS)
{
	macaddr    *key = PG_GETARG_MACADDR_P(0);

	return hash_any_extended((unsigned char *) key, sizeof(macaddr),
							 PG_GETARG_INT64(1));
}

/*
 * 算术函数：按位 NOT、AND、OR。
 */
Datum
macaddr_not(PG_FUNCTION_ARGS)
{
	macaddr    *addr = PG_GETARG_MACADDR_P(0);
	macaddr    *result;

	result = (macaddr *) palloc(sizeof(macaddr));
	result->a = ~addr->a;
	result->b = ~addr->b;
	result->c = ~addr->c;
	result->d = ~addr->d;
	result->e = ~addr->e;
	result->f = ~addr->f;
	PG_RETURN_MACADDR_P(result);
}

Datum
macaddr_and(PG_FUNCTION_ARGS)
{
	macaddr    *addr1 = PG_GETARG_MACADDR_P(0);
	macaddr    *addr2 = PG_GETARG_MACADDR_P(1);
	macaddr    *result;

	result = (macaddr *) palloc(sizeof(macaddr));
	result->a = addr1->a & addr2->a;
	result->b = addr1->b & addr2->b;
	result->c = addr1->c & addr2->c;
	result->d = addr1->d & addr2->d;
	result->e = addr1->e & addr2->e;
	result->f = addr1->f & addr2->f;
	PG_RETURN_MACADDR_P(result);
}

Datum
macaddr_or(PG_FUNCTION_ARGS)
{
	macaddr    *addr1 = PG_GETARG_MACADDR_P(0);
	macaddr    *addr2 = PG_GETARG_MACADDR_P(1);
	macaddr    *result;

	result = (macaddr *) palloc(sizeof(macaddr));
	result->a = addr1->a | addr2->a;
	result->b = addr1->b | addr2->b;
	result->c = addr1->c | addr2->c;
	result->d = addr1->d | addr2->d;
	result->e = addr1->e | addr2->e;
	result->f = addr1->f | addr2->f;
	PG_RETURN_MACADDR_P(result);
}

/*
 *	截断函数，用于比较 MAC 厂商。
 *	由 Alex Pilosov <alex@pilosoft.com> 建议
 */
Datum
macaddr_trunc(PG_FUNCTION_ARGS)
{
	macaddr    *addr = PG_GETARG_MACADDR_P(0);
	macaddr    *result;

	result = (macaddr *) palloc(sizeof(macaddr));

	result->a = addr->a;
	result->b = addr->b;
	result->c = addr->c;
	result->d = 0;
	result->e = 0;
	result->f = 0;

	PG_RETURN_MACADDR_P(result);
}

/*
 * SortSupport 策略函数。填充 SortSupport 结构体，提供使用缩写键比较
 * 所需的信息。
 */
Datum
macaddr_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = macaddr_fast_cmp;
	ssup->ssup_extra = NULL;

	if (ssup->abbreviate)
	{
		macaddr_sortsupport_state *uss;
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

		uss = palloc(sizeof(macaddr_sortsupport_state));
		uss->input_count = 0;
		uss->estimating = true;
		initHyperLogLog(&uss->abbr_card, 10);

		ssup->ssup_extra = uss;

		ssup->comparator = ssup_datum_unsigned_cmp;
		ssup->abbrev_converter = macaddr_abbrev_convert;
		ssup->abbrev_abort = macaddr_abbrev_abort;
		ssup->abbrev_full_comparator = macaddr_fast_cmp;

		MemoryContextSwitchTo(oldcontext);
	}

	PG_RETURN_VOID();
}

/*
 * SortSupport 的“传统”比较函数。从堆中取出两个 MAC 地址并对它们执行
 * 标准比较。
 */
static int
macaddr_fast_cmp(Datum x, Datum y, SortSupport ssup)
{
	macaddr    *arg1 = DatumGetMacaddrP(x);
	macaddr    *arg2 = DatumGetMacaddrP(y);

	return macaddr_cmp_internal(arg1, arg2);
}

/*
 * 用于估算缩写键优化效果的回调函数。
 *
 * 我们不关注未缩写数据的基数（cardinality），因为在权威的 macaddr
 * 比较器中不存在相等性快速路径。
 */
static bool
macaddr_abbrev_abort(int memtupcount, SortSupport ssup)
{
	macaddr_sortsupport_state *uss = ssup->ssup_extra;
	double		abbr_card;

	if (memtupcount < 10000 || uss->input_count < 10000 || !uss->estimating)
		return false;

	abbr_card = estimateHyperLogLog(&uss->abbr_card);

		/*
		 * 如果我们拥有的不同值超过 10 万，那么即使我们要排序数十亿行，
		 * 也很可能仍然划算，而撤销如此多行缩写键的代价可能并不值得。
		 * 此时我们停止计数，因为我们已经完全确定采用缩写方案了。
		 */
	if (abbr_card > 100000.0)
	{
		if (trace_sort)
			elog(LOG,
				 "macaddr_abbrev: estimation ends at cardinality %f"
				 " after " INT64_FORMAT " values (%d rows)",
				 abbr_card, uss->input_count, memtupcount);
		uss->estimating = false;
		return false;
	}

		/*
		 * 目标最小基数为每约 2k 个非 NULL 输入 1 个。0.5 行的容差因子
		 * 让我们能够在真正病态的数据上更早中止——即在头 2k 行（非 NULL）
		 * 中恰好只有一个缩写值的情况。
		 */
	if (abbr_card < uss->input_count / 2000.0 + 0.5)
	{
		if (trace_sort)
			elog(LOG,
				 "macaddr_abbrev: aborting abbreviation at cardinality %f"
				 " below threshold %f after " INT64_FORMAT " values (%d rows)",
				 abbr_card, uss->input_count / 2000.0 + 0.5, uss->input_count,
				 memtupcount);
		return true;
	}

	if (trace_sort)
		elog(LOG,
			 "macaddr_abbrev: cardinality %f after " INT64_FORMAT
			 " values (%d rows)", abbr_card, uss->input_count, memtupcount);

	return false;
}

/*
 * SortSupport 转换例程。将原始的 macaddr 表示转换为缩写键表示。
 *
 * 将 6 字节 MAC 地址的字节打包进一个 Datum，并将其作为无符号整数用于
 * 比较。在 64 位机器上，会有两个填充的零字节。该整数被转换为本机字节
 * 序，以便于比较。
 */
static Datum
macaddr_abbrev_convert(Datum original, SortSupport ssup)
{
	macaddr_sortsupport_state *uss = ssup->ssup_extra;
	macaddr    *authoritative = DatumGetMacaddrP(original);
	Datum		res;

	/*
	 * 在 64 位机器上，先将 8 字节 datum 清零，再把 MAC 地址的 6 个字节
	 * 拷贝进去。在最低有效位的末尾会有两个零填充字节。
	 */
#if SIZEOF_DATUM == 8
	memset(&res, 0, SIZEOF_DATUM);
	memcpy(&res, authoritative, sizeof(macaddr));
#else							/* SIZEOF_DATUM != 8 */
	memcpy(&res, authoritative, SIZEOF_DATUM);
#endif
	uss->input_count += 1;

		/*
		 * 基数估算。该估算使用 uint32，因此在 64 位架构下，将两个 32 位
		 * 半字异或在一起以产生略多的熵。这两个零字节不会对这一运算产生
		 * 任何实际影响。
		 */
	if (uss->estimating)
	{
		uint32		tmp;

#if SIZEOF_DATUM == 8
		tmp = (uint32) res ^ (uint32) ((uint64) res >> 32);
#else							/* SIZEOF_DATUM != 8 */
		tmp = (uint32) res;
#endif

		addHyperLogLog(&uss->abbr_card, DatumGetUInt32(hash_uint32(tmp)));
	}

	/*
	 * 在小端机器上进行字节交换。
	 *
	 * 这是为了让 ssup_datum_unsigned_cmp()（一个无符号整数三路比较器）
	 * 在所有平台上都能正确工作。否则，比较器将不得不对每对缩写键的首字节
	 * 指针调用 memcmp()，那样会更慢。
	 */
	res = DatumBigEndianToNative(res);

	return res;
}
