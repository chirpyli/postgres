/*-------------------------------------------------------------------------
 *
 * inv_api.c
 *	  用于操作反转文件系统大对象的例程。本文件
 *	  包含用户级大对象应用接口例程。
 *
 *
 * 注意：我们通过 C 结构体声明来访问 pg_largeobject.data。
 * 这是安全的，因为它紧随 int4 字段 pageno，因此 data 字段将始终
 * 按 4 字节对齐，即使它采用短的 1 字节头格式也是如此。
 * 我们必须对其进行 detoast，因为它很可能处于压缩或短格式状态。
 * 我们还需要检查 NULL 值，因为 initdb 会将 loid 和 pageno 标记为
 * NOT NULL，但不会对 data 这样做。
 *
 * 注意：这些例程中有许多会在 CurrentMemoryContext 中泄漏内存，
 * 后端代码的大部分也是如此。我们期望 CurrentMemoryContext 是
 * 一个短暂的内存上下文。需要在函数调用之间持久化的数据要么保存在
 * CacheMemoryContext 中（对于 Relation 结构体），要么保存在
 * 传递给 inv_open 的内存上下文中（对于 LargeObjectDesc 结构体）。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/large_object/inv_api.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/detoast.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_largeobject.h"
#include "libpq/libpq-fs.h"
#include "miscadmin.h"
#include "storage/large_object.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"


/*
 * GUC：向后兼容标记，用于跳过 LO 权限检查
 */
bool		lo_compat_privileges;

/*
 * 对 pg_largeobject 及其索引的所有访问都使用单一的 Relation 引用。
 * 为保证 relcache 条目保留在缓存中，在子事务内部首次引用时，
 * 我们执行一个略显取巧的操作：将 Relation 引用的所有权
 * 赋给 TopTransactionResourceOwner。
 */
static Relation lo_heap_r = NULL;
static Relation lo_index_r = NULL;


/*
 * 在当前事务中打开 pg_largeobject 及其索引（如果尚未打开）
 */
static void
open_lo_relation(void)
{
	ResourceOwner currentOwner;

	if (lo_heap_r && lo_index_r)
		return;					/* 当前事务中已打开 */

	/* 安排顶层事务拥有这些关系引用 */
	currentOwner = CurrentResourceOwner;
	CurrentResourceOwner = TopTransactionResourceOwner;

	/* 使用 RowExclusiveLock，因为我们可能需要读取或写入 */
	if (lo_heap_r == NULL)
		lo_heap_r = table_open(LargeObjectRelationId, RowExclusiveLock);
	if (lo_index_r == NULL)
		lo_index_r = index_open(LargeObjectLOidPNIndexId, RowExclusiveLock);

	CurrentResourceOwner = currentOwner;
}

/*
 * 在主事务结束时进行清理
 */
void
close_lo_relation(bool isCommit)
{
	if (lo_heap_r || lo_index_r)
	{
		/*
		 * 仅在提交时才需要关闭；否则 abort 清理会处理它
		 */
		if (isCommit)
		{
			ResourceOwner currentOwner;

			currentOwner = CurrentResourceOwner;
			CurrentResourceOwner = TopTransactionResourceOwner;

			if (lo_index_r)
				index_close(lo_index_r, NoLock);
			if (lo_heap_r)
				table_close(lo_heap_r, NoLock);

			CurrentResourceOwner = currentOwner;
		}
		lo_heap_r = NULL;
		lo_index_r = NULL;
	}
}


/*
 * 从 pg_largeobject 元组中提取 data 字段，必要时进行 detoast，
 * 并验证长度是否合理。返回数据指针（bytea *）、数据长度，
 * 以及是否需要 pfree 数据指针的指示。
 */
static void
getdatafield(Form_pg_largeobject tuple,
			 bytea **pdatafield,
			 int *plen,
			 bool *pfreeit)
{
	bytea	   *datafield;
	int			len;
	bool		freeit;

	datafield = &(tuple->data); /* 见文件顶部的注释 */
	freeit = false;
	if (VARATT_IS_EXTENDED(datafield))
	{
		datafield = (bytea *)
			detoast_attr((struct varlena *) datafield);
		freeit = true;
	}
	len = VARSIZE(datafield) - VARHDRSZ;
	if (len < 0 || len > LOBLKSIZE)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pg_largeobject entry for OID %u, page %d has invalid data field size %d",
						tuple->loid, tuple->pageno, len)));
	*pdatafield = datafield;
	*plen = len;
	*pfreeit = freeit;
}


/*
 *	inv_create -- 创建一个新的大对象
 *
 *	参数：
 *	  lobjId - 新大对象要使用的 OID，或 InvalidOid 以自动选取
 *
 *	返回值：
 *	  新对象的 OID
 *
 * 如果 lobjId 不是 InvalidOid，且该 OID 已被使用，则报错。
 */
Oid
inv_create(Oid lobjId)
{
	Oid			lobjId_new;

	/*
	 * 创建一个带有空数据页的新 largeobject
	 */
	lobjId_new = LargeObjectCreate(lobjId);

	/*
	 * 大对象所有者的依赖关系
	 *
	 * 注意：出于向后兼容的原因，LO 依赖关系使用 classId
	 * LargeObjectRelationId 来记录。使用 LargeObjectMetadataRelationId
	 * 可以简化后端的逻辑，但会使 pg_dump 变得复杂，并可能破坏其他客户端。
	 */
	recordDependencyOnOwner(LargeObjectRelationId,
							lobjId_new, GetUserId());

	/* 新大对象的创建后钩子 */
	InvokeObjectPostCreateHook(LargeObjectRelationId, lobjId_new, 0);

	/*
	 * 推进命令计数器，使新元组对后续操作可见。
	 */
	CommandCounterIncrement();

	return lobjId_new;
}

/*
 *	inv_open -- 访问一个现有的大对象。
 *
 * 返回一个适当填充的大对象描述符。
 * 描述符及其附属数据分配在指定的内存上下文中，
 * 该上下文必须具有足够长的生命周期以满足调用方的需求。
 * 如果返回的描述符关联了一个快照，调用方必须确保它也能存活足够长的时间，
 * 例如通过调用 RegisterSnapshotOnOwner。
 */
LargeObjectDesc *
inv_open(Oid lobjId, int flags, MemoryContext mcxt)
{
	LargeObjectDesc *retval;
	Snapshot	snapshot = NULL;
	int			descflags = 0;

	/*
	 * 历史上，(INV_WRITE) 和 (INV_WRITE | INV_READ) 之间不做区分，
	 * 无论哪种情况，调用方都可以读取大对象描述符。
	 */
	if (flags & INV_WRITE)
		descflags |= IFS_WRLOCK | IFS_RDLOCK;
	if (flags & INV_READ)
		descflags |= IFS_RDLOCK;

	if (descflags == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid flags for opening a large object: %d",
						flags)));

	/* 获取快照。如果请求了写入，使用瞬时快照。 */
	if (descflags & IFS_WRLOCK)
		snapshot = NULL;
	else
		snapshot = GetActiveSnapshot();

	/* 这里不能使用 LargeObjectExists，因为我们需要指定快照 */
	if (!LargeObjectExistsWithSnapshot(lobjId, snapshot))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("large object %u does not exist", lobjId)));

	/* 应用权限检查，同样需要指定快照 */
	if ((descflags & IFS_RDLOCK) != 0)
	{
		if (!lo_compat_privileges &&
			pg_largeobject_aclcheck_snapshot(lobjId,
											 GetUserId(),
											 ACL_SELECT,
											 snapshot) != ACLCHECK_OK)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied for large object %u",
							lobjId)));
	}
	if ((descflags & IFS_WRLOCK) != 0)
	{
		if (!lo_compat_privileges &&
			pg_largeobject_aclcheck_snapshot(lobjId,
											 GetUserId(),
											 ACL_UPDATE,
											 snapshot) != ACLCHECK_OK)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied for large object %u",
							lobjId)));
	}

	/* 可以创建描述符了 */
	retval = (LargeObjectDesc *) MemoryContextAlloc(mcxt,
													sizeof(LargeObjectDesc));
	retval->id = lobjId;
	retval->offset = 0;
	retval->flags = descflags;

	/* 由调用方按需设置，本文件中的函数不使用此字段 */
	retval->subid = InvalidSubTransactionId;

	/*
	 * 快照（如果有）只是当前活跃的快照。
	 * 调用方将在需要时将其替换为生命周期更长的副本。
	 */
	retval->snapshot = snapshot;

	return retval;
}

/*
 * 关闭先前由 inv_open() 创建的大对象描述符，并释放其所占用的长期内存。
 */
void
inv_close(LargeObjectDesc *obj_desc)
{
	Assert(PointerIsValid(obj_desc));
	pfree(obj_desc);
}

/*
 * 销毁一个现有的大对象（不要与描述符混淆！）
 *
 * 注意：我们期望调用方已完成任何所需的权限检查。
 */
int
inv_drop(Oid lobjId)
{
	ObjectAddress object;

	/*
	 * 删除大对象上的所有注释和依赖关系
	 */
	object.classId = LargeObjectRelationId;
	object.objectId = lobjId;
	object.objectSubId = 0;
	performDeletion(&object, DROP_CASCADE, 0);

	/*
	 * 推进命令计数器，使元组删除对本次事务中的后续大对象操作可见。
	 */
	CommandCounterIncrement();

	/* 出于历史原因，成功时始终返回 1。 */
	return 1;
}

/*
 * 确定大对象的大小
 *
 * 注意：LO 可能包含空洞，就像 Unix 文件一样。
 * 我们实际返回的是最后一个字节的偏移量 + 1。
 */
static uint64
inv_getsize(LargeObjectDesc *obj_desc)
{
	uint64		lastbyte = 0;
	ScanKeyData skey[1];
	SysScanDesc sd;
	HeapTuple	tuple;

	Assert(PointerIsValid(obj_desc));

	open_lo_relation();

	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_loid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(obj_desc->id));

	sd = systable_beginscan_ordered(lo_heap_r, lo_index_r,
									obj_desc->snapshot, 1, skey);

	/*
	 * 由于 pg_largeobject 的索引覆盖 loid 和 pageno 两列，
	 * 而我们仅约束 loid，因此反向扫描将按 pageno 逆序遍历
	 * 大对象的所有页面。所以，只需检查第一个有效元组（即最后一个有效页面）即可。
	 */
	tuple = systable_getnext_ordered(sd, BackwardScanDirection);
	if (HeapTupleIsValid(tuple))
	{
		Form_pg_largeobject data;
		bytea	   *datafield;
		int			len;
		bool		pfreeit;

		if (HeapTupleHasNulls(tuple))	/* 偏执检查 */
			elog(ERROR, "null field found in pg_largeobject");
		data = (Form_pg_largeobject) GETSTRUCT(tuple);
		getdatafield(data, &datafield, &len, &pfreeit);
		lastbyte = (uint64) data->pageno * LOBLKSIZE + len;
		if (pfreeit)
			pfree(datafield);
	}

	systable_endscan_ordered(sd);

	return lastbyte;
}

int64
inv_seek(LargeObjectDesc *obj_desc, int64 offset, int whence)
{
	int64		newoffset;

	Assert(PointerIsValid(obj_desc));

	/*
	 * 只要拥有读或写权限之一，我们就允许 seek/tell，
	 * 因此这里无需进行权限检查。
	 */

	/*
	 * 注意：加法可能溢出，但由于我们会拒绝负数结果，
	 * 因此不需要额外的溢出检测。
	 */
	switch (whence)
	{
		case SEEK_SET:
			newoffset = offset;
			break;
		case SEEK_CUR:
			newoffset = obj_desc->offset + offset;
			break;
		case SEEK_END:
			newoffset = inv_getsize(obj_desc) + offset;
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid whence setting: %d", whence)));
			newoffset = 0;		/* 消除编译器警告 */
			break;
	}

	/*
	 * 这里使用 errmsg_internal，因为我们不想在可翻译字符串中
	 * 暴露 INT64_FORMAT；做得更好不值得这个麻烦。
	 */
	if (newoffset < 0 || newoffset > MAX_LARGE_OBJECT_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg_internal("invalid large object seek target: " INT64_FORMAT,
								 newoffset)));

	obj_desc->offset = newoffset;
	return newoffset;
}

int64
inv_tell(LargeObjectDesc *obj_desc)
{
	Assert(PointerIsValid(obj_desc));

	/*
	 * 只要拥有读或写权限之一，我们就允许 seek/tell，
	 * 因此这里无需进行权限检查。
	 */

	return obj_desc->offset;
}

int
inv_read(LargeObjectDesc *obj_desc, char *buf, int nbytes)
{
	int			nread = 0;
	int64		n;
	int64		off;
	int			len;
	int32		pageno = (int32) (obj_desc->offset / LOBLKSIZE);
	uint64		pageoff;
	ScanKeyData skey[2];
	SysScanDesc sd;
	HeapTuple	tuple;

	Assert(PointerIsValid(obj_desc));
	Assert(buf != NULL);

	if ((obj_desc->flags & IFS_RDLOCK) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for large object %u",
						obj_desc->id)));

	if (nbytes <= 0)
		return 0;

	open_lo_relation();

	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_loid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(obj_desc->id));

	ScanKeyInit(&skey[1],
				Anum_pg_largeobject_pageno,
				BTGreaterEqualStrategyNumber, F_INT4GE,
				Int32GetDatum(pageno));

	sd = systable_beginscan_ordered(lo_heap_r, lo_index_r,
									obj_desc->snapshot, 2, skey);

	while ((tuple = systable_getnext_ordered(sd, ForwardScanDirection)) != NULL)
	{
		Form_pg_largeobject data;
		bytea	   *datafield;
		bool		pfreeit;

		if (HeapTupleHasNulls(tuple))	/* 偏执检查 */
			elog(ERROR, "null field found in pg_largeobject");
		data = (Form_pg_largeobject) GETSTRUCT(tuple);

		/*
		 * 我们期望索引扫描将按顺序返回页面。但是，
		 * 如果 LO 包含未写入的"空洞"，则可能出现缺失的页面。
		 * 我们期望缺失的部分读取为零。
		 */
		pageoff = ((uint64) data->pageno) * LOBLKSIZE;
		if (pageoff > obj_desc->offset)
		{
			n = pageoff - obj_desc->offset;
			n = (n <= (nbytes - nread)) ? n : (nbytes - nread);
			MemSet(buf + nread, 0, n);
			nread += n;
			obj_desc->offset += n;
		}

		if (nread < nbytes)
		{
			Assert(obj_desc->offset >= pageoff);
			off = (int) (obj_desc->offset - pageoff);
			Assert(off >= 0 && off < LOBLKSIZE);

			getdatafield(data, &datafield, &len, &pfreeit);
			if (len > off)
			{
				n = len - off;
				n = (n <= (nbytes - nread)) ? n : (nbytes - nread);
				memcpy(buf + nread, VARDATA(datafield) + off, n);
				nread += n;
				obj_desc->offset += n;
			}
			if (pfreeit)
				pfree(datafield);
		}

		if (nread >= nbytes)
			break;
	}

	systable_endscan_ordered(sd);

	return nread;
}

int
inv_write(LargeObjectDesc *obj_desc, const char *buf, int nbytes)
{
	int			nwritten = 0;
	int			n;
	int			off;
	int			len;
	int32		pageno = (int32) (obj_desc->offset / LOBLKSIZE);
	ScanKeyData skey[2];
	SysScanDesc sd;
	HeapTuple	oldtuple;
	Form_pg_largeobject olddata;
	bool		neednextpage;
	bytea	   *datafield;
	bool		pfreeit;
	union
	{
		bytea		hdr;
		/* 使联合体足够大以容纳一个 LO 数据块： */
		char		data[LOBLKSIZE + VARHDRSZ];
		/* 确保联合体对齐良好： */
		int32		align_it;
	}			workbuf;
	char	   *workb = VARDATA(&workbuf.hdr);
	HeapTuple	newtup;
	Datum		values[Natts_pg_largeobject];
	bool		nulls[Natts_pg_largeobject];
	bool		replace[Natts_pg_largeobject];
	CatalogIndexState indstate;

	Assert(PointerIsValid(obj_desc));
	Assert(buf != NULL);

	/* 强制检查可写权限，否则快照很可能是不正确的 */
	if ((obj_desc->flags & IFS_WRLOCK) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for large object %u",
						obj_desc->id)));

	if (nbytes <= 0)
		return 0;

	/* 此加法不可能溢出，因为 nbytes 仅为 int32 */
	if ((nbytes + obj_desc->offset) > MAX_LARGE_OBJECT_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid large object write request size: %d",
						nbytes)));

	open_lo_relation();

	indstate = CatalogOpenIndexes(lo_heap_r);

	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_loid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(obj_desc->id));

	ScanKeyInit(&skey[1],
				Anum_pg_largeobject_pageno,
				BTGreaterEqualStrategyNumber, F_INT4GE,
				Int32GetDatum(pageno));

	sd = systable_beginscan_ordered(lo_heap_r, lo_index_r,
									obj_desc->snapshot, 2, skey);

	oldtuple = NULL;
	olddata = NULL;
	neednextpage = true;

	while (nwritten < nbytes)
	{
		/*
		 * 如果可能，获取 LO 中下一个已存在的页面。
		 * 我们期望索引扫描按顺序返回它们 —— 但可能存在空洞。
		 */
		if (neednextpage)
		{
			if ((oldtuple = systable_getnext_ordered(sd, ForwardScanDirection)) != NULL)
			{
				if (HeapTupleHasNulls(oldtuple))	/* 偏执检查 */
					elog(ERROR, "null field found in pg_largeobject");
				olddata = (Form_pg_largeobject) GETSTRUCT(oldtuple);
				Assert(olddata->pageno >= pageno);
			}
			neednextpage = false;
		}

		/*
		 * 如果我们有已存在的页面，检查它是否是我们想要写入的页面，
		 * 或是更晚的页面。
		 */
		if (olddata != NULL && olddata->pageno == pageno)
		{
			/*
			 * 用新数据更新现有页面。
			 *
			 * 首先，将旧数据加载到 workbuf 中
			 */
			getdatafield(olddata, &datafield, &len, &pfreeit);
			memcpy(workb, VARDATA(datafield), len);
			if (pfreeit)
				pfree(datafield);

			/*
			 * 填充空洞
			 */
			off = (int) (obj_desc->offset % LOBLKSIZE);
			if (off > len)
				MemSet(workb + len, 0, off - len);

			/*
			 * 插入新数据的适当部分
			 */
			n = LOBLKSIZE - off;
			n = (n <= (nbytes - nwritten)) ? n : (nbytes - nwritten);
			memcpy(workb + off, buf + nwritten, n);
			nwritten += n;
			obj_desc->offset += n;
			off += n;
			/* 计算新页面的有效长度 */
			len = (len >= off) ? len : off;
			SET_VARSIZE(&workbuf.hdr, len + VARHDRSZ);

			/*
			 * 构造并插入更新后的元组
			 */
			memset(values, 0, sizeof(values));
			memset(nulls, false, sizeof(nulls));
			memset(replace, false, sizeof(replace));
			values[Anum_pg_largeobject_data - 1] = PointerGetDatum(&workbuf);
			replace[Anum_pg_largeobject_data - 1] = true;
			newtup = heap_modify_tuple(oldtuple, RelationGetDescr(lo_heap_r),
									   values, nulls, replace);
			CatalogTupleUpdateWithInfo(lo_heap_r, &newtup->t_self, newtup,
									   indstate);
			heap_freetuple(newtup);

			/*
			 * 这个旧页面已处理完毕。
			 */
			oldtuple = NULL;
			olddata = NULL;
			neednextpage = true;
		}
		else
		{
			/*
			 * 写入一个全新的页面。
			 *
			 * 首先，填充空洞
			 */
			off = (int) (obj_desc->offset % LOBLKSIZE);
			if (off > 0)
				MemSet(workb, 0, off);

			/*
			 * 插入新数据的适当部分
			 */
			n = LOBLKSIZE - off;
			n = (n <= (nbytes - nwritten)) ? n : (nbytes - nwritten);
			memcpy(workb + off, buf + nwritten, n);
			nwritten += n;
			obj_desc->offset += n;
			/* 计算新页面的有效长度 */
			len = off + n;
			SET_VARSIZE(&workbuf.hdr, len + VARHDRSZ);

			/*
			 * 构造并插入更新后的元组
			 */
			memset(values, 0, sizeof(values));
			memset(nulls, false, sizeof(nulls));
			values[Anum_pg_largeobject_loid - 1] = ObjectIdGetDatum(obj_desc->id);
			values[Anum_pg_largeobject_pageno - 1] = Int32GetDatum(pageno);
			values[Anum_pg_largeobject_data - 1] = PointerGetDatum(&workbuf);
			newtup = heap_form_tuple(lo_heap_r->rd_att, values, nulls);
			CatalogTupleInsertWithInfo(lo_heap_r, newtup, indstate);
			heap_freetuple(newtup);
		}
		pageno++;
	}

	systable_endscan_ordered(sd);

	CatalogCloseIndexes(indstate);

	/*
	 * 推进命令计数器，使我们的元组更新对本次事务中的后续大对象操作可见。
	 */
	CommandCounterIncrement();

	return nwritten;
}

void
inv_truncate(LargeObjectDesc *obj_desc, int64 len)
{
	int32		pageno = (int32) (len / LOBLKSIZE);
	int32		off;
	ScanKeyData skey[2];
	SysScanDesc sd;
	HeapTuple	oldtuple;
	Form_pg_largeobject olddata;
	union
	{
		bytea		hdr;
		/* 使联合体足够大以容纳一个 LO 数据块： */
		char		data[LOBLKSIZE + VARHDRSZ];
		/* 确保联合体对齐良好： */
		int32		align_it;
	}			workbuf;
	char	   *workb = VARDATA(&workbuf.hdr);
	HeapTuple	newtup;
	Datum		values[Natts_pg_largeobject];
	bool		nulls[Natts_pg_largeobject];
	bool		replace[Natts_pg_largeobject];
	CatalogIndexState indstate;

	Assert(PointerIsValid(obj_desc));

	/* 强制检查可写权限，否则快照很可能是不正确的 */
	if ((obj_desc->flags & IFS_WRLOCK) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for large object %u",
						obj_desc->id)));

	/*
	 * 这里使用 errmsg_internal，因为我们不想在可翻译字符串中
	 * 暴露 INT64_FORMAT；做得更好不值得这个麻烦。
	 */
	if (len < 0 || len > MAX_LARGE_OBJECT_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg_internal("invalid large object truncation target: " INT64_FORMAT,
								 len)));

	open_lo_relation();

	indstate = CatalogOpenIndexes(lo_heap_r);

	/*
	 * 设置以查找所有具有所需 loid 且 pageno >= 目标的页面
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_loid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(obj_desc->id));

	ScanKeyInit(&skey[1],
				Anum_pg_largeobject_pageno,
				BTGreaterEqualStrategyNumber, F_INT4GE,
				Int32GetDatum(pageno));

	sd = systable_beginscan_ordered(lo_heap_r, lo_index_r,
									obj_desc->snapshot, 2, skey);

	/*
	 * 如果可能，获取截断点所在的页面。截断点可能超出
	 * LO 的末尾或位于空洞中。
	 */
	olddata = NULL;
	if ((oldtuple = systable_getnext_ordered(sd, ForwardScanDirection)) != NULL)
	{
		if (HeapTupleHasNulls(oldtuple))	/* 偏执检查 */
			elog(ERROR, "null field found in pg_largeobject");
		olddata = (Form_pg_largeobject) GETSTRUCT(oldtuple);
		Assert(olddata->pageno >= pageno);
	}

	/*
	 * 如果找到了截断点所在的页面，我们需要截断其中的数据。
	 * 否则如果位于空洞中，我们需要创建一个页面来标记数据的结束位置。
	 */
	if (olddata != NULL && olddata->pageno == pageno)
	{
		/* 首先，将旧数据加载到 workbuf 中 */
		bytea	   *datafield;
		int			pagelen;
		bool		pfreeit;

		getdatafield(olddata, &datafield, &pagelen, &pfreeit);
		memcpy(workb, VARDATA(datafield), pagelen);
		if (pfreeit)
			pfree(datafield);

		/*
		 * 填充空洞
		 */
		off = len % LOBLKSIZE;
		if (off > pagelen)
			MemSet(workb + pagelen, 0, off - pagelen);

		/* 计算新页面的长度 */
		SET_VARSIZE(&workbuf.hdr, off + VARHDRSZ);

		/*
		 * 构造并插入更新后的元组
		 */
		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));
		memset(replace, false, sizeof(replace));
		values[Anum_pg_largeobject_data - 1] = PointerGetDatum(&workbuf);
		replace[Anum_pg_largeobject_data - 1] = true;
		newtup = heap_modify_tuple(oldtuple, RelationGetDescr(lo_heap_r),
								   values, nulls, replace);
		CatalogTupleUpdateWithInfo(lo_heap_r, &newtup->t_self, newtup,
								   indstate);
		heap_freetuple(newtup);
	}
	else
	{
		/*
		 * 如果找到的第一个页面在截断点之后，则我们处于一个
		 * 将要填充的空洞中，但需要删除后面的页面，
		 * 因为下面的循环不会再访问它。
		 */
		if (olddata != NULL)
		{
			Assert(olddata->pageno > pageno);
			CatalogTupleDelete(lo_heap_r, &oldtuple->t_self);
		}

		/*
		 * 写入一个全新的页面。
		 *
		 * 将空洞填充到截断点
		 */
		off = len % LOBLKSIZE;
		if (off > 0)
			MemSet(workb, 0, off);

		/* 计算新页面的长度 */
		SET_VARSIZE(&workbuf.hdr, off + VARHDRSZ);

		/*
		 * 构造并插入新元组
		 */
		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));
		values[Anum_pg_largeobject_loid - 1] = ObjectIdGetDatum(obj_desc->id);
		values[Anum_pg_largeobject_pageno - 1] = Int32GetDatum(pageno);
		values[Anum_pg_largeobject_data - 1] = PointerGetDatum(&workbuf);
		newtup = heap_form_tuple(lo_heap_r->rd_att, values, nulls);
		CatalogTupleInsertWithInfo(lo_heap_r, newtup, indstate);
		heap_freetuple(newtup);
	}

	/*
	 * 删除截断点之后的所有页面。如果初始搜索没有找到页面，
	 * 那么自然没有更多的事情可做了。
	 */
	if (olddata != NULL)
	{
		while ((oldtuple = systable_getnext_ordered(sd, ForwardScanDirection)) != NULL)
		{
			CatalogTupleDelete(lo_heap_r, &oldtuple->t_self);
		}
	}

	systable_endscan_ordered(sd);

	CatalogCloseIndexes(indstate);

	/*
	 * 推进命令计数器，使元组更新对本次事务中的后续大对象操作可见。
	 */
	CommandCounterIncrement();
}
