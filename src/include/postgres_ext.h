/*-------------------------------------------------------------------------
 *
 * postgres_ext.h
 *
 *	   本文件包含的内容在 PostgreSQL 各处均可见，且对前端接口库的客户端也
 *	可见。例如，Oid 类型是 libpq 及其他库 API 的一部分。
 *
 *	   特定于某一接口的声明应放在对应接口的头文件中（如 libpq-fe.h）。
 *	本文件仅包含最基础的 Postgres 声明。
 *
 *	   用户编写的 C 函数不属于 "Postgres 外部" 的范畴。这些函数更像是
 *	对后端本身的局部修改，使用 Postgres 内部头文件与后端交互。
 *
 * src/include/postgres_ext.h
 *
 *-------------------------------------------------------------------------
 */
/* IWYU pragma: always_keep */

#ifndef POSTGRES_EXT_H
#define POSTGRES_EXT_H

#include <stdint.h>

/*
 * 对象 ID 是 Postgres 中的基本类型。
 */
typedef unsigned int Oid;

#ifdef __cplusplus
#define InvalidOid		(Oid(0))
#else
#define InvalidOid		((Oid) 0)
#endif

#define OID_MAX  UINT_MAX
/* 需要包含 <limits.h> 才能使用上面的 #define */

#define atooid(x) ((Oid) strtoul((x), NULL, 10))
/* 上面这个宏需要 <stdlib.h> */


/* int64_t 的已废弃名称，先前用于客户端 API 声明 */
typedef int64_t pg_int64;

/*
 * 错误消息字段的标识符。放在这里是为了在前端和后端之间保持通用，
 * 同时也将其导出给 libpq 应用使用。
 */
#define PG_DIAG_SEVERITY		'S'
#define PG_DIAG_SEVERITY_NONLOCALIZED 'V'
#define PG_DIAG_SQLSTATE		'C'
#define PG_DIAG_MESSAGE_PRIMARY 'M'
#define PG_DIAG_MESSAGE_DETAIL	'D'
#define PG_DIAG_MESSAGE_HINT	'H'
#define PG_DIAG_STATEMENT_POSITION 'P'
#define PG_DIAG_INTERNAL_POSITION 'p'
#define PG_DIAG_INTERNAL_QUERY	'q'
#define PG_DIAG_CONTEXT			'W'
#define PG_DIAG_SCHEMA_NAME		's'
#define PG_DIAG_TABLE_NAME		't'
#define PG_DIAG_COLUMN_NAME		'c'
#define PG_DIAG_DATATYPE_NAME	'd'
#define PG_DIAG_CONSTRAINT_NAME 'n'
#define PG_DIAG_SOURCE_FILE		'F'
#define PG_DIAG_SOURCE_LINE		'L'
#define PG_DIAG_SOURCE_FUNCTION 'R'

#endif							/* POSTGRES_EXT_H */
