/*-------------------------------------------------------------------------
 *
 * printtup.c
 *	  将元组打印到目标端的例程（这里同时支持前端客户端与独立后端）。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/common/printtup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/printtup.h"
#include "libpq/pqformat.h"
#include "libpq/protocol.h"
#include "tcop/pquery.h"
#include "utils/lsyscache.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"


static void printtup_startup(DestReceiver *self, int operation,
							 TupleDesc typeinfo);
static bool printtup(TupleTableSlot *slot, DestReceiver *self);
static void printtup_shutdown(DestReceiver *self);
static void printtup_destroy(DestReceiver *self);

/* ----------------------------------------------------------------
 *		printtup / debugtup 支持
 * ----------------------------------------------------------------
 */

/* ----------------
 *		printtup 目标对象的私有状态
 *
 * 注意：finfo 是 typoutput 或 typsend（我们为此列所用的那个）的查找信息。
 * ----------------
 */
typedef struct
{								/* 每属性的信息 */
	Oid			typoutput;		/* 该类型文本输出函数的 OID */
	Oid			typsend;		/* 该类型二进制输出函数的 OID */
	bool		typisvarlena;	/* 是否为 varlena（即可能可 toast）？ */
	int16		format;			/* 此列的格式码 */
	FmgrInfo	finfo;			/* 输出函数的预计算调用信息 */
} PrinttupAttrInfo;

typedef struct
{
	DestReceiver pub;			/* 公开的函数指针 */
	Portal		portal;			/* 我们正从中打印的 Portal */
	bool		sendDescrip;	/* 启动时是否发送 RowDescription？ */
	TupleDesc	attrinfo;		/* 我们为其建立的属性信息 */
	int			nattrs;
	PrinttupAttrInfo *myinfo;	/* 关于每个属性的缓存信息 */
	StringInfoData buf;			/* 输出缓冲区（*不*在 tmpcontext 中） */
	MemoryContext tmpcontext;	/* 每行工作区的内存上下文 */
} DR_printtup;

/* ----------------
 *		初始化：为 printtup 创建一个 DestReceiver
 * ----------------
 */
DestReceiver *
printtup_create_DR(CommandDest dest)
{
	DR_printtup *self = (DR_printtup *) palloc0(sizeof(DR_printtup));

	self->pub.receiveSlot = printtup;	/* 稍后可能会被改掉 */
	self->pub.rStartup = printtup_startup;
	self->pub.rShutdown = printtup_shutdown;
	self->pub.rDestroy = printtup_destroy;
	self->pub.mydest = dest;

	/*
	 * 如果是 DestRemote 则自动发送 T 消息，但 DestRemoteExecute 不发送
	 */
	self->sendDescrip = (dest == DestRemote);

	self->attrinfo = NULL;
	self->nattrs = 0;
	self->myinfo = NULL;
	self->buf.data = NULL;
	self->tmpcontext = NULL;

	return (DestReceiver *) self;
}

/*
 * Set parameters for a DestRemote (or DestRemoteExecute) receiver
 */
void
SetRemoteDestReceiverParams(DestReceiver *self, Portal portal)
{
	DR_printtup *myState = (DR_printtup *) self;

	Assert(myState->pub.mydest == DestRemote ||
		   myState->pub.mydest == DestRemoteExecute);

	myState->portal = portal;
}

static void
printtup_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	DR_printtup *myState = (DR_printtup *) self;
	Portal		portal = myState->portal;

	/*
	 * 创建供所有消息使用的 I/O 缓冲区。它不能位于 tmpcontext 内，
	 * 因为我们要在多个行之间复用它。
	 */
	initStringInfo(&myState->buf);

	/*
	 * 创建一个临时内存上下文，每行结束时重置一次以回收 palloc 分配的内存。
	 * 这可以避免数据类型输出例程内部发生内存泄漏的问题，而且无论如何
	 * 都应比逐个零售式 pfree 更快。
	 */
	myState->tmpcontext = AllocSetContextCreate(CurrentMemoryContext,
												"printtup",
												ALLOCSET_DEFAULT_SIZES);

	/*
	 * If we are supposed to emit row descriptions, then send the tuple
	 * descriptor of the tuples.
	 */
	if (myState->sendDescrip)
		SendRowDescriptionMessage(&myState->buf,
								  typeinfo,
								  FetchPortalTargetList(portal),
								  portal->formats);

	/* ----------------
	 * We could set up the derived attr info at this time, but we postpone it
	 * until the first call of printtup, for 2 reasons:
	 * 1. We don't waste time (compared to the old way) if there are no
	 *	  tuples at all to output.
	 * 2. Checking in printtup allows us to handle the case that the tuples
	 *	  change type midway through (although this probably can't happen in
	 *	  the current executor).
	 * ----------------
	 */
}

/*
 * SendRowDescriptionMessage --- 向前端发送一个 RowDescription 消息
 *
 * 注意：TupleDesc 通常是由 ExecTypeFromTL() 或类似函数构造出来的；
 * 它并不包含完整的一组字段。当执行一个没有执行计划的工具函数时，
 * targetlist 为 NIL。如果 targetlist 不是 NIL，则它是一个 Query 节点的
 * targetlist；忽略其中的 resjunk 列由我们负责。formats[] 数组指针
 * 可能为 NULL（例如在对一个预备语句做 Describe 时）；这种情况发送
 * 全零的格式码即可。
 */
void
SendRowDescriptionMessage(StringInfo buf, TupleDesc typeinfo,
						  List *targetlist, int16 *formats)
{
	int			natts = typeinfo->natts;
	int			i;
	ListCell   *tlist_item = list_head(targetlist);

	/* 元组描述符消息类型 */
	pq_beginmessage_reuse(buf, PqMsg_RowDescription);
	/* 元组中的属性数量 */
	pq_sendint16(buf, natts);

	/*
	 * 为将要发送的完整消息预分配内存。这样可以使用显著更快的内联
	 * pqformat.h 函数，并避免反复 realloc。
	 *
	 * 必须高估列名的大小，以考虑字符集带来的额外开销。
	 */
	enlargeStringInfo(buf, (NAMEDATALEN * MAX_CONVERSION_GROWTH /* attname */
							+ sizeof(Oid)	/* resorigtbl */
							+ sizeof(AttrNumber)	/* resorigcol */
							+ sizeof(Oid)	/* atttypid */
							+ sizeof(int16) /* attlen */
							+ sizeof(int32) /* attypmod */
							+ sizeof(int16) /* format */
							) * natts);

	for (i = 0; i < natts; ++i)
	{
		Form_pg_attribute att = TupleDescAttr(typeinfo, i);
		Oid			atttypid = att->atttypid;
		int32		atttypmod = att->atttypmod;
		Oid			resorigtbl;
		AttrNumber	resorigcol;
		int16		format;

		/*
		 * 如果列是一个域，则发送其基类型和 typmod。
		 * 为效率起见，在任何整数发送之前完成查找。
		 */
		atttypid = getBaseTypeAndTypmod(atttypid, &atttypmod);

		/* 我们是否有一个非 resjunk 的 tlist 项？ */
		while (tlist_item &&
			   ((TargetEntry *) lfirst(tlist_item))->resjunk)
			tlist_item = lnext(targetlist, tlist_item);
		if (tlist_item)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tlist_item);

			resorigtbl = tle->resorigtbl;
			resorigcol = tle->resorigcol;
			tlist_item = lnext(targetlist, tlist_item);
		}
		else
		{
			/* 没有可用信息，因此发送零值 */
			resorigtbl = 0;
			resorigcol = 0;
		}

		if (formats)
			format = formats[i];
		else
			format = 0;

		pq_writestring(buf, NameStr(att->attname));
		pq_writeint32(buf, resorigtbl);
		pq_writeint16(buf, resorigcol);
		pq_writeint32(buf, atttypid);
		pq_writeint16(buf, att->attlen);
		pq_writeint32(buf, atttypmod);
		pq_writeint16(buf, format);
	}

	pq_endmessage_reuse(buf);
}

/*
 * 获取 printtup() 所需的查找信息
 */
static void
printtup_prepare_info(DR_printtup *myState, TupleDesc typeinfo, int numAttrs)
{
	int16	   *formats = myState->portal->formats;
	int			i;

	/* 清除任何旧数据 */
	if (myState->myinfo)
		pfree(myState->myinfo);
	myState->myinfo = NULL;

	myState->attrinfo = typeinfo;
	myState->nattrs = numAttrs;
	if (numAttrs <= 0)
		return;

	myState->myinfo = (PrinttupAttrInfo *)
		palloc0(numAttrs * sizeof(PrinttupAttrInfo));

	for (i = 0; i < numAttrs; i++)
	{
		PrinttupAttrInfo *thisState = myState->myinfo + i;
		int16		format = (formats ? formats[i] : 0);
		Form_pg_attribute attr = TupleDescAttr(typeinfo, i);

		thisState->format = format;
		if (format == 0)
		{
			getTypeOutputInfo(attr->atttypid,
							  &thisState->typoutput,
							  &thisState->typisvarlena);
			fmgr_info(thisState->typoutput, &thisState->finfo);
		}
		else if (format == 1)
		{
			getTypeBinaryOutputInfo(attr->atttypid,
									&thisState->typsend,
									&thisState->typisvarlena);
			fmgr_info(thisState->typsend, &thisState->finfo);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsupported format code: %d", format)));
	}
}

/* ----------------
 *		printtup --- 向客户端发送一个元组
 *
 * 注意：如果你修改这个函数，也请参见 explain.c 中的
 * serializeAnalyzeReceive，它意在复现这里所做的计算。
 * ----------------
 */
static bool
printtup(TupleTableSlot *slot, DestReceiver *self)
{
	TupleDesc	typeinfo = slot->tts_tupleDescriptor;
	DR_printtup *myState = (DR_printtup *) self;
	MemoryContext oldcontext;
	StringInfo	buf = &myState->buf;
	int			natts = typeinfo->natts;
	int			i;

	/* 如有需要，设置或更新我的派生属性信息 */
	if (myState->attrinfo != typeinfo || myState->nattrs != natts)
		printtup_prepare_info(myState, typeinfo, natts);

	/* 确保元组被完全拆解 */
	slot_getallattrs(slot);

	/* 切换到每行上下文，以便回收下方内存 */
	oldcontext = MemoryContextSwitchTo(myState->tmpcontext);

	/*
	 * 准备一个 DataRow 消息（注意缓冲区位于每查询上下文中）
	 */
	pq_beginmessage_reuse(buf, PqMsg_DataRow);

	pq_sendint16(buf, natts);

	/*
	 * 发送此元组的各个属性
	 */
	for (i = 0; i < natts; ++i)
	{
		PrinttupAttrInfo *thisState = myState->myinfo + i;
		Datum		attr = slot->tts_values[i];

		if (slot->tts_isnull[i])
		{
			pq_sendint32(buf, -1);
			continue;
		}

		/*
		 * 这里捕获返回给客户端、却未落盘的 datum 中的未定义字节；
		 * 参见 PageAddItem() 中相关检查的注释。该测试对于未压缩、
		 * 非外部的 datum 最有用，但在测试新的 C 函数时我们很可能会
		 * 在这里遇到这类 datum。
		 */
		if (thisState->typisvarlena)
			VALGRIND_CHECK_MEM_IS_DEFINED(DatumGetPointer(attr),
										  VARSIZE_ANY(attr));

		if (thisState->format == 0)
		{
			/* 文本输出 */
			char	   *outputstr;

			outputstr = OutputFunctionCall(&thisState->finfo, attr);
			pq_sendcountedtext(buf, outputstr, strlen(outputstr));
		}
		else
		{
			/* 二进制输出 */
			bytea	   *outputbytes;

			outputbytes = SendFunctionCall(&thisState->finfo, attr);
			pq_sendint32(buf, VARSIZE(outputbytes) - VARHDRSZ);
			pq_sendbytes(buf, VARDATA(outputbytes),
						 VARSIZE(outputbytes) - VARHDRSZ);
		}
	}

	pq_endmessage_reuse(buf);

	/* 返回到调用方的上下文，并刷新该行的临时内存 */
	MemoryContextSwitchTo(oldcontext);
	MemoryContextReset(myState->tmpcontext);

	return true;
}

/* ----------------
 *		printtup_shutdown
 * ----------------
 */
static void
printtup_shutdown(DestReceiver *self)
{
	DR_printtup *myState = (DR_printtup *) self;

	if (myState->myinfo)
		pfree(myState->myinfo);
	myState->myinfo = NULL;

	myState->attrinfo = NULL;

	if (myState->buf.data)
		pfree(myState->buf.data);
	myState->buf.data = NULL;

	if (myState->tmpcontext)
		MemoryContextDelete(myState->tmpcontext);
	myState->tmpcontext = NULL;
}

/* ----------------
 *		printtup_destroy
 * ----------------
 */
static void
printtup_destroy(DestReceiver *self)
{
	pfree(self);
}

/* ----------------
 *		printatt
 * ----------------
 */
static void
printatt(unsigned attributeId,
		 Form_pg_attribute attributeP,
		 char *value)
{
	printf("\t%2d: %s%s%s%s\t(typeid = %u, len = %d, typmod = %d, byval = %c)\n",
		   attributeId,
		   NameStr(attributeP->attname),
		   value != NULL ? " = \"" : "",
		   value != NULL ? value : "",
		   value != NULL ? "\"" : "",
		   (unsigned int) (attributeP->atttypid),
		   attributeP->attlen,
		   attributeP->atttypmod,
		   attributeP->attbyval ? 't' : 'f');
}

/* ----------------
 *		debugStartup - 准备为交互式后端打印元组
 * ----------------
 */
void
debugStartup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	int			natts = typeinfo->natts;
	int			i;

	/*
	 * 显示元组的返回类型
	 */
	for (i = 0; i < natts; ++i)
		printatt((unsigned) i + 1, TupleDescAttr(typeinfo, i), NULL);
	printf("\t----\n");
}

/* ----------------
 *		debugtup - 为交互式后端打印一个元组
 * ----------------
 */
bool
debugtup(TupleTableSlot *slot, DestReceiver *self)
{
	TupleDesc	typeinfo = slot->tts_tupleDescriptor;
	int			natts = typeinfo->natts;
	int			i;
	Datum		attr;
	char	   *value;
	bool		isnull;
	Oid			typoutput;
	bool		typisvarlena;

	for (i = 0; i < natts; ++i)
	{
		attr = slot_getattr(slot, i + 1, &isnull);
		if (isnull)
			continue;
		getTypeOutputInfo(TupleDescAttr(typeinfo, i)->atttypid,
						  &typoutput, &typisvarlena);

		value = OidOutputFunctionCall(typoutput, attr);

		printatt((unsigned) i + 1, TupleDescAttr(typeinfo, i), value);
	}
	printf("\t----\n");

	return true;
}
