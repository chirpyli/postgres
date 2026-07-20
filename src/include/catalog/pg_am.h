/*-------------------------------------------------------------------------
 *
 * pg_am.h
 *		"访问方法"（access method）系统目录（pg_am）的定义
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_am.h
 *
 * 说明
 *		Catalog.pm 模块会读取此文件并推导模式信息。
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AM_H
#define PG_AM_H

#include "catalog/genbki.h"
#include "catalog/pg_am_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_am 定义。cpp 会将其转换为
 *		typedef struct FormData_pg_am
 * ----------------
 */
CATALOG(pg_am,2601,AccessMethodRelationId)
{
	Oid			oid;			/* oid */

	/* 访问方法名称 */
	NameData	amname;

	/* 处理器函数 */
	regproc		amhandler BKI_LOOKUP(pg_proc);

	/* 参见下方的 AMTYPE_xxx 常量 */
	char		amtype;
} FormData_pg_am;

/* ----------------
 *		Form_pg_am 对应一个指向具有 pg_am 关系格式的元组的指针。
 * ----------------
 */
typedef FormData_pg_am *Form_pg_am;

DECLARE_UNIQUE_INDEX(pg_am_name_index, 2651, AmNameIndexId, pg_am, btree(amname name_ops));
DECLARE_UNIQUE_INDEX_PKEY(pg_am_oid_index, 2652, AmOidIndexId, pg_am, btree(oid oid_ops));

MAKE_SYSCACHE(AMNAME, pg_am_name_index, 4);
MAKE_SYSCACHE(AMOID, pg_am_oid_index, 4);

#ifdef EXPOSE_TO_CLIENT_CODE

/*
 * amtype 的合法取值
 */
#define AMTYPE_INDEX					'i' /* 索引访问方法 */
#define AMTYPE_TABLE					't' /* 表访问方法 */

#endif							/* EXPOSE_TO_CLIENT_CODE */

#endif							/* PG_AM_H */
