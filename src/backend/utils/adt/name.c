/*-------------------------------------------------------------------------
 *
 * name.c
 *	  内建类型 "name" 的处理函数。
 *
 * name 替代了 char16，其实现经过精心设计，使其成为一个物理长度为
 * NAMEDATALEN 的字符串。
 * 任何地方都不要使用硬编码常量，
 * 请始终使用符号常量 NAMEDATALEN！   - jolly 8/21/95
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/name.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/varlena.h"


/*****************************************************************************
 *	 用户 I/O 例程（无）												 *
 *****************************************************************************/


/*
 *		namein	- 将 cstring 转换为内部表示
 *
 *		注意：
 *				[旧] 此前若 strlen(s) < NAMEDATALEN，多余的字符会填充为空字符
 *				现在，总是以 NULL 结尾
 */
Datum
namein(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
	Name		result;
	int			len;

	len = strlen(s);

	/* 截断超长的输入 */
	if (len >= NAMEDATALEN)
		len = pg_mbcliplen(s, len, NAMEDATALEN - 1);

	/* 这里使用 palloc0 以确保结果以零填充 */
	result = (Name) palloc0(NAMEDATALEN);
	memcpy(NameStr(*result), s, len);

	PG_RETURN_NAME(result);
}

/*
 *		nameout - 将内部表示转换为 cstring
 */
Datum
nameout(PG_FUNCTION_ARGS)
{
	Name		s = PG_GETARG_NAME(0);

	PG_RETURN_CSTRING(pstrdup(NameStr(*s)));
}

/*
 *		namerecv			- 将外部二进制格式转换为 name
 */
Datum
namerecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	Name		result;
	char	   *str;
	int			nbytes;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);
	if (nbytes >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("identifier too long"),
				 errdetail("Identifier must be less than %d characters.",
						   NAMEDATALEN)));
	result = (NameData *) palloc0(NAMEDATALEN);
	memcpy(result, str, nbytes);
	pfree(str);
	PG_RETURN_NAME(result);
}

/*
 *		namesend			- 将 name 转换为二进制格式
 */
Datum
namesend(PG_FUNCTION_ARGS)
{
	Name		s = PG_GETARG_NAME(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendtext(&buf, NameStr(*s), strlen(NameStr(*s)));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*****************************************************************************
 *	 比较/排序例程											 *
 *****************************************************************************/

/*
 *		nameeq	- 当且仅当参数相等时返回 1
 *		namene	- 当且仅当参数不相等时返回 1
 *		namelt	- 当且仅当 a < b 时返回 1
 *		namele	- 当且仅当 a <= b 时返回 1
 *		namegt	- 当且仅当 a > b 时返回 1
 *		namege	- 当且仅当 a >= b 时返回 1
 *
 * 注意，配合 NAMEDATALEN 上限使用 strncmp 主要是出于历史原因；用 strcmp 也
 * 同样可行，因为我们不允许没有 '\0' 结束符的 NAME 值。结束符之后的任何内容
 * 都不被视为与比较相关。
 */
static int
namecmp(Name arg1, Name arg2, Oid collid)
{
	/* 针对系统目录中常见情形的快速路径 */
	if (collid == C_COLLATION_OID)
		return strncmp(NameStr(*arg1), NameStr(*arg2), NAMEDATALEN);

	/* 否则依赖 varstr 基础设施 */
	return varstr_cmp(NameStr(*arg1), strlen(NameStr(*arg1)),
					  NameStr(*arg2), strlen(NameStr(*arg2)),
					  collid);
}

Datum
nameeq(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	Name		arg2 = PG_GETARG_NAME(1);

	PG_RETURN_BOOL(namecmp(arg1, arg2, PG_GET_COLLATION()) == 0);
}

Datum
namene(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	Name		arg2 = PG_GETARG_NAME(1);

	PG_RETURN_BOOL(namecmp(arg1, arg2, PG_GET_COLLATION()) != 0);
}

Datum
namelt(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	Name		arg2 = PG_GETARG_NAME(1);

	PG_RETURN_BOOL(namecmp(arg1, arg2, PG_GET_COLLATION()) < 0);
}

Datum
namele(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	Name		arg2 = PG_GETARG_NAME(1);

	PG_RETURN_BOOL(namecmp(arg1, arg2, PG_GET_COLLATION()) <= 0);
}

Datum
namegt(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	Name		arg2 = PG_GETARG_NAME(1);

	PG_RETURN_BOOL(namecmp(arg1, arg2, PG_GET_COLLATION()) > 0);
}

Datum
namege(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	Name		arg2 = PG_GETARG_NAME(1);

	PG_RETURN_BOOL(namecmp(arg1, arg2, PG_GET_COLLATION()) >= 0);
}

Datum
btnamecmp(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	Name		arg2 = PG_GETARG_NAME(1);

	PG_RETURN_INT32(namecmp(arg1, arg2, PG_GET_COLLATION()));
}

Datum
btnamesortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);
	Oid			collid = ssup->ssup_collation;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

	/* 使用通用字符串 SortSupport */
	varstr_sortsupport(ssup, NAMEOID, collid);

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_VOID();
}


/*****************************************************************************
 *	 其他公共例程											 *
 *****************************************************************************/

void
namestrcpy(Name name, const char *str)
{
	/* 注意：我们需要对目标进行零填充。 */
	strncpy(NameStr(*name), str, NAMEDATALEN);
	NameStr(*name)[NAMEDATALEN - 1] = '\0';
}

/*
 * 将一个 NAME 与一个 C 字符串进行比较
 *
 * 始终假定使用 C 排序规则；除等值检查外，将其用于其他用途时务必小心！
 */
int
namestrcmp(Name name, const char *str)
{
	if (!name && !str)
		return 0;
	if (!name)
		return -1;				/* NULL < 任何值 */
	if (!str)
		return 1;				/* NULL < 任何值 */
	return strncmp(NameStr(*name), str, NAMEDATALEN);
}


/*
 * SQL 函数 CURRENT_USER、SESSION_USER
 */
Datum
current_user(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall1(namein, CStringGetDatum(GetUserNameFromId(GetUserId(), false))));
}

Datum
session_user(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall1(namein, CStringGetDatum(GetUserNameFromId(GetSessionUserId(), false))));
}


/*
 * SQL 函数 CURRENT_SCHEMA、CURRENT_SCHEMAS
 */
Datum
current_schema(PG_FUNCTION_ARGS)
{
	List	   *search_path = fetch_search_path(false);
	char	   *nspname;

	if (search_path == NIL)
		PG_RETURN_NULL();
	nspname = get_namespace_name(linitial_oid(search_path));
	list_free(search_path);
	if (!nspname)
		PG_RETURN_NULL();		/* 最近被删除的命名空间？ */
	PG_RETURN_DATUM(DirectFunctionCall1(namein, CStringGetDatum(nspname)));
}

Datum
current_schemas(PG_FUNCTION_ARGS)
{
	List	   *search_path = fetch_search_path(PG_GETARG_BOOL(0));
	ListCell   *l;
	Datum	   *names;
	int			i;
	ArrayType  *array;

	names = (Datum *) palloc(list_length(search_path) * sizeof(Datum));
	i = 0;
	foreach(l, search_path)
	{
		char	   *nspname;

		nspname = get_namespace_name(lfirst_oid(l));
		if (nspname)			/* 注意可能被删除的命名空间 */
		{
			names[i] = DirectFunctionCall1(namein, CStringGetDatum(nspname));
			i++;
		}
	}
	list_free(search_path);

	array = construct_array_builtin(names, i, NAMEOID);

	PG_RETURN_POINTER(array);
}

/*
 * SQL 函数 nameconcatoid(name, oid) 返回 name
 *
 * 它在 information_schema 中用于生成 specific_name 列，这些列应在每个 schema
 * 内唯一。我们（以一种不太优雅的方式）通过追加对象的 OID 来实现这一点。其
 * 结果与
 *		($1::text || '_' || $2::text)::name
 * 相同，区别在于：如果结果放不下 NAMEDATALEN，我们会通过截断 name 输入（而
 * 非 oid）使其容纳得下。
 */
Datum
nameconcatoid(PG_FUNCTION_ARGS)
{
	Name		nam = PG_GETARG_NAME(0);
	Oid			oid = PG_GETARG_OID(1);
	Name		result;
	char		suffix[20];
	int			suflen;
	int			namlen;

	suflen = snprintf(suffix, sizeof(suffix), "_%u", oid);
	namlen = strlen(NameStr(*nam));

	/* 通过截断 name 部分（而非后缀）来截断超长的输入 */
	if (namlen + suflen >= NAMEDATALEN)
		namlen = pg_mbcliplen(NameStr(*nam), namlen, NAMEDATALEN - 1 - suflen);

	/* 这里使用 palloc0 以确保结果以零填充 */
	result = (Name) palloc0(NAMEDATALEN);
	memcpy(NameStr(*result), NameStr(*nam), namlen);
	memcpy(NameStr(*result) + namlen, suffix, suflen);

	PG_RETURN_NAME(result);
}
