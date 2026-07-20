/*-------------------------------------------------------------------------
 *
 * sharedfileset.c
 *	  共享临时文件管理。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/sharedfileset.c
 *
 * SharedFileSet 提供一个临时命名空间（可以理解为目录），使得文件可以通过名称被发现，
 * 并提供共享所有权语义，使共享文件在最后一个用户解除关联（detach）之前一直存在。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "storage/dsm.h"
#include "storage/sharedfileset.h"

static void SharedFileSetOnDetach(dsm_segment *segment, Datum datum);

/*
 * 初始化一个供其他后端访问的临时文件空间。
 * 其他后端必须先行关联（attach）才能访问它。将此 SharedFileSet 与 'seg' 绑定。
 * 当最后一个后端解除关联时，其中包含的所有文件都会被删除。
 *
 * 在底层，这个集合是一个或多个目录，最终会被删除。
 */
void
SharedFileSetInit(SharedFileSet *fileset, dsm_segment *seg)
{
	/* 初始化共享文件集特有的成员。 */
	SpinLockInit(&fileset->mutex);
	fileset->refcnt = 1;

	/* 初始化文件集。 */
	FileSetInit(&fileset->fs);

	/* 注册清理回调函数。 */
	if (seg)
		on_dsm_detach(seg, SharedFileSetOnDetach, PointerGetDatum(fileset));
}

/*
 * 关联到通过 SharedFileSetInit 创建的目录集合。
 */
void
SharedFileSetAttach(SharedFileSet *fileset, dsm_segment *seg)
{
	bool		success;

	SpinLockAcquire(&fileset->mutex);
	if (fileset->refcnt == 0)
		success = false;
	else
	{
		++fileset->refcnt;
		success = true;
	}
	SpinLockRelease(&fileset->mutex);

	if (!success)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not attach to a SharedFileSet that is already destroyed")));

	/* 注册清理回调函数。 */
	on_dsm_detach(seg, SharedFileSetOnDetach, PointerGetDatum(fileset));
}

/*
 * 删除集合中的所有文件。
 */
void
SharedFileSetDeleteAll(SharedFileSet *fileset)
{
	FileSetDeleteAll(&fileset->fs);
}

/*
 * 回调函数：当此后端从持有 SharedFileSet 的 DSM 段中解除关联时被调用，
 * 该 SharedFileSet 是此后端创建或关联过的。如果我们是最后一个解除关联的，
 * 则尝试删除目录及其中的所有内容。失败时不能抛出错误，因为此函数运行在
 * 错误清理路径中。
 */
static void
SharedFileSetOnDetach(dsm_segment *segment, Datum datum)
{
	bool		unlink_all = false;
	SharedFileSet *fileset = (SharedFileSet *) DatumGetPointer(datum);

	SpinLockAcquire(&fileset->mutex);
	Assert(fileset->refcnt > 0);
	if (--fileset->refcnt == 0)
		unlink_all = true;
	SpinLockRelease(&fileset->mutex);

	/*
	 * 如果我们是最后一个解除关联的，则删除所有表空间中的目录。
	 * 注意，在此函数的剩余部分，我们实际上仍然处于关联状态，
	 * 因此可以安全地访问其数据。
	 */
	if (unlink_all)
		FileSetDeleteAll(&fileset->fs);
}
