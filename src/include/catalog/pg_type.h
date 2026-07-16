/*-------------------------------------------------------------------------
 *
 * pg_type.h
 *	  “type”系统目录(pg_type)的定义
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_type.h
 *
 * NOTES
 *	  Catalog.pm 模块读取本文件并推导出模式信息。
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TYPE_H
#define PG_TYPE_H

#include "catalog/genbki.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_type_d.h"	/* IWYU pragma: export */
#include "nodes/nodes.h"

/* ----------------
 *		pg_type 定义。cpp 将此转换为
 *		typedef struct FormData_pg_type
 *
 *		pg_type 实例中的部分字段会被复制到
 *		pg_attribute 实例中。Postgres 的某些部分使用 pg_type 的副本，
 *		而另一些部分使用 pg_attribute 的副本，因此两者必须保持一致。
 *		详见 struct FormData_pg_attribute。
 * ----------------
 */
CATALOG(pg_type,1247,TypeRelationId) BKI_BOOTSTRAP BKI_ROWTYPE_OID(71,TypeRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	Oid			oid;			/* OID */

	/* 类型名称 */
	NameData	typname;

	/* 包含此类型的命名空间的 OID */
	Oid			typnamespace BKI_DEFAULT(pg_catalog) BKI_LOOKUP(pg_namespace);

	/* 类型所有者 */
	Oid			typowner BKI_DEFAULT(POSTGRES) BKI_LOOKUP(pg_authid);

	/*
	 * 对于定长类型，typlen 表示我们用多少字节来表示该类型的一个值，
	 * 例如 int4 为 4。而对于变长类型，typlen 为负数。我们用 -1 表示
	 * 一种“varlena”类型(带长度字)，用 -2 表示以空字符结尾的 C 字符串。
	 */
	int16		typlen BKI_ARRAY_DEFAULT(-1);

	/*
	 * typbyval 决定 Postgres 内部例程是按值还是按引用传递该类型的值。
	 * 若长度不是 1、2 或 4(或在 8 字节 Datum 机器上不是 8)，
	 * typbyval 最好为 false。变长类型总是按引用传递。注意，
	 * 即使长度允许按值传递，typbyval 仍可以为 false；
	 * 例如 macaddr8 类型在 Datum 为 8 字节时仍然按引用传递。
	 */
	bool		typbyval BKI_ARRAY_DEFAULT(f);

	/*
	 * typtype 为 'b' 表示基类型，'c' 表示组合类型(如表行类型)，
	 * 'd' 表示域，'e' 表示枚举类型，'p' 表示伪类型，
	 * 或 'r' 表示范围类型。(使用下方的 TYPTYPE 宏。)
	 *
	 * 若 typtype 为 'c'，则 typrelid 是该类在 pg_class 中条目的 OID。
	 */
	char		typtype BKI_DEFAULT(b) BKI_ARRAY_DEFAULT(b);

	/*
	 * typcategory 与 typispreferred 帮助解析器区分首选与非首选的类型
	 * 强制转换。类别可以是任意单个 ASCII 字符(但不能是 \0)。
	 * 内置类型所使用的类别由下方的 TYPCATEGORY 宏标识。
	 */

	/* 任意类型分类 */
	char		typcategory BKI_ARRAY_DEFAULT(A);

	/* 该类型在其类别中是否“首选”？ */
	bool		typispreferred BKI_DEFAULT(f) BKI_ARRAY_DEFAULT(f);

	/*
	 * 若 typisdefined 为 false，则该条目仅是一个占位符(前向引用)。
	 * 此时我们只知道该类型的名称和所有者，对其余信息尚一无所知。
	 */
	bool		typisdefined BKI_DEFAULT(t);

	/* 该类型数组的分隔符 */
	char		typdelim BKI_DEFAULT(',');

	/* 若是组合类型则为关联的 pg_class OID，否则为 0 */
	Oid			typrelid BKI_DEFAULT(0) BKI_ARRAY_DEFAULT(0) BKI_LOOKUP_OPT(pg_class);

	/*
	 * 类型特定的下标处理函数。若 typsubscript 为 0，表示该类型不支持
	 * 下标访问。注意，系统中多处仅当某类型的 typsubscript 为
	 * array_subscript_handler 时才将其视为“真正的”数组类型。
	 */
	regproc		typsubscript BKI_DEFAULT(-) BKI_ARRAY_DEFAULT(array_subscript_handler) BKI_LOOKUP_OPT(pg_proc);

	/*
	 * 若 typelem 非 0，则它指向 pg_type 中的另一行，定义下标访问
	 * 所得的类型。若 typsubscript 为 0，则此处也应为 0。但若处理函数
	 * 不需要 typelem 来确定下标结果类型，则即使 typsubscript 非 0，
	 * typelem 也可以为 0。注意，typelem 依赖被视为隐含该元素类型
	 * 在本类型中的物理包含；因此对该元素类型的 DDL 变更可能会受到
	 * 本类型存在的限制。
	 */
	Oid			typelem BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_type);

	/*
	 * 若存在以本类型为元素类型的“真正的”数组类型，typarray 指向它。
	 * 若没有关联的“真正的”数组类型则为 0。
	 */
	Oid			typarray BKI_DEFAULT(0) BKI_ARRAY_DEFAULT(0) BKI_LOOKUP_OPT(pg_type);

	/*
	 * 该数据类型的 I/O 转换过程。
	 */

	/* 文本格式(必需) */
	regproc		typinput BKI_ARRAY_DEFAULT(array_in) BKI_LOOKUP(pg_proc);
	regproc		typoutput BKI_ARRAY_DEFAULT(array_out) BKI_LOOKUP(pg_proc);

	/* 二进制格式(可选) */
	regproc		typreceive BKI_ARRAY_DEFAULT(array_recv) BKI_LOOKUP_OPT(pg_proc);
	regproc		typsend BKI_ARRAY_DEFAULT(array_send) BKI_LOOKUP_OPT(pg_proc);

	/*
	 * 可选类型修饰符的 I/O 函数。
	 */
	regproc		typmodin BKI_DEFAULT(-) BKI_LOOKUP_OPT(pg_proc);
	regproc		typmodout BKI_DEFAULT(-) BKI_LOOKUP_OPT(pg_proc);

	/*
	 * 用于该数据类型的自定义 ANALYZE 过程(0 表示选择默认过程)。
	 */
	regproc		typanalyze BKI_DEFAULT(-) BKI_ARRAY_DEFAULT(array_typanalyze) BKI_LOOKUP_OPT(pg_proc);

	/* ----------------
	 * typalign 是存储该类型值时所需的对齐方式。它既适用于磁盘上的
	 * 存储，也适用于 Postgres 内部该值的大多数表示形式。当多个值
	 * 连续存储时(例如在磁盘上完整行的表示中)，会在本类型 datum 之前
	 * 插入填充，使其从指定的边界开始。对齐的参考点是序列中第一个
	 * datum 的起始位置。
	 *
	 * 'c' = CHAR 对齐，即无需对齐。
	 * 's' = SHORT 对齐(在大多数机器上为 2 字节)。
	 * 'i' = INT 对齐(在大多数机器上为 4 字节)。
	 * 'd' = DOUBLE 对齐(在许多机器上为 8 字节，但并非全部)。
	 * (对这些值使用下方的 TYPALIGN 宏。)
	 *
	 * 计算这些对齐需求的宏参见 include/access/tupmacs.h。另请注意，
	 * 在存储“打包”的 varlena 时，我们允许名义对齐被打破；TOAST 机制
	 * 负责向大多数代码隐藏这一点。
	 *
	 * 注意：对于系统表中使用的类型，pg_type 中定义的大小与对齐方式，
	 * 必须与编译器在表示表行结构体中对该字段的布局方式保持一致。
	 * ----------------
	 */
	char		typalign;

	/* ----------------
	 * typstorage 说明该类型是否做好了 toasting 准备，以及该类型属性
	 * 的默认策略应该是什么。
	 *
	 * 'p' PLAIN	  未做 toasting 准备的类型
	 * 'e' EXTERNAL   可外部存储，不做压缩尝试
	 * 'x' EXTENDED   必要时尝试压缩并外部存储
	 * 'm' MAIN		  与 'x' 类似，但尽量保留在主元组中
	 * (对这些值使用下方的 TYPSTORAGE 宏。)
	 *
	 * 注意，'m' 字段也可以被移出到二级存储，但仅作为最后手段
	 * ('e' 和 'x' 字段会优先被移出)。
	 * ----------------
	 */
	char		typstorage BKI_DEFAULT(p) BKI_ARRAY_DEFAULT(x);

	/*
	 * 该标志表示该数据类型上的“NOT NULL”约束。
	 *
	 * 若为 true，则使用此数据类型的相应表列的 attnotnull 字段将始终
	 * 强制执行 NOT NULL 约束。
	 *
	 * 主要用于域类型。
	 */
	bool		typnotnull BKI_DEFAULT(f);

	/*
	 * 域使用 typbasetype 来指明其所基于的基类型(或域类型)。
	 * 若该类型不是域，则为 0。
	 */
	Oid			typbasetype BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_type);

	/*
	 * 域使用 typtypmod 记录应用到其基类型的 typmod
	 * (若基类型不使用 typmod 则为 -1)。若该类型不是域则为 -1。
	 */
	int32		typtypmod BKI_DEFAULT(-1);

	/*
	 * typndims 是数组域类型(即 typbasetype 为数组类型)声明的维度数。
	 * 否则为 0。
	 */
	int32		typndims BKI_DEFAULT(0);

	/*
	 * 排序规则：若该类型不能使用排序规则则为 0；对于可排序的基类型
	 * 为非零(通常是 DEFAULT_COLLATION_OID)；对于建立在可排序类型之上的
	 * 域，可能为其他某个 OID。
	 */
	Oid			typcollation BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_collation);

#ifdef CATALOG_VARLEN			/* 变长字段自此开始 */

	/*
	 * 若 typdefaultbin 非 NULL，它是该类型默认表达式的 nodeToString
	 * 表示。目前仅用于域。
	 */
	pg_node_tree typdefaultbin BKI_DEFAULT(_null_) BKI_ARRAY_DEFAULT(_null_);

	/*
	 * 若类型没有关联的默认值，则 typdefault 为 NULL。若 typdefaultbin
	 * 非 NULL，则 typdefault 必须包含由 typdefaultbin 所表示默认表达式
	 * 的可读版本。若 typdefaultbin 为 NULL 而 typdefault 非 NULL，则
	 * typdefault 是该类型默认值的外部表示，可喂给该类型的输入转换器
	 * 以生成一个常量。
	 */
	text		typdefault BKI_DEFAULT(_null_) BKI_ARRAY_DEFAULT(_null_);

	/*
	 * 访问权限
	 */
	aclitem		typacl[1] BKI_DEFAULT(_null_);
#endif
} FormData_pg_type;

/* ----------------
 *		Form_pg_type 对应于一个指向具有
 *		pg_type 关系格式行的指针。
 * ----------------
 */
typedef FormData_pg_type *Form_pg_type;

DECLARE_TOAST(pg_type, 4171, 4172);

DECLARE_UNIQUE_INDEX_PKEY(pg_type_oid_index, 2703, TypeOidIndexId, pg_type, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_type_typname_nsp_index, 2704, TypeNameNspIndexId, pg_type, btree(typname name_ops, typnamespace oid_ops));

MAKE_SYSCACHE(TYPEOID, pg_type_oid_index, 64);
MAKE_SYSCACHE(TYPENAMENSP, pg_type_typname_nsp_index, 64);

#ifdef EXPOSE_TO_CLIENT_CODE

/*
 * 用于“穷人版枚举类型”列的宏的取值
 */
#define  TYPTYPE_BASE		'b' /* base type (ordinary scalar type) */
#define  TYPTYPE_COMPOSITE	'c' /* composite (e.g., table's rowtype) */
#define  TYPTYPE_DOMAIN		'd' /* domain over another type */
#define  TYPTYPE_ENUM		'e' /* enumerated type */
#define  TYPTYPE_MULTIRANGE	'm' /* multirange type */
#define  TYPTYPE_PSEUDO		'p' /* pseudo-type */
#define  TYPTYPE_RANGE		'r' /* range type */

#define  TYPCATEGORY_INVALID	'\0'	/* not an allowed category */
#define  TYPCATEGORY_ARRAY		'A'
#define  TYPCATEGORY_BOOLEAN	'B'
#define  TYPCATEGORY_COMPOSITE	'C'
#define  TYPCATEGORY_DATETIME	'D'
#define  TYPCATEGORY_ENUM		'E'
#define  TYPCATEGORY_GEOMETRIC	'G'
#define  TYPCATEGORY_NETWORK	'I' /* think INET */
#define  TYPCATEGORY_NUMERIC	'N'
#define  TYPCATEGORY_PSEUDOTYPE 'P'
#define  TYPCATEGORY_RANGE		'R'
#define  TYPCATEGORY_STRING		'S'
#define  TYPCATEGORY_TIMESPAN	'T'
#define  TYPCATEGORY_USER		'U'
#define  TYPCATEGORY_BITSTRING	'V' /* er ... "varbit"? */
#define  TYPCATEGORY_UNKNOWN	'X'
#define  TYPCATEGORY_INTERNAL	'Z'

#define  TYPALIGN_CHAR			'c' /* char alignment (i.e. unaligned) */
#define  TYPALIGN_SHORT			's' /* short alignment (typically 2 bytes) */
#define  TYPALIGN_INT			'i' /* int alignment (typically 4 bytes) */
#define  TYPALIGN_DOUBLE		'd' /* double alignment (often 8 bytes) */

#define  TYPSTORAGE_PLAIN		'p' /* type not prepared for toasting */
#define  TYPSTORAGE_EXTERNAL	'e' /* toastable, don't try to compress */
#define  TYPSTORAGE_EXTENDED	'x' /* fully toastable */
#define  TYPSTORAGE_MAIN		'm' /* like 'x' but try to store inline */

/* 一个类型 OID 是否为多态伪类型？	(注意多次求值问题) */
#define IsPolymorphicType(typid)  \
	(IsPolymorphicTypeFamily1(typid) || \
	 IsPolymorphicTypeFamily2(typid))

/* 非多态类型解析的代码不应使用这些宏： */
#define IsPolymorphicTypeFamily1(typid)  \
	((typid) == ANYELEMENTOID || \
	 (typid) == ANYARRAYOID || \
	 (typid) == ANYNONARRAYOID || \
	 (typid) == ANYENUMOID || \
	 (typid) == ANYRANGEOID || \
	 (typid) == ANYMULTIRANGEOID)

#define IsPolymorphicTypeFamily2(typid)  \
	((typid) == ANYCOMPATIBLEOID || \
	 (typid) == ANYCOMPATIBLEARRAYOID || \
	 (typid) == ANYCOMPATIBLENONARRAYOID || \
	 (typid) == ANYCOMPATIBLERANGEOID || \
	 (typid) == ANYCOMPATIBLEMULTIRANGEOID)

/* 这是否是一个“真正的”数组类型？(需要 fmgroids.h) */
#define IsTrueArrayType(typeForm)  \
	(OidIsValid((typeForm)->typelem) && \
	 (typeForm)->typsubscript == F_ARRAY_SUBSCRIPT_HANDLER)

/*
 * 对 pg_type OID 宏各种古老随意拼写的向后兼容。
 * 新代码中不要使用这些名称。
 */
#define CASHOID	MONEYOID
#define LSNOID	PG_LSNOID

#endif							/* EXPOSE_TO_CLIENT_CODE */


extern ObjectAddress TypeShellMake(const char *typeName,
								   Oid typeNamespace,
								   Oid ownerId);

extern ObjectAddress TypeCreate(Oid newTypeOid,
								const char *typeName,
								Oid typeNamespace,
								Oid relationOid,
								char relationKind,
								Oid ownerId,
								int16 internalSize,
								char typeType,
								char typeCategory,
								bool typePreferred,
								char typDelim,
								Oid inputProcedure,
								Oid outputProcedure,
								Oid receiveProcedure,
								Oid sendProcedure,
								Oid typmodinProcedure,
								Oid typmodoutProcedure,
								Oid analyzeProcedure,
								Oid subscriptProcedure,
								Oid elementType,
								bool isImplicitArray,
								Oid arrayType,
								Oid baseType,
								const char *defaultTypeValue,
								char *defaultTypeBin,
								bool passedByValue,
								char alignment,
								char storage,
								int32 typeMod,
								int32 typNDims,
								bool typeNotNull,
								Oid typeCollation);

extern void GenerateTypeDependencies(HeapTuple typeTuple,
									 Relation typeCatalog,
									 Node *defaultExpr,
									 void *typacl,
									 char relationKind, /* only for relation
														 * rowtypes */
									 bool isImplicitArray,
									 bool isDependentType,
									 bool makeExtensionDep,
									 bool rebuild);

extern void RenameTypeInternal(Oid typeOid, const char *newTypeName,
							   Oid typeNamespace);

extern char *makeArrayTypeName(const char *typeName, Oid typeNamespace);

extern bool moveArrayTypeName(Oid typeOid, const char *typeName,
							  Oid typeNamespace);

extern char *makeMultirangeTypeName(const char *rangeTypeName,
									Oid typeNamespace);

#endif							/* PG_TYPE_H */
