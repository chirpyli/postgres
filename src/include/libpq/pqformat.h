/*-------------------------------------------------------------------------
 *
 * pqformat.h
 *		前端/后端消息格式化与解析的相关定义
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/pqformat.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQFORMAT_H
#define PQFORMAT_H

#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "port/pg_bswap.h"

extern void pq_beginmessage(StringInfo buf, char msgtype);
extern void pq_beginmessage_reuse(StringInfo buf, char msgtype);
extern void pq_endmessage(StringInfo buf);
extern void pq_endmessage_reuse(StringInfo buf);

extern void pq_sendbytes(StringInfo buf, const void *data, int datalen);
extern void pq_sendcountedtext(StringInfo buf, const char *str, int slen);
extern void pq_sendtext(StringInfo buf, const char *str, int slen);
extern void pq_sendstring(StringInfo buf, const char *str);
extern void pq_send_ascii_string(StringInfo buf, const char *str);
extern void pq_sendfloat4(StringInfo buf, float4 f);
extern void pq_sendfloat8(StringInfo buf, float8 f);

/*
 * 向一个已预分配足够空间的 StringInfo 缓冲区追加一个 [u]int8。
 *
 * 使用 pg_restrict 可让编译器基于 buf、buf->len、buf->data 以及
 * *buf->data 不会重叠的假设来优化代码。若没有该注解，buf->len 等
 * 在后续一系列 pq_writeintN 调用中将无法保留在寄存器中。
 *
 * 使用 StringInfoData * 而非 StringInfo，是因为 MSVC 过于挑剔，
 * 要求在 restrict 前加上一个 *。
 */
static inline void
pq_writeint8(StringInfoData *pg_restrict buf, uint8 i)
{
	uint8		ni = i;

	Assert(buf->len + (int) sizeof(uint8) <= buf->maxlen);
	memcpy((char *pg_restrict) (buf->data + buf->len), &ni, sizeof(uint8));
	buf->len += sizeof(uint8);
}

/*
 * 向一个已预分配足够空间的 StringInfo 缓冲区追加一个 [u]int16。
 */
static inline void
pq_writeint16(StringInfoData *pg_restrict buf, uint16 i)
{
	uint16		ni = pg_hton16(i);

	Assert(buf->len + (int) sizeof(uint16) <= buf->maxlen);
	memcpy((char *pg_restrict) (buf->data + buf->len), &ni, sizeof(uint16));
	buf->len += sizeof(uint16);
}

/*
 * 向一个已预分配足够空间的 StringInfo 缓冲区追加一个 [u]int32。
 */
static inline void
pq_writeint32(StringInfoData *pg_restrict buf, uint32 i)
{
	uint32		ni = pg_hton32(i);

	Assert(buf->len + (int) sizeof(uint32) <= buf->maxlen);
	memcpy((char *pg_restrict) (buf->data + buf->len), &ni, sizeof(uint32));
	buf->len += sizeof(uint32);
}

/*
 * 向一个已预分配足够空间的 StringInfo 缓冲区追加一个 [u]int64。
 */
static inline void
pq_writeint64(StringInfoData *pg_restrict buf, uint64 i)
{
	uint64		ni = pg_hton64(i);

	Assert(buf->len + (int) sizeof(uint64) <= buf->maxlen);
	memcpy((char *pg_restrict) (buf->data + buf->len), &ni, sizeof(uint64));
	buf->len += sizeof(uint64);
}

/*
 * 向一个已预分配空间的缓冲区追加一个以 NUL 结尾的文本字符串（会进行编码转换）。
 *
 * 注意：预分配的空间需要足以容纳转换为客户端编码后的字符串。
 *
 * 注意：传入的文本字符串必须以 NUL 结尾，发送到前端的数据同样如此。
 */
static inline void
pq_writestring(StringInfoData *pg_restrict buf, const char *pg_restrict str)
{
	int			slen = strlen(str);
	char	   *p;

	p = pg_server_to_client(str, slen);
	if (p != str)				/* 是否真正进行了编码转换？ */
		slen = strlen(p);

	Assert(buf->len + slen + 1 <= buf->maxlen);

	memcpy(((char *pg_restrict) buf->data + buf->len), p, slen + 1);
	buf->len += slen + 1;

	if (p != str)
		pfree(p);
}

/* 向 StringInfo 缓冲区追加一个二进制 [u]int8 */
static inline void
pq_sendint8(StringInfo buf, uint8 i)
{
	enlargeStringInfo(buf, sizeof(uint8));
	pq_writeint8(buf, i);
}

/* 向 StringInfo 缓冲区追加一个二进制 [u]int16 */
static inline void
pq_sendint16(StringInfo buf, uint16 i)
{
	enlargeStringInfo(buf, sizeof(uint16));
	pq_writeint16(buf, i);
}

/* 向 StringInfo 缓冲区追加一个二进制 [u]int32 */
static inline void
pq_sendint32(StringInfo buf, uint32 i)
{
	enlargeStringInfo(buf, sizeof(uint32));
	pq_writeint32(buf, i);
}

/* 向 StringInfo 缓冲区追加一个二进制 [u]int64 */
static inline void
pq_sendint64(StringInfo buf, uint64 i)
{
	enlargeStringInfo(buf, sizeof(uint64));
	pq_writeint64(buf, i);
}

/* 向 StringInfo 缓冲区追加一个二进制字节 */
static inline void
pq_sendbyte(StringInfo buf, uint8 byt)
{
	pq_sendint8(buf, byt);
}

/*
 * 向 StringInfo 缓冲区追加一个二进制整数
 *
 * 此函数已废弃，建议优先使用上面的各个函数。
 */
static inline void
pq_sendint(StringInfo buf, uint32 i, int b)
{
	switch (b)
	{
		case 1:
			pq_sendint8(buf, (uint8) i);
			break;
		case 2:
			pq_sendint16(buf, (uint16) i);
			break;
		case 4:
			pq_sendint32(buf, (uint32) i);
			break;
		default:
			elog(ERROR, "unsupported integer size %d", b);
			break;
	}
}


extern void pq_begintypsend(StringInfo buf);
extern bytea *pq_endtypsend(StringInfo buf);

extern void pq_puttextmessage(char msgtype, const char *str);
extern void pq_putemptymessage(char msgtype);

extern int	pq_getmsgbyte(StringInfo msg);
extern unsigned int pq_getmsgint(StringInfo msg, int b);
extern int64 pq_getmsgint64(StringInfo msg);
extern float4 pq_getmsgfloat4(StringInfo msg);
extern float8 pq_getmsgfloat8(StringInfo msg);
extern const char *pq_getmsgbytes(StringInfo msg, int datalen);
extern void pq_copymsgbytes(StringInfo msg, void *buf, int datalen);
extern char *pq_getmsgtext(StringInfo msg, int rawbytes, int *nbytes);
extern const char *pq_getmsgstring(StringInfo msg);
extern const char *pq_getmsgrawstring(StringInfo msg);
extern void pq_getmsgend(StringInfo msg);

#endif							/* PQFORMAT_H */
