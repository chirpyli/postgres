/*-------------------------------------------------------------------------
 *
 * varchar.c
 *	  Functions for the built-in types char(n) and varchar(n).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/varchar.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/pg_locale.h"
#include "utils/varlena.h"

/* bpchartypmodin 与 varchartypmodin 的共用代码 */
static int32
anychar_typmodin(ArrayType *ta, const char *typename)
{
	int32		typmod;
	int32	   *tl;
	int			n;

	tl = ArrayGetIntegerTypmods(ta, &n);

	/*
 * 此处我们并不太在意给出好的错误信息，因为语法分析器
 * 本就不应该允许 CHAR 出现错误数量的修饰符
	 */
	if (n != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid type modifier")));

	if (*tl < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("length for type %s must be at least 1", typename)));
	if (*tl > MaxAttrSize)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("length for type %s cannot exceed %d",
						typename, MaxAttrSize)));

	/*
 * 出于历史原因，typmod 是 VARHDRSZ 加上字符个数；
 * 已经有足够多的客户端代码了解这一约定，
 * 因此我们最好不要改动它。
	 */
	typmod = VARHDRSZ + *tl;

	return typmod;
}

/* bpchartypmodout 与 varchartypmodout 的共用代码 */
static char *
anychar_typmodout(int32 typmod)
{
	char	   *res = (char *) palloc(64);

	if (typmod > VARHDRSZ)
		snprintf(res, 64, "(%d)", (int) (typmod - VARHDRSZ));
	else
		*res = '\0';

	return res;
}


/*
 * CHAR() 和 VARCHAR() 类型是 SQL 标准的一部分。CHAR()
 * 用于空白填充（blank-padded）字符串，其长度在 CREATE TABLE 时指定。
 * VARCHAR 则用于存储长度不超过 CREATE TABLE 时所指定长度的字符串。
 *
 * 实现这些类型比较困难，因为我们无法从类型本身推断出它的长度。
 * 我把（希望是全部的）调用某数据类型输入函数的 fmgr 调用都改为同时
 * 传入长度。（例如在 INSERT 中，我们有 tupleDescriptor，其中包含
 * 各属性的长度，从而也就能知道 char() 或 varchar() 的确切长度。
 * 我们把这个长度传给 bpcharin() 或 varcharin()。）在无法确定的情况下，
 * 我们改为传入 -1，此时输入转换函数不会执行任何长度检查。
 *
 * 我们实际上把它实现为 varlena，这样就无需为比较函数传入长度。
 * （这些类型与 "text" 的区别在于：我们会在插入时截断字符串，
 * 并可能用空格填充。）
 *
 *															  - ay 6/95
 */


/*****************************************************************************
 *	 bpchar - char()														 *
 *****************************************************************************/

/*
 * bpchar_input —— bpcharin 与 bpcharrecv 的共用核心实现
 *
 * s 是长度为 len 的输入文本（可能不以 null 结尾）
 * atttypmod 是要应用的 typmod 值
 *
 * 注意，atttypmod 是以字符为单位度量的，
 * 并不一定等于字节数。
 *
 * 如果输入字符串过长，则报错；除非多余的字符都是空格，
 * 在这种情况下将它们截断。（依据 SQL 标准）
 *
 * 如果 escontext 指向一个 ErrorSaveContext 节点，则填入该节点
 * 而不是抛出异常；调用方必须检查 SOFT_ERROR_OCCURRED()
 * 来检测错误。
 */
static BpChar *
bpchar_input(const char *s, size_t len, int32 atttypmod, Node *escontext)
{
	BpChar	   *result;
	char	   *r;
	size_t		maxlen;

	/* 如果 typmod 为 -1（或无效），则使用字符串的实际长度 */
	if (atttypmod < (int32) VARHDRSZ)
		maxlen = len;
	else
	{
		size_t		charlen;	/* 输入中的字符（CHARACTER）个数 */

		maxlen = atttypmod - VARHDRSZ;
		charlen = pg_mbstrlen_with_len(s, len);
		if (charlen > maxlen)
		{
			/* 验证多余的字符是否都是空格，若是则裁掉它们 */
			size_t		mbmaxlen = pg_mbcharcliplen(s, len, maxlen);
			size_t		j;

			/*
 * 至此，len 是输入字符串的实际字节长度，
 * maxlen 是该 bpchar 类型所允许的字符（CHARACTER）最大个数，
 * mbmaxlen 是这些字符所占的字节长度。
			 */
			for (j = mbmaxlen; j < len; j++)
			{
				if (s[j] != ' ')
					ereturn(escontext, NULL,
							(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
							 errmsg("value too long for type character(%d)",
									(int) maxlen)));
			}

			/*
 * 现在我们把 maxlen 设为所需的字节长度，而不是
 * 字符个数！
			 */
			maxlen = len = mbmaxlen;
		}
		else
		{
			/*
 * 现在我们把 maxlen 设为所需的字节长度，而不是
 * 字符个数！
			 */
			maxlen = len + (maxlen - charlen);
		}
	}

	result = (BpChar *) palloc(maxlen + VARHDRSZ);
	SET_VARSIZE(result, maxlen + VARHDRSZ);
	r = VARDATA(result);
	memcpy(r, s, len);

	/* 必要时用空格填充字符串 */
	if (maxlen > len)
		memset(r + len, ' ', maxlen - len);

	return result;
}

/*
 * 将 C 字符串转换为 CHARACTER 的内部表示。atttypmod
 * 是类型的声明长度加上 VARHDRSZ。
 */
Datum
bpcharin(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	BpChar	   *result;

	result = bpchar_input(s, strlen(s), atttypmod, fcinfo->context);
	PG_RETURN_BPCHAR_P(result);
}


/*
 * 将 CHARACTER 值转换为 C 字符串。
 *
 * 使用的是 text 的转换函数，这仅在 BpChar 与
 * text 是等价类型时才合适。
 */
Datum
bpcharout(PG_FUNCTION_ARGS)
{
	Datum		txt = PG_GETARG_DATUM(0);

	PG_RETURN_CSTRING(TextDatumGetCString(txt));
}

/*
 *		bpcharrecv			- 将外部二进制格式转换为 bpchar
 */
Datum
bpcharrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	BpChar	   *result;
	char	   *str;
	int			nbytes;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);
	result = bpchar_input(str, nbytes, atttypmod, NULL);
	pfree(str);
	PG_RETURN_BPCHAR_P(result);
}

/*
 *		bpcharsend			- 将 bpchar 转换为二进制格式
 */
Datum
bpcharsend(PG_FUNCTION_ARGS)
{
	/* 与 textsend 完全相同，因此共用代码 */
	return textsend(fcinfo);
}


/*
 * 将 CHARACTER 类型转换为指定的长度。
 *
 * maxlen 是 typmod，即声明长度加上 VARHDRSZ 字节。
 * 如果这是针对显式转换到 char(N) 的，则 isExplicit 为 true。
 *
 * 截断规则：对于显式转换，静默截断到给定长度；
 * 对于隐式转换，除非多余字符全为空格，否则报错。
 * （这在某种程度上符合 SQL：规范实际上要求我们在显式转换的情形下
 * 抛出一个 "完成条件"（completion condition），但 Postgres
 * 并没有这样的概念。）
 */
Datum
bpchar(PG_FUNCTION_ARGS)
{
	BpChar	   *source = PG_GETARG_BPCHAR_PP(0);
	int32		maxlen = PG_GETARG_INT32(1);
	bool		isExplicit = PG_GETARG_BOOL(2);
	BpChar	   *result;
	int32		len;
	char	   *r;
	char	   *s;
	int			i;
	int			charlen;		/* 输入字符串中的字符个数 +
								 * VARHDRSZ */

	/* 如果 typmod 无效则无需处理 */
	if (maxlen < (int32) VARHDRSZ)
		PG_RETURN_BPCHAR_P(source);

	maxlen -= VARHDRSZ;

	len = VARSIZE_ANY_EXHDR(source);
	s = VARDATA_ANY(source);

	charlen = pg_mbstrlen_with_len(s, len);

	/* 如果提供的数据已经符合 typmod 则无需处理 */
	if (charlen == maxlen)
		PG_RETURN_BPCHAR_P(source);

	if (charlen > maxlen)
	{
		/* 验证多余的字符是否都是空格，若是则裁掉它们 */
		size_t		maxmblen;

		maxmblen = pg_mbcharcliplen(s, len, maxlen);

		if (!isExplicit)
		{
			for (i = maxmblen; i < len; i++)
				if (s[i] != ' ')
					ereport(ERROR,
							(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
							 errmsg("value too long for type character(%d)",
									maxlen)));
		}

		len = maxmblen;

		/*
 * 至此，maxlen 是所需的字节长度，而不是
 * 字符个数！
		 */
		maxlen = len;
	}
	else
	{
		/*
 * 至此，maxlen 是所需的字节长度，而不是
 * 字符个数！
		 */
		maxlen = len + (maxlen - charlen);
	}

	Assert(maxlen >= len);

	result = palloc(maxlen + VARHDRSZ);
	SET_VARSIZE(result, maxlen + VARHDRSZ);
	r = VARDATA(result);

	memcpy(r, s, len);

	/* 必要时用空格填充字符串 */
	if (maxlen > len)
		memset(r + len, ' ', maxlen - len);

	PG_RETURN_BPCHAR_P(result);
}


/* char_bpchar()
 * 将 char 转换为 bpchar(1)。
 */
Datum
char_bpchar(PG_FUNCTION_ARGS)
{
	char		c = PG_GETARG_CHAR(0);
	BpChar	   *result;

	result = (BpChar *) palloc(VARHDRSZ + 1);

	SET_VARSIZE(result, VARHDRSZ + 1);
	*(VARDATA(result)) = c;

	PG_RETURN_BPCHAR_P(result);
}


/* bpchar_name()
 * 将 bpchar() 类型转换为 NameData 类型。
 */
Datum
bpchar_name(PG_FUNCTION_ARGS)
{
	BpChar	   *s = PG_GETARG_BPCHAR_PP(0);
	char	   *s_data;
	Name		result;
	int			len;

	len = VARSIZE_ANY_EXHDR(s);
	s_data = VARDATA_ANY(s);

	/* 截断过长（oversize）的输入 */
	if (len >= NAMEDATALEN)
		len = pg_mbcliplen(s_data, len, NAMEDATALEN - 1);

	/* 去除尾随的空格 */
	while (len > 0)
	{
		if (s_data[len - 1] != ' ')
			break;
		len--;
	}

	/* 这里使用 palloc0 以确保结果被零填充 */
	result = (Name) palloc0(NAMEDATALEN);
	memcpy(NameStr(*result), s_data, len);

	PG_RETURN_NAME(result);
}

/* name_bpchar()
 * 将 NameData 类型转换为 bpchar 类型。
 *
 * 使用的是 text 的转换函数，这仅在 BpChar 与
 * text 是等价类型时才合适。
 */
Datum
name_bpchar(PG_FUNCTION_ARGS)
{
	Name		s = PG_GETARG_NAME(0);
	BpChar	   *result;

	result = (BpChar *) cstring_to_text(NameStr(*s));
	PG_RETURN_BPCHAR_P(result);
}

Datum
bpchartypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_INT32(anychar_typmodin(ta, "char"));
}

Datum
bpchartypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);

	PG_RETURN_CSTRING(anychar_typmodout(typmod));
}


/*****************************************************************************
 *	 varchar —— varchar(n)
 *
 * 注意：varchar 在大部分操作上借用了 text 类型，因此除 I/O 和 typmod
 * 检查之外，没有用 C 编写的函数。
 *****************************************************************************/

/*
 * varchar_input —— varcharin 与 varcharrecv 的共用核心实现
 *
 * s 是长度为 len 的输入文本（可能不以 null 结尾）
 * atttypmod 是要应用的 typmod 值
 *
 * 注意，atttypmod 是以字符为单位度量的，
 * 并不一定等于字节数。
 *
 * 如果输入字符串过长，则报错；除非多余的字符都是空格，
 * 在这种情况下将它们截断。（依据 SQL 标准）
 *
 * 如果 escontext 指向一个 ErrorSaveContext 节点，则填入该节点
 * 而不是抛出异常；调用方必须检查 SOFT_ERROR_OCCURRED()
 * 来检测错误。
 */
static VarChar *
varchar_input(const char *s, size_t len, int32 atttypmod, Node *escontext)
{
	VarChar    *result;
	size_t		maxlen;

	maxlen = atttypmod - VARHDRSZ;

	if (atttypmod >= (int32) VARHDRSZ && len > maxlen)
	{
		/* 验证多余的字符是否都是空格，若是则裁掉它们 */
		size_t		mbmaxlen = pg_mbcharcliplen(s, len, maxlen);
		size_t		j;

		for (j = mbmaxlen; j < len; j++)
		{
			if (s[j] != ' ')
				ereturn(escontext, NULL,
						(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
						 errmsg("value too long for type character varying(%d)",
								(int) maxlen)));
		}

		len = mbmaxlen;
	}

	/*
 * 我们可以使用 cstring_to_text_with_len，因为 VarChar 与 text 是
 * 二进制兼容的类型。
	 */
	result = (VarChar *) cstring_to_text_with_len(s, len);
	return result;
}

/*
 * 将 C 字符串转换为 VARCHAR 的内部表示。atttypmod
 * 是类型的声明长度加上 VARHDRSZ。
 */
Datum
varcharin(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	VarChar    *result;

	result = varchar_input(s, strlen(s), atttypmod, fcinfo->context);
	PG_RETURN_VARCHAR_P(result);
}


/*
 * 将 VARCHAR 值转换为 C 字符串。
 *
 * 使用的是 text 到 C 字符串的转换函数，这仅在 VarChar 与
 * text 是等价类型时才合适。
 */
Datum
varcharout(PG_FUNCTION_ARGS)
{
	Datum		txt = PG_GETARG_DATUM(0);

	PG_RETURN_CSTRING(TextDatumGetCString(txt));
}

/*
 *		varcharrecv			- 将外部二进制格式转换为 varchar
 */
Datum
varcharrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	VarChar    *result;
	char	   *str;
	int			nbytes;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);
	result = varchar_input(str, nbytes, atttypmod, NULL);
	pfree(str);
	PG_RETURN_VARCHAR_P(result);
}

/*
 *		varcharsend			- 将 varchar 转换为二进制格式
 */
Datum
varcharsend(PG_FUNCTION_ARGS)
{
	/* 与 textsend 完全相同，因此共用代码 */
	return textsend(fcinfo);
}


/*
 * varchar_support()
 *
 * varchar() 长度强制转换函数的规划器（planner）支持函数。
 *
 * 目前，我们唯一能做的优化是将“新最大长度 >= 先前最大长度”的
 * 调用做扁平化处理。我们可以忽略 isExplicit 参数，
 * 因为它只影响截断的情形。
 */
Datum
varchar_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;
		FuncExpr   *expr = req->fcall;
		Node	   *typmod;

		Assert(list_length(expr->args) >= 2);

		typmod = (Node *) lsecond(expr->args);

		if (IsA(typmod, Const) && !((Const *) typmod)->constisnull)
		{
			Node	   *source = (Node *) linitial(expr->args);
			int32		old_typmod = exprTypmod(source);
			int32		new_typmod = DatumGetInt32(((Const *) typmod)->constvalue);
			int32		old_max = old_typmod - VARHDRSZ;
			int32		new_max = new_typmod - VARHDRSZ;

			if (new_typmod < 0 || (old_typmod >= 0 && old_max <= new_max))
				ret = relabel_to_typmod(source, new_typmod);
		}
	}

	PG_RETURN_POINTER(ret);
}

/*
 * 将 VARCHAR 类型转换为指定的长度。
 *
 * maxlen 是 typmod，即声明长度加上 VARHDRSZ 字节。
 * 如果这是针对显式转换到 varchar(N) 的，则 isExplicit 为 true。
 *
 * 截断规则：对于显式转换，静默截断到给定长度；
 * 对于隐式转换，除非多余字符全为空格，否则报错。
 * （这在某种程度上符合 SQL：规范实际上要求我们在显式转换的情形下
 * 抛出一个 "完成条件"（completion condition），但 Postgres
 * 并没有这样的概念。）
 */
Datum
varchar(PG_FUNCTION_ARGS)
{
	VarChar    *source = PG_GETARG_VARCHAR_PP(0);
	int32		typmod = PG_GETARG_INT32(1);
	bool		isExplicit = PG_GETARG_BOOL(2);
	int32		len,
				maxlen;
	size_t		maxmblen;
	int			i;
	char	   *s_data;

	len = VARSIZE_ANY_EXHDR(source);
	s_data = VARDATA_ANY(source);
	maxlen = typmod - VARHDRSZ;

	/* 如果 typmod 无效，或提供的数据已经符合则无需处理 */
	if (maxlen < 0 || len <= maxlen)
		PG_RETURN_VARCHAR_P(source);

	/* 仅当字符串过长时才会执行到这里…… */

	/* 截断多字节字符串，同时保持多字节边界完整 */
	maxmblen = pg_mbcharcliplen(s_data, len, maxlen);

	if (!isExplicit)
	{
		for (i = maxmblen; i < len; i++)
			if (s_data[i] != ' ')
				ereport(ERROR,
						(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
						 errmsg("value too long for type character varying(%d)",
								maxlen)));
	}

	PG_RETURN_VARCHAR_P((VarChar *) cstring_to_text_with_len(s_data,
															 maxmblen));
}

Datum
varchartypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_INT32(anychar_typmodin(ta, "varchar"));
}

Datum
varchartypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);

	PG_RETURN_CSTRING(anychar_typmodout(typmod));
}


/*****************************************************************************
 * Exported functions
 *****************************************************************************/

/* BpChar 的“真实”长度（不计入尾随空格） */
static inline int
bcTruelen(BpChar *arg)
{
	return bpchartruelen(VARDATA_ANY(arg), VARSIZE_ANY_EXHDR(arg));
}

int
bpchartruelen(char *s, int len)
{
	int			i;

	/*
 * 注意，我们依赖于这样一个假设：在所有受支持的
 * 多字节服务端编码中，' ' 都是单字节单元。
	 */
	for (i = len - 1; i >= 0; i--)
	{
		if (s[i] != ' ')
			break;
	}
	return i + 1;
}

Datum
bpcharlen(PG_FUNCTION_ARGS)
{
	BpChar	   *arg = PG_GETARG_BPCHAR_PP(0);
	int			len;

	/* 获取字节数，忽略尾随空格 */
	len = bcTruelen(arg);

	/* 在多字节编码下，转换为字符个数 */
	if (pg_database_encoding_max_length() != 1)
		len = pg_mbstrlen_with_len(VARDATA_ANY(arg), len);

	PG_RETURN_INT32(len);
}

Datum
bpcharoctetlen(PG_FUNCTION_ARGS)
{
	Datum		arg = PG_GETARG_DATUM(0);

	/* 我们完全不需要对输入进行 detoast 处理 */
	PG_RETURN_INT32(toast_raw_datum_size(arg) - VARHDRSZ);
}


/*****************************************************************************
 *	用于 bpchar 的比较函数
 *
 * 注意：btree 索引要求这些例程不能泄漏内存；因此，
 * 要小心释放被烘烤（toasted）数据的工作副本。大多数地方
 * 不需要如此谨慎。
 *****************************************************************************/

static void
check_collation_set(Oid collid)
{
	if (!OidIsValid(collid))
	{
		/*
 * 这通常意味着解析器无法消解
 * 隐式排序规则（collation）之间的冲突，因此按此方式报告。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for string comparison"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}
}

Datum
bpchareq(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	bool		result;
	Oid			collid = PG_GET_COLLATION();
	pg_locale_t mylocale;

	check_collation_set(collid);

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	mylocale = pg_newlocale_from_collation(collid);

	if (mylocale->deterministic)
	{
		/*
 * 由于我们只在意是否相等，因此可以避免在这里
 * 付出 strcoll() 的全部开销，直接做按位比较即可。
		 */
		if (len1 != len2)
			result = false;
		else
			result = (memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), len1) == 0);
	}
	else
	{
		result = (varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
							 collid) == 0);
	}

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bpcharne(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	bool		result;
	Oid			collid = PG_GET_COLLATION();
	pg_locale_t mylocale;

	check_collation_set(collid);

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	mylocale = pg_newlocale_from_collation(collid);

	if (mylocale->deterministic)
	{
		/*
 * 由于我们只在意是否相等，因此可以避免在这里
 * 付出 strcoll() 的全部开销，直接做按位比较即可。
		 */
		if (len1 != len2)
			result = true;
		else
			result = (memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), len1) != 0);
	}
	else
	{
		result = (varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
							 collid) != 0);
	}

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bpcharlt(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
					 PG_GET_COLLATION());

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(cmp < 0);
}

Datum
bpcharle(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
					 PG_GET_COLLATION());

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(cmp <= 0);
}

Datum
bpchargt(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
					 PG_GET_COLLATION());

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(cmp > 0);
}

Datum
bpcharge(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
					 PG_GET_COLLATION());

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(cmp >= 0);
}

Datum
bpcharcmp(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
					 PG_GET_COLLATION());

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(cmp);
}

Datum
bpchar_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);
	Oid			collid = ssup->ssup_collation;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

	/* 使用通用的字符串 SortSupport */
	varstr_sortsupport(ssup, BPCHAROID, collid);

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_VOID();
}

Datum
bpchar_larger(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
					 PG_GET_COLLATION());

	PG_RETURN_BPCHAR_P((cmp >= 0) ? arg1 : arg2);
}

Datum
bpchar_smaller(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
					 PG_GET_COLLATION());

	PG_RETURN_BPCHAR_P((cmp <= 0) ? arg1 : arg2);
}


/*
 * bpchar 需要专用的哈希函数，因为我们希望在比较时
 * 忽略尾随的空格。
 */
Datum
hashbpchar(PG_FUNCTION_ARGS)
{
	BpChar	   *key = PG_GETARG_BPCHAR_PP(0);
	Oid			collid = PG_GET_COLLATION();
	char	   *keydata;
	int			keylen;
	pg_locale_t mylocale;
	Datum		result;

	if (!collid)
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for string hashing"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));

	keydata = VARDATA_ANY(key);
	keylen = bcTruelen(key);

	mylocale = pg_newlocale_from_collation(collid);

	if (mylocale->deterministic)
	{
		result = hash_any((unsigned char *) keydata, keylen);
	}
	else
	{
		Size		bsize,
					rsize;
		char	   *buf;

		bsize = pg_strnxfrm(NULL, 0, keydata, keylen, mylocale);
		buf = palloc(bsize + 1);

		rsize = pg_strnxfrm(buf, bsize + 1, keydata, keylen, mylocale);

		/* 第二次调用返回的值可能比第一次调用小 */
		if (rsize > bsize)
			elog(ERROR, "pg_strnxfrm() returned unexpected result");

		/*
 * 原则上，没有理由把结尾的 NUL 字符
 * 纳入哈希，但之前就是这么做的，因此
 * 必须保持这一行为不变。
		 */
		result = hash_any((uint8_t *) buf, bsize + 1);

		pfree(buf);
	}

	/* 避免为被烘烤（toasted）的输入泄漏内存 */
	PG_FREE_IF_COPY(key, 0);

	return result;
}

Datum
hashbpcharextended(PG_FUNCTION_ARGS)
{
	BpChar	   *key = PG_GETARG_BPCHAR_PP(0);
	Oid			collid = PG_GET_COLLATION();
	char	   *keydata;
	int			keylen;
	pg_locale_t mylocale;
	Datum		result;

	if (!collid)
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for string hashing"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));

	keydata = VARDATA_ANY(key);
	keylen = bcTruelen(key);

	mylocale = pg_newlocale_from_collation(collid);

	if (mylocale->deterministic)
	{
		result = hash_any_extended((unsigned char *) keydata, keylen,
								   PG_GETARG_INT64(1));
	}
	else
	{
		Size		bsize,
					rsize;
		char	   *buf;

		bsize = pg_strnxfrm(NULL, 0, keydata, keylen, mylocale);
		buf = palloc(bsize + 1);

		rsize = pg_strnxfrm(buf, bsize + 1, keydata, keylen, mylocale);

		/* 第二次调用返回的值可能比第一次调用小 */
		if (rsize > bsize)
			elog(ERROR, "pg_strnxfrm() returned unexpected result");

		/*
 * 原则上，没有理由把结尾的 NUL 字符
 * 纳入哈希，但之前就是这么做的，因此
 * 必须保持这一行为不变。
		 */
		result = hash_any_extended((uint8_t *) buf, bsize + 1,
								   PG_GETARG_INT64(1));

		pfree(buf);
	}

	PG_FREE_IF_COPY(key, 0);

	return result;
}

/*
 * 以下运算符支持对 bpchar 数据（datum）逐字符比较，
 * 以便构建适用于 LIKE 子句的索引。
 * 注意，普通的 bpchareq/bpcharne 比较运算符，以及
 * 使用 "C" 排序规则的常规支持函数 1 和 2，都被假定与这些运算符兼容！
 */

static int
internal_bpchar_pattern_compare(BpChar *arg1, BpChar *arg2)
{
	int			result;
	int			len1,
				len2;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	result = memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), Min(len1, len2));
	if (result != 0)
		return result;
	else if (len1 < len2)
		return -1;
	else if (len1 > len2)
		return 1;
	else
		return 0;
}


Datum
bpchar_pattern_lt(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			result;

	result = internal_bpchar_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result < 0);
}


Datum
bpchar_pattern_le(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			result;

	result = internal_bpchar_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result <= 0);
}


Datum
bpchar_pattern_ge(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			result;

	result = internal_bpchar_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result >= 0);
}


Datum
bpchar_pattern_gt(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			result;

	result = internal_bpchar_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result > 0);
}


Datum
btbpchar_pattern_cmp(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			result;

	result = internal_bpchar_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(result);
}


Datum
btbpchar_pattern_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

	/* 使用通用的字符串 SortSupport，并强制使用 "C" 排序规则 */
	varstr_sortsupport(ssup, BPCHAROID, C_COLLATION_OID);

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_VOID();
}
