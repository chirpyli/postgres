/*-------------------------------------------------------------------------
 *
 * storage_xlog.h
 *	  backend/catalog/storage.c 的 XLog 支持的声明
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/storage_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STORAGE_XLOG_H
#define STORAGE_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"

/*
 * 与 smgr 相关的 XLOG 记录的声明
 *
 * 注意：我们在此记录文件的创建与截断，但删除动作的记录
 * 由 xact.c 处理，因为它属于事务提交的一部分。
 */

/* XLOG 占用高 4 位 */
#define XLOG_SMGR_CREATE	0x10
#define XLOG_SMGR_TRUNCATE	0x20

typedef struct xl_smgr_create
{
	RelFileLocator rlocator;
	ForkNumber	forkNum;
} xl_smgr_create;

/* xl_smgr_truncate 的标志位 */
#define SMGR_TRUNCATE_HEAP		0x0001
#define SMGR_TRUNCATE_VM		0x0002
#define SMGR_TRUNCATE_FSM		0x0004
#define SMGR_TRUNCATE_ALL		\
	(SMGR_TRUNCATE_HEAP|SMGR_TRUNCATE_VM|SMGR_TRUNCATE_FSM)

typedef struct xl_smgr_truncate
{
	BlockNumber blkno;
	RelFileLocator rlocator;
	int			flags;
} xl_smgr_truncate;

extern void log_smgrcreate(const RelFileLocator *rlocator, ForkNumber forkNum);

extern void smgr_redo(XLogReaderState *record);
extern void smgr_desc(StringInfo buf, XLogReaderState *record);
extern const char *smgr_identify(uint8 info);

#endif							/* STORAGE_XLOG_H */
