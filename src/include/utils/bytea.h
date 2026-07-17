/*-------------------------------------------------------------------------
 *
 * bytea.h
 *	  BYTEA 数据类型支持的声明。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/bytea.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BYTEA_H
#define BYTEA_H



typedef enum
{
	BYTEA_OUTPUT_ESCAPE,
	BYTEA_OUTPUT_HEX,
}			ByteaOutputType;

extern PGDLLIMPORT int bytea_output;	/* 类型本为 ByteaOutputType，
										 * 但为 GUC 而用 int 表示 */

#endif							/* BYTEA_H */
