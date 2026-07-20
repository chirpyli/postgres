/*-------------------------------------------------------------------------
 *
 * buf.h
 *	  缓冲区管理器的基础数据类型。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/buf.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUF_H
#define BUF_H

/*
 * 缓冲区标识符。
 *
 * 0 表示无效；正数表示共享缓冲区的索引（1..NBuffers）；
 * 负数表示本地缓冲区的索引（-1 .. -NLocBuffer）。
 */
typedef int Buffer;

#define InvalidBuffer	0

/*
 * BufferIsInvalid
 *		当且仅当缓冲区无效时为真。
 */
#define BufferIsInvalid(buffer) ((buffer) == InvalidBuffer)

/*
 * BufferIsLocal
 *		当且仅当缓冲区为本地缓冲区时为真（对其他后端不可见）。
 */
#define BufferIsLocal(buffer)	((buffer) < 0)

/*
 * 缓冲区访问策略对象。
 *
 * BufferAccessStrategyData 的定义对 freelist.c 私有
 */
typedef struct BufferAccessStrategyData *BufferAccessStrategy;

#endif							/* BUF_H */
