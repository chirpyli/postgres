/*-------------------------------------------------------------------------
 *
 * printsimple.c
 *	  本文件包含用于在 不访问系统目录 的情况下打印只包含有限范围
 *	  内建类型的元组的例程。它面向那些因未绑定到特定数据库（例如某些
 *	  walsender 进程）而没有目录访问权限的后端。它不处理独立（standalone）
 *	  后端以及除 3.0 以外的协议版本，因为当前的应用并不需要此类处理。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/common/printsimple.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/printsimple.h"
#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "libpq/protocol.h"
#include "utils/builtins.h"

/*
 * 在启动时，发送一个 RowDescription 消息。
 */
void
printsimple_startup(DestReceiver *self, int operation, TupleDesc tupdesc)
{
	StringInfoData buf;
	int			i;

	pq_beginmessage(&buf, PqMsg_RowDescription);
	pq_sendint16(&buf, tupdesc->natts);

	for (i = 0; i < tupdesc->natts; ++i)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		pq_sendstring(&buf, NameStr(attr->attname));
	pq_sendint32(&buf, 0);	/* 表 oid */
	pq_sendint16(&buf, 0);	/* attnum */
		pq_sendint32(&buf, (int) attr->atttypid);
		pq_sendint16(&buf, attr->attlen);
		pq_sendint32(&buf, attr->atttypmod);
		pq_sendint16(&buf, 0);	/* format code */
	}

	pq_endmessage(&buf);
}

/*
 * 对于每个元组，发送一个 DataRow 消息。
 */
bool
printsimple(TupleTableSlot *slot, DestReceiver *self)
{
	TupleDesc	tupdesc = slot->tts_tupleDescriptor;
	StringInfoData buf;
	int			i;

	/* 确保元组已被完全解构 */
	slot_getallattrs(slot);

	/* 准备并发送消息 */
	pq_beginmessage(&buf, PqMsg_DataRow);
	pq_sendint16(&buf, tupdesc->natts);

	for (i = 0; i < tupdesc->natts; ++i)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
		Datum		value;

		if (slot->tts_isnull[i])
		{
			pq_sendint32(&buf, -1);
			continue;
		}

		value = slot->tts_values[i];

		/*
		 * 我们不能在这里调用常规的类型输出函数，因为我们可能没有
		 * 目录访问权限。因此，我们必须将所需类型的知识硬编码（hard-wire）
		 * 到代码中。
		 */
		switch (attr->atttypid)
		{
			case TEXTOID:
				{
					text	   *t = DatumGetTextPP(value);

					pq_sendcountedtext(&buf,
									   VARDATA_ANY(t),
									   VARSIZE_ANY_EXHDR(t));
				}
				break;

			case INT4OID:
				{
					int32		num = DatumGetInt32(value);
					char		str[12];	/* 符号位、10 位数字以及 '\0' */
					int			len;

					len = pg_ltoa(num, str);
					pq_sendcountedtext(&buf, str, len);
				}
				break;

			case INT8OID:
				{
					int64		num = DatumGetInt64(value);
					char		str[MAXINT8LEN + 1];
					int			len;

					len = pg_lltoa(num, str);
					pq_sendcountedtext(&buf, str, len);
				}
				break;

			case OIDOID:
				{
					Oid			num = ObjectIdGetDatum(value);
					char		str[10];	/* 10 位数字 */
					int			len;

					len = pg_ultoa_n(num, str);
					pq_sendcountedtext(&buf, str, len);
				}
				break;

			default:
				elog(ERROR, "unsupported type OID: %u", attr->atttypid);
		}
	}

	pq_endmessage(&buf);

	return true;
}
