/*-------------------------------------------------------------------------
 *
 * postgres_fe.h
 *	  PostgreSQL 客户端 .c 文件的主包含文件
 *
 * 这应当是 PostgreSQL 客户端库和应用程序的第一个包含文件——但不应用于
 * 后端模块，后端模块应包含 postgres.h。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * src/include/postgres_fe.h
 *
 *-------------------------------------------------------------------------
 */
/* IWYU pragma: always_keep */
#ifndef POSTGRES_FE_H
#define POSTGRES_FE_H

#ifndef FRONTEND
#define FRONTEND 1
#endif

/* IWYU pragma: begin_exports */

#include "c.h"

#include "common/fe_memutils.h"

/* IWYU pragma: end_exports */

#endif							/* POSTGRES_FE_H */
