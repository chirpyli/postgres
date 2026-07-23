/*-------------------------------------------------------------------------
 *
 * checksum.h
 *	  数据页的校验和实现。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/checksum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CHECKSUM_H
#define CHECKSUM_H

#include "storage/block.h"

/*
 * 计算 Postgres 页面的校验和。页面必须按 4 字节边界对齐。
 */
extern uint16 pg_checksum_page(char *page, BlockNumber blkno);

#endif							/* CHECKSUM_H */
