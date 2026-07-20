/*-------------------------------------------------------------------------
 *
 * reloptions.h
 *	  关系和表空间选项的核心支持（pg_class.reloptions
 *	  和 pg_tablespace.spcoptions）
 *
 * 注意：处理 text-array 形式的 reloptions 值的函数将其声明为 Datum
 * 而非 ArrayType *，以避免在大量底层代码中引入 array.h。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/reloptions.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELOPTIONS_H
#define RELOPTIONS_H

#include "access/amapi.h"
#include "access/htup.h"
#include "access/tupdesc.h"
#include "nodes/pg_list.h"
#include "storage/lock.h"

/* reloptions 支持的类型 */
typedef enum relopt_type
{
	RELOPT_TYPE_BOOL,
	RELOPT_TYPE_INT,
	RELOPT_TYPE_REAL,
	RELOPT_TYPE_ENUM,
	RELOPT_TYPE_STRING,
} relopt_type;

/* reloptions 支持的类别 */
typedef enum relopt_kind
{
	RELOPT_KIND_LOCAL = 0,
	RELOPT_KIND_HEAP = (1 << 0),
	RELOPT_KIND_TOAST = (1 << 1),
	RELOPT_KIND_BTREE = (1 << 2),
	RELOPT_KIND_HASH = (1 << 3),
	RELOPT_KIND_GIN = (1 << 4),
	RELOPT_KIND_GIST = (1 << 5),
	RELOPT_KIND_ATTRIBUTE = (1 << 6),
	RELOPT_KIND_TABLESPACE = (1 << 7),
	RELOPT_KIND_SPGIST = (1 << 8),
	RELOPT_KIND_VIEW = (1 << 9),
	RELOPT_KIND_BRIN = (1 << 10),
	RELOPT_KIND_PARTITIONED = (1 << 11),
	/* 如果添加了新的类别，请确保同时更新 "last_default" */
	RELOPT_KIND_LAST_DEFAULT = RELOPT_KIND_PARTITIONED,
	/* 某些编译器将枚举视为有符号 int，因此不能使用 1 << 31 */
	RELOPT_KIND_MAX = (1 << 30)
} relopt_kind;

/* 堆表允许的 reloption 命名空间 —— 当前仅为 TOAST */
#define HEAP_RELOPT_NAMESPACES { "toast", NULL }

/* 保存共享数据的通用结构体 */
typedef struct relopt_gen
{
	const char *name;			/* 必须位于第一个（用作列表终止
								 * 标记） */
	const char *desc;
	bits32		kinds;
	LOCKMODE	lockmode;
	int			namelen;
	relopt_type type;
} relopt_gen;

/* 保存已解析的值 */
typedef struct relopt_value
{
	relopt_gen *gen;
	bool		isset;
	union
	{
		bool		bool_val;
		int			int_val;
		double		real_val;
		int			enum_val;
		char	   *string_val; /* 单独分配 */
	}			values;
} relopt_value;

/* 特定变量类型的 reloptions 记录 */
typedef struct relopt_bool
{
	relopt_gen	gen;
	bool		default_val;
} relopt_bool;

typedef struct relopt_int
{
	relopt_gen	gen;
	int			default_val;
	int			min;
	int			max;
} relopt_int;

typedef struct relopt_real
{
	relopt_gen	gen;
	double		default_val;
	double		min;
	double		max;
} relopt_real;

/*
 * relopt_enum_elt_def -- 枚举 reloption 的可接受值数组的一个成员。
 */
typedef struct relopt_enum_elt_def
{
	const char *string_val;
	int			symbol_val;
} relopt_enum_elt_def;

typedef struct relopt_enum
{
	relopt_gen	gen;
	relopt_enum_elt_def *members;
	int			default_val;
	const char *detailmsg;
	/* 以 null 结尾的成员数组 */
} relopt_enum;

/* 字符串的校验例程 */
typedef void (*validate_string_relopt) (const char *value);
typedef Size (*fill_string_relopt) (const char *value, void *ptr);

/* 整个选项集合的校验例程 */
typedef void (*relopts_validator) (void *parsed_options, relopt_value *vals, int nvals);

typedef struct relopt_string
{
	relopt_gen	gen;
	int			default_len;
	bool		default_isnull;
	validate_string_relopt validate_cb;
	fill_string_relopt fill_cb;
	char	   *default_val;
} relopt_string;

/* 这是 build_reloptions() 使用的表格数据类型 */
typedef struct
{
	const char *optname;		/* 选项名称 */
	relopt_type opttype;		/* 选项数据类型 */
	int			offset;			/* 结果结构体中字段的偏移量 */

	/*
	 * isset_offset 是结果结构体中一个字段的可选偏移量，用于存储该选项是
	 * 为关系显式设置的值，还是仅仅使用了默认值。在大多数情况下，可以通过
	 * 为 reloption 指定一个特殊的越界默认值（例如，某些整数 reloption 使用
	 * -2）来实现这一点，但这并非总是可行。例如，布尔类型 reloption 无法
	 * 设置一个越界的默认值，因此我们需要另一种方式来发现其值的来源。
	 * 此偏移量仅在赋予大于零的值时才被使用。
	 */
	int			isset_offset;
} relopt_parse_elt;

/* 局部 reloption 定义 */
typedef struct local_relopt
{
	relopt_gen *option;			/* 选项定义 */
	int			offset;			/* 已解析值在 bytea 结构中的偏移量 */
} local_relopt;

/* 用于保存 build_local_reloptions() 所需局部 reloption 数据的结构体 */
typedef struct local_relopts
{
	List	   *options;		/* local_relopt 定义列表 */
	List	   *validators;		/* relopts_validator 回调列表 */
	Size		relopt_struct_size; /* 已解析 bytea 结构体的大小 */
} local_relopts;

/*
 * 用于在选项解析完成后获取字符串 reloption 值的工具宏。
 * 这获取的是指向字符串值本身的指针。"optstruct" 是 StdRdOptions 结构体
 * 或等价物，"member" 是对应于该字符串选项的结构体成员。
 */
#define GET_STRING_RELOPTION(optstruct, member) \
	((optstruct)->member == 0 ? NULL : \
	 (char *)(optstruct) + (optstruct)->member)

extern relopt_kind add_reloption_kind(void);
extern void add_bool_reloption(bits32 kinds, const char *name, const char *desc,
							   bool default_val, LOCKMODE lockmode);
extern void add_int_reloption(bits32 kinds, const char *name, const char *desc,
							  int default_val, int min_val, int max_val,
							  LOCKMODE lockmode);
extern void add_real_reloption(bits32 kinds, const char *name, const char *desc,
							   double default_val, double min_val, double max_val,
							   LOCKMODE lockmode);
extern void add_enum_reloption(bits32 kinds, const char *name, const char *desc,
							   relopt_enum_elt_def *members, int default_val,
							   const char *detailmsg, LOCKMODE lockmode);
extern void add_string_reloption(bits32 kinds, const char *name, const char *desc,
								 const char *default_val, validate_string_relopt validator,
								 LOCKMODE lockmode);

extern void init_local_reloptions(local_relopts *relopts, Size relopt_struct_size);
extern void register_reloptions_validator(local_relopts *relopts,
										  relopts_validator validator);
extern void add_local_bool_reloption(local_relopts *relopts, const char *name,
									 const char *desc, bool default_val,
									 int offset);
extern void add_local_int_reloption(local_relopts *relopts, const char *name,
									const char *desc, int default_val,
									int min_val, int max_val, int offset);
extern void add_local_real_reloption(local_relopts *relopts, const char *name,
									 const char *desc, double default_val,
									 double min_val, double max_val,
									 int offset);
extern void add_local_enum_reloption(local_relopts *relopts,
									 const char *name, const char *desc,
									 relopt_enum_elt_def *members,
									 int default_val, const char *detailmsg,
									 int offset);
extern void add_local_string_reloption(local_relopts *relopts, const char *name,
									   const char *desc,
									   const char *default_val,
									   validate_string_relopt validator,
									   fill_string_relopt filler, int offset);

extern Datum transformRelOptions(Datum oldOptions, List *defList,
								 const char *namspace, const char *const validnsps[],
								 bool acceptOidsOff, bool isReset);
extern List *untransformRelOptions(Datum options);
extern bytea *extractRelOptions(HeapTuple tuple, TupleDesc tupdesc,
								amoptions_function amoptions);
extern void *build_reloptions(Datum reloptions, bool validate,
							  relopt_kind kind,
							  Size relopt_struct_size,
							  const relopt_parse_elt *relopt_elems,
							  int num_relopt_elems);
extern void *build_local_reloptions(local_relopts *relopts, Datum options,
									bool validate);

extern bytea *default_reloptions(Datum reloptions, bool validate,
								 relopt_kind kind);
extern bytea *heap_reloptions(char relkind, Datum reloptions, bool validate);
extern bytea *view_reloptions(Datum reloptions, bool validate);
extern bytea *partitioned_table_reloptions(Datum reloptions, bool validate);
extern bytea *index_reloptions(amoptions_function amoptions, Datum reloptions,
							   bool validate);
extern bytea *attribute_reloptions(Datum reloptions, bool validate);
extern bytea *tablespace_reloptions(Datum reloptions, bool validate);
extern LOCKMODE AlterTableGetRelOptionsLockLevel(List *defList);

#endif							/* RELOPTIONS_H */
