/*-------------------------------------------------------------------------
 *
 * enum.c
 *	  枚举类型的 I/O 函数、运算符、聚合等
 *
 * Copyright (c) 2006-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/enum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_enum.h"
#include "libpq/pqformat.h"
#include "storage/procarray.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


static Oid	enum_endpoint(Oid enumtypoid, ScanDirection direction);
static ArrayType *enum_range_internal(Oid enumtypoid, Oid lower, Oid upper);


/*
 * 禁止对未提交的 pg_enum 元组进行使用。
 *
 * 我们需要确保未提交的枚举值不会进入索引。如果进入了索引，而我们随后
 * 又回滚了对 pg_enum 的添加，就会破坏索引，因为没有底层的 pg_enum 条目
 * 时，值的比较将无法可靠工作。（注意：删除包含某个枚举值的堆元组
 * 并不足以保证该值不会出现在索引的更高层级中。）为此，我们禁止将
 * 未提交的行用于任何 SQL 层面的用途。这比必要的限制更严格，因为该值
 * 可能根本不会被插入到表中，或者其列上可能并没有索引，但这种集中式
 * 的强制检查很容易实现。
 *
 * 不过，允许使用属于那些在同一事务中创建的枚举类型的未提交值是没问题的，
 * 因为那样的话任何这样的索引也都是全新的，会在回滚时一并消失。我们目前
 * 并未完整实现这一点，但确实允许自由使用 CREATE TYPE AS ENUM 期间创建的
 * 枚举值，它们显然与枚举类型具有相同的生命周期。（"pg_restore -1" 需要
 * 这种情况。）通过 ALTER TYPE ADD VALUE 添加的值，如果已知该枚举类型是在
 * 同一事务中较早创建的，也允许使用。（注意：我们必须显式地跟踪这一点；
 * 比较元组的 xmin 是不够的，因为类型元组可能已在当前事务中被更新。
 * 子事务也会带来需要考虑的风险；目前 pg_enum.c 只在最外层事务级别
 * 处理 ADD VALUE。）
 *
 * 在下面任何可能向 SQL 操作返回枚举值的函数中，都需要调用本函数
 *（直接或间接调用）。
 */
static void
check_safe_enum_use(HeapTuple enumval_tup)
{
	TransactionId xmin;
	Form_pg_enum en = (Form_pg_enum) GETSTRUCT(enumval_tup);

	/*
	 * 如果该行被标记为已提交，那么它肯定是安全的。这为所有正常的
	 * 使用场景提供了一条快速路径。
	 */
	if (HeapTupleHeaderXminCommitted(enumval_tup->t_data))
		return;

	/*
	 * 通常情况下，一行在被读取或载入 syscache 时会被标记为已提交；
	 * 但以防万一没有被标记，我们直接检查 xmin。
	 */
	xmin = HeapTupleHeaderGetXmin(enumval_tup->t_data);
	if (!TransactionIdIsInProgress(xmin) &&
		TransactionIdDidCommit(xmin))
		return;

	/*
	 * 检查该枚举值是否被标记为未提交。如果没有，那它就是安全的，
	 * 因为它不可能比其所属的类型的生命周期更短。（对于其他事务
	 * 创建的值，这里也会是假；但前面的检查应该已经处理过所有这些情况了。）
	 */
	if (!EnumUncommitted(en->oid))
		return;

	/*
	 * 我们本可以在这里做更多检查来缩小不安全条件的范围，但目前
	 * 就直接抛出一个异常。
	 */
	ereport(ERROR,
			(errcode(ERRCODE_UNSAFE_NEW_ENUM_VALUE_USAGE),
			 errmsg("unsafe use of new value \"%s\" of enum type %s",
					NameStr(en->enumlabel),
					format_type_be(en->enumtypid)),
			 errhint("New enum values must be committed before they can be used.")));
}


/* 基本 I/O 支持 */

Datum
enum_in(PG_FUNCTION_ARGS)
{
	char	   *name = PG_GETARG_CSTRING(0);
	Oid			enumtypoid = PG_GETARG_OID(1);
	Node	   *escontext = fcinfo->context;
	Oid			enumoid;
	HeapTuple	tup;

	/* 必须检查长度，以防止 SearchSysCache 内部发生 Assert 失败 */
	if (strlen(name) >= NAMEDATALEN)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input value for enum %s: \"%s\"",
						format_type_be(enumtypoid),
						name)));

	tup = SearchSysCache2(ENUMTYPOIDNAME,
						  ObjectIdGetDatum(enumtypoid),
						  CStringGetDatum(name));
	if (!HeapTupleIsValid(tup))
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input value for enum %s: \"%s\"",
						format_type_be(enumtypoid),
						name)));

	/*
	 * 检查它在 SQL 中使用是否安全。或许我们应该费点功夫以"软"方式
	 * 报告"不安全的使用"；但目前尚不清楚这番功夫是否值得，也不清楚
	 * 这到底算不算一个合法的非法输入情形，抑或只是一种实现上的不足。
	 */
	check_safe_enum_use(tup);

	/*
	 * 这个值来自 pg_enum.oid，并在用户表中存储系统 OID。该 OID 必须在
	 * 二进制升级过程中被保留。
	 */
	enumoid = ((Form_pg_enum) GETSTRUCT(tup))->oid;

	ReleaseSysCache(tup);

	PG_RETURN_OID(enumoid);
}

Datum
enum_out(PG_FUNCTION_ARGS)
{
	Oid			enumval = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	tup;
	Form_pg_enum en;

	tup = SearchSysCache1(ENUMOID, ObjectIdGetDatum(enumval));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid internal value for enum: %u",
						enumval)));
	en = (Form_pg_enum) GETSTRUCT(tup);

	result = pstrdup(NameStr(en->enumlabel));

	ReleaseSysCache(tup);

	PG_RETURN_CSTRING(result);
}

/* 二进制 I/O 支持 */
Datum
enum_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	Oid			enumtypoid = PG_GETARG_OID(1);
	Oid			enumoid;
	HeapTuple	tup;
	char	   *name;
	int			nbytes;

	name = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);

	/* 必须检查长度，以防止 SearchSysCache 内部发生 Assert 失败 */
	if (strlen(name) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input value for enum %s: \"%s\"",
						format_type_be(enumtypoid),
						name)));

	tup = SearchSysCache2(ENUMTYPOIDNAME,
						  ObjectIdGetDatum(enumtypoid),
						  CStringGetDatum(name));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input value for enum %s: \"%s\"",
						format_type_be(enumtypoid),
						name)));

	/* 检查它在 SQL 中使用是否安全 */
	check_safe_enum_use(tup);

	enumoid = ((Form_pg_enum) GETSTRUCT(tup))->oid;

	ReleaseSysCache(tup);

	pfree(name);

	PG_RETURN_OID(enumoid);
}

Datum
enum_send(PG_FUNCTION_ARGS)
{
	Oid			enumval = PG_GETARG_OID(0);
	StringInfoData buf;
	HeapTuple	tup;
	Form_pg_enum en;

	tup = SearchSysCache1(ENUMOID, ObjectIdGetDatum(enumval));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid internal value for enum: %u",
						enumval)));
	en = (Form_pg_enum) GETSTRUCT(tup);

	pq_begintypsend(&buf);
	pq_sendtext(&buf, NameStr(en->enumlabel), strlen(NameStr(en->enumlabel)));

	ReleaseSysCache(tup);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/* 比较函数及相关 */

/*
 * enum_cmp_internal 是所有可见比较函数的公共引擎，
 * 例外是 enum_eq 和 enum_ne，它们只需直接检查 OID 是否相等即可。
 */
static int
enum_cmp_internal(Oid arg1, Oid arg2, FunctionCallInfo fcinfo)
{
	TypeCacheEntry *tcache;

	/*
	 * 我们并不需要 typcache，除非（但愿很少见）其中一个或两个 OID
	 * 是奇数。这意味着对未将 flinfo 传给枚举比较函数的代码所做的粗略测试，
	 * 可能无法暴露这一疏忽。为了让此类错误更明显，我们即便在快速路径
	 * 退出时也要 Assert 存在一个可供缓存的位置。
	 */
	Assert(fcinfo->flinfo != NULL);

	/* 相等的 OID 无论如何都相等 */
	if (arg1 == arg2)
		return 0;

	/* 快速路径：偶数 OID 的比较结果是已知的，可直接比较 */
	if ((arg1 & 1) == 0 && (arg2 & 1) == 0)
	{
		if (arg1 < arg2)
			return -1;
		else
			return 1;
	}

	/* 定位该枚举类型对应的 typcache 条目 */
	tcache = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;
	if (tcache == NULL)
	{
		HeapTuple	enum_tup;
		Form_pg_enum en;
		Oid			typeoid;

		/* 获取包含 arg1 的枚举类型的 OID */
		enum_tup = SearchSysCache1(ENUMOID, ObjectIdGetDatum(arg1));
		if (!HeapTupleIsValid(enum_tup))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("invalid internal value for enum: %u",
							arg1)));
		en = (Form_pg_enum) GETSTRUCT(enum_tup);
		typeoid = en->enumtypid;
		ReleaseSysCache(enum_tup);
		/* 现在定位并记住 typcache 条目 */
		tcache = lookup_type_cache(typeoid, 0);
		fcinfo->flinfo->fn_extra = tcache;
	}

	/* 余下的比较逻辑位于 typcache.c */
	return compare_values_of_enum(tcache, arg1, arg2);
}

Datum
enum_lt(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(enum_cmp_internal(a, b, fcinfo) < 0);
}

Datum
enum_le(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(enum_cmp_internal(a, b, fcinfo) <= 0);
}

Datum
enum_eq(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(a == b);
}

Datum
enum_ne(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(a != b);
}

Datum
enum_ge(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(enum_cmp_internal(a, b, fcinfo) >= 0);
}

Datum
enum_gt(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(enum_cmp_internal(a, b, fcinfo) > 0);
}

Datum
enum_smaller(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_OID(enum_cmp_internal(a, b, fcinfo) < 0 ? a : b);
}

Datum
enum_larger(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_OID(enum_cmp_internal(a, b, fcinfo) > 0 ? a : b);
}

Datum
enum_cmp(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_INT32(enum_cmp_internal(a, b, fcinfo));
}

/* 枚举编程支持函数 */

/*
 * enum_endpoint：enum_first/enum_last 的公共代码
 */
static Oid
enum_endpoint(Oid enumtypoid, ScanDirection direction)
{
	Relation	enum_rel;
	Relation	enum_idx;
	SysScanDesc enum_scan;
	HeapTuple	enum_tuple;
	ScanKeyData skey;
	Oid			minmax;

	/*
	 * 使用 pg_enum_typid_sortorder_index 查找第一个/最后一个枚举成员。
	 * 注意：我们一定不能使用 syscache。更多说明参见 catalog/pg_enum.c
	 * 中 RenumberEnumType 的注释。
	 */
	ScanKeyInit(&skey,
				Anum_pg_enum_enumtypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(enumtypoid));

	enum_rel = table_open(EnumRelationId, AccessShareLock);
	enum_idx = index_open(EnumTypIdSortOrderIndexId, AccessShareLock);
	enum_scan = systable_beginscan_ordered(enum_rel, enum_idx, NULL,
										   1, &skey);

	enum_tuple = systable_getnext_ordered(enum_scan, direction);
	if (HeapTupleIsValid(enum_tuple))
	{
		/* 检查它在 SQL 中使用是否安全 */
		check_safe_enum_use(enum_tuple);
		minmax = ((Form_pg_enum) GETSTRUCT(enum_tuple))->oid;
	}
	else
	{
		/* 这种情况只会在空枚举时发生 */
		minmax = InvalidOid;
	}

	systable_endscan_ordered(enum_scan);
	index_close(enum_idx, AccessShareLock);
	table_close(enum_rel, AccessShareLock);

	return minmax;
}

Datum
enum_first(PG_FUNCTION_ARGS)
{
	Oid			enumtypoid;
	Oid			min;

	/*
	 * 我们依赖能够从调用方的表达式树中获取具体的枚举类型。注意，
	 * 参数实际的值根本不会被检查；尤其是它可能为空（NULL）。
	 */
	enumtypoid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (enumtypoid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not determine actual enum type")));

	/* 通过索引获取 OID */
	min = enum_endpoint(enumtypoid, ForwardScanDirection);

	if (!OidIsValid(min))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("enum %s contains no values",
						format_type_be(enumtypoid))));

	PG_RETURN_OID(min);
}

Datum
enum_last(PG_FUNCTION_ARGS)
{
	Oid			enumtypoid;
	Oid			max;

	/*
	 * 我们依赖能够从调用方的表达式树中获取具体的枚举类型。注意，
	 * 参数实际的值根本不会被检查；尤其是它可能为空（NULL）。
	 */
	enumtypoid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (enumtypoid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not determine actual enum type")));

	/* 通过索引获取 OID */
	max = enum_endpoint(enumtypoid, BackwardScanDirection);

	if (!OidIsValid(max))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("enum %s contains no values",
						format_type_be(enumtypoid))));

	PG_RETURN_OID(max);
}

/* 枚举范围（enum_range）的两参数变体 */
Datum
enum_range_bounds(PG_FUNCTION_ARGS)
{
	Oid			lower;
	Oid			upper;
	Oid			enumtypoid;

	if (PG_ARGISNULL(0))
		lower = InvalidOid;
	else
		lower = PG_GETARG_OID(0);
	if (PG_ARGISNULL(1))
		upper = InvalidOid;
	else
		upper = PG_GETARG_OID(1);

	/*
	 * 我们依赖能够从调用方的表达式树中获取具体的枚举类型。泛型类型
	 * 机制应该已经保证了二者属于同一类型。
	 */
	enumtypoid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (enumtypoid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not determine actual enum type")));

	PG_RETURN_ARRAYTYPE_P(enum_range_internal(enumtypoid, lower, upper));
}

/* 枚举范围（enum_range）的一参数变体 */
Datum
enum_range_all(PG_FUNCTION_ARGS)
{
	Oid			enumtypoid;

	/*
	 * 我们依赖能够从调用方的表达式树中获取具体的枚举类型。注意，
	 * 参数实际的值根本不会被检查；尤其是它可能为空（NULL）。
	 */
	enumtypoid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (enumtypoid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not determine actual enum type")));

	PG_RETURN_ARRAYTYPE_P(enum_range_internal(enumtypoid,
											  InvalidOid, InvalidOid));
}

static ArrayType *
enum_range_internal(Oid enumtypoid, Oid lower, Oid upper)
{
	ArrayType  *result;
	Relation	enum_rel;
	Relation	enum_idx;
	SysScanDesc enum_scan;
	HeapTuple	enum_tuple;
	ScanKeyData skey;
	Datum	   *elems;
	int			max,
				cnt;
	bool		left_found;

	/*
	 * 使用 pg_enum_typid_sortorder_index 按序扫描枚举成员。
	 * 注意：我们一定不能使用 syscache。更多说明参见 catalog/pg_enum.c
	 * 中 RenumberEnumType 的注释。
	 */
	ScanKeyInit(&skey,
				Anum_pg_enum_enumtypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(enumtypoid));

	enum_rel = table_open(EnumRelationId, AccessShareLock);
	enum_idx = index_open(EnumTypIdSortOrderIndexId, AccessShareLock);
	enum_scan = systable_beginscan_ordered(enum_rel, enum_idx, NULL, 1, &skey);

	max = 64;
	elems = (Datum *) palloc(max * sizeof(Datum));
	cnt = 0;
	left_found = !OidIsValid(lower);

	while (HeapTupleIsValid(enum_tuple = systable_getnext_ordered(enum_scan, ForwardScanDirection)))
	{
		Oid			enum_oid = ((Form_pg_enum) GETSTRUCT(enum_tuple))->oid;

		if (!left_found && lower == enum_oid)
			left_found = true;

		if (left_found)
		{
			/* 检查它在 SQL 中使用是否安全 */
			check_safe_enum_use(enum_tuple);

			if (cnt >= max)
			{
				max *= 2;
				elems = (Datum *) repalloc(elems, max * sizeof(Datum));
			}

			elems[cnt++] = ObjectIdGetDatum(enum_oid);
		}

		if (OidIsValid(upper) && upper == enum_oid)
			break;
	}

	systable_endscan_ordered(enum_scan);
	index_close(enum_idx, AccessShareLock);
	table_close(enum_rel, AccessShareLock);

	/* 构建结果数组 */
	/* 注意：这里硬编码了 Oid 表示方式的一些细节 */
	result = construct_array(elems, cnt, enumtypoid,
							 sizeof(Oid), true, TYPALIGN_INT);

	pfree(elems);

	return result;
}
