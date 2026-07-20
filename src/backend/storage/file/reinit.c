/*-------------------------------------------------------------------------
 *
 * reinit.c
 *	  未日志化关系的重新初始化
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/reinit.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "common/relpath.h"
#include "postmaster/startup.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "storage/reinit.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

static void ResetUnloggedRelationsInTablespaceDir(const char *tsdirname,
												  int op);
static void ResetUnloggedRelationsInDbspaceDir(const char *dbspacedirname,
											   int op);

typedef struct
{
	RelFileNumber relnumber;	/* 哈希键 */
} unlogged_relation_entry;

/*
 * 重置上一次重启前的未日志化关系。
 *
 * 如果 op 包含 UNLOGGED_RELATION_CLEANUP，则删除所有带有 "init" fork 的
 * 关系除 "init" fork 本身之外的全部 fork。
 *
 * 如果 op 包含 UNLOGGED_RELATION_INIT，则将 "init" fork 复制到主 fork。
 */
void
ResetUnloggedRelations(int op)
{
	char		temp_path[MAXPGPATH + sizeof(PG_TBLSPC_DIR) + sizeof(TABLESPACE_VERSION_DIRECTORY)];
	DIR		   *spc_dir;
	struct dirent *spc_de;
	MemoryContext tmpctx,
				oldctx;

	/* 记录日志。 */
	elog(DEBUG1, "resetting unlogged relations: cleanup %d init %d",
		 (op & UNLOGGED_RELATION_CLEANUP) != 0,
		 (op & UNLOGGED_RELATION_INIT) != 0);

	/*
	 * 为确保不泄漏任何内存，为此操作创建一个临时内存上下文。
	 */
	tmpctx = AllocSetContextCreate(CurrentMemoryContext,
								   "ResetUnloggedRelations",
								   ALLOCSET_DEFAULT_SIZES);
	oldctx = MemoryContextSwitchTo(tmpctx);

	/* 准备报告重置未日志化关系的进度。 */
	begin_startup_progress_phase();

	/*
	 * 首先处理 pg_default ($PGDATA/base) 中的未日志化文件
	 */
	ResetUnloggedRelationsInTablespaceDir("base", op);

	/*
	 * 遍历所有非默认表空间的目录。
	 */
	spc_dir = AllocateDir(PG_TBLSPC_DIR);

	while ((spc_de = ReadDir(spc_dir, PG_TBLSPC_DIR)) != NULL)
	{
		if (strcmp(spc_de->d_name, ".") == 0 ||
			strcmp(spc_de->d_name, "..") == 0)
			continue;

		snprintf(temp_path, sizeof(temp_path), "%s/%s/%s",
				 PG_TBLSPC_DIR, spc_de->d_name, TABLESPACE_VERSION_DIRECTORY);
		ResetUnloggedRelationsInTablespaceDir(temp_path, op);
	}

	FreeDir(spc_dir);

	/*
	 * 恢复内存上下文。
	 */
	MemoryContextSwitchTo(oldctx);
	MemoryContextDelete(tmpctx);
}

/*
 * 为 ResetUnloggedRelations 处理一个表空间目录
 */
static void
ResetUnloggedRelationsInTablespaceDir(const char *tsdirname, int op)
{
	DIR		   *ts_dir;
	struct dirent *de;
	char		dbspace_path[MAXPGPATH * 2];

	ts_dir = AllocateDir(tsdirname);

	/*
	 * 如果在表空间目录上遇到 ENOENT，记录日志并返回。这种情况可能发生在
	 * 之前的 DROP TABLESPACE 在删除表空间目录与删除 pg_tblspc 中的软链接
	 * 之间崩溃时。我们不想在这种情况下阻止数据库启动，因此让它通过。
	 * 其他任何类型的错误将由 ReadDir 报告（导致启动失败）。
	 */
	if (ts_dir == NULL && errno == ENOENT)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						tsdirname)));
		return;
	}

	while ((de = ReadDir(ts_dir, tsdirname)) != NULL)
	{
		/*
		 * 我们只关心以数字命名的每个数据库的目录。注意这段代码也会
		 * （正确地）忽略 "." 和 ".."。
		 */
		if (strspn(de->d_name, "0123456789") != strlen(de->d_name))
			continue;

		snprintf(dbspace_path, sizeof(dbspace_path), "%s/%s",
				 tsdirname, de->d_name);

		if (op & UNLOGGED_RELATION_INIT)
			ereport_startup_progress("resetting unlogged relations (init), elapsed time: %ld.%02d s, current path: %s",
									 dbspace_path);
		else if (op & UNLOGGED_RELATION_CLEANUP)
			ereport_startup_progress("resetting unlogged relations (cleanup), elapsed time: %ld.%02d s, current path: %s",
									 dbspace_path);

		ResetUnloggedRelationsInDbspaceDir(dbspace_path, op);
	}

	FreeDir(ts_dir);
}

/*
 * 为 ResetUnloggedRelations 处理一个数据库空间目录
 */
static void
ResetUnloggedRelationsInDbspaceDir(const char *dbspacedirname, int op)
{
	DIR		   *dbspace_dir;
	struct dirent *de;
	char		rm_path[MAXPGPATH * 2];

	/* 调用者必须指定至少一个操作。 */
	Assert((op & (UNLOGGED_RELATION_CLEANUP | UNLOGGED_RELATION_INIT)) != 0);

	/*
	 * 清理是一个两趟操作。首先，遍历并识别所有带有 init fork 的文件。
	 * 然后，再次遍历并删除除 init fork 之外具有相同 OID 的所有内容。
	 */
	if ((op & UNLOGGED_RELATION_CLEANUP) != 0)
	{
		HTAB	   *hash;
		HASHCTL		ctl;

		/*
		 * 有可能有人在同一个数据库和表空间中创建了大量未日志化关系，
		 * 因此我们最好使用哈希表而非数组或链表来跟踪需要重置的文件。
		 * 否则，此清理操作将是 O(n^2) 的。
		 */
		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(unlogged_relation_entry);
		ctl.hcxt = CurrentMemoryContext;
		hash = hash_create("unlogged relation OIDs", 32, &ctl,
						   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

		/* 扫描目录。 */
		dbspace_dir = AllocateDir(dbspacedirname);
		while ((de = ReadDir(dbspace_dir, dbspacedirname)) != NULL)
		{
			ForkNumber	forkNum;
			unsigned	segno;
			unlogged_relation_entry ent;

			/* 跳过不像是关系数据文件的内容。 */
			if (!parse_filename_for_nontemp_relation(de->d_name,
													 &ent.relnumber,
													 &forkNum, &segno))
				continue;

			/* 如果不是 init fork 也跳过。 */
			if (forkNum != INIT_FORKNUM)
				continue;

			/*
			 * 将 RelFileNumber 放入哈希表（如果尚未存在）。
			 */
			(void) hash_search(hash, &ent, HASH_ENTER, NULL);
		}

		/* 第一趟扫描完成。 */
		FreeDir(dbspace_dir);

		/*
		 * 如果没有找到任何 init fork，就没有必要继续；现在就可以退出。
		 */
		if (hash_get_num_entries(hash) == 0)
		{
			hash_destroy(hash);
			return;
		}

		/*
		 * 现在进行第二趟扫描，删除任何匹配的内容。
		 */
		dbspace_dir = AllocateDir(dbspacedirname);
		while ((de = ReadDir(dbspace_dir, dbspacedirname)) != NULL)
		{
			ForkNumber	forkNum;
			unsigned	segno;
			unlogged_relation_entry ent;

			/* 跳过不像是关系数据文件的内容。 */
			if (!parse_filename_for_nontemp_relation(de->d_name,
													 &ent.relnumber,
													 &forkNum, &segno))
				continue;

			/* 绝不删除 init fork。 */
			if (forkNum == INIT_FORKNUM)
				continue;

			/*
			 * 检查名称中的 OID 部分是否出现在哈希表中。如果在，则删除！
			 */
			if (hash_search(hash, &ent, HASH_FIND, NULL))
			{
				snprintf(rm_path, sizeof(rm_path), "%s/%s",
						 dbspacedirname, de->d_name);
				if (unlink(rm_path) < 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not remove file \"%s\": %m",
									rm_path)));
				else
					elog(DEBUG2, "unlinked file \"%s\"", rm_path);
			}
		}

		/* 清理完成。 */
		FreeDir(dbspace_dir);
		hash_destroy(hash);
	}

	/*
	 * 初始化在清理完成后进行：将每个 init fork 文件复制到对应的主 fork
	 * 文件。注意，如果同时要求执行清理和初始化，我们可能永远不会到达这里：
	 * 如果清理代码确定此数据库空间中没有任何 init fork，它会在到达此处
	 * 之前返回。
	 */
	if ((op & UNLOGGED_RELATION_INIT) != 0)
	{
		/* 扫描目录。 */
		dbspace_dir = AllocateDir(dbspacedirname);
		while ((de = ReadDir(dbspace_dir, dbspacedirname)) != NULL)
		{
			ForkNumber	forkNum;
			RelFileNumber relNumber;
			unsigned	segno;
			char		srcpath[MAXPGPATH * 2];
			char		dstpath[MAXPGPATH];

			/* 跳过不像是关系数据文件的内容。 */
			if (!parse_filename_for_nontemp_relation(de->d_name, &relNumber,
													 &forkNum, &segno))
				continue;

			/* 如果不是 init fork 也跳过。 */
			if (forkNum != INIT_FORKNUM)
				continue;

			/* 构造源路径名。 */
			snprintf(srcpath, sizeof(srcpath), "%s/%s",
					 dbspacedirname, de->d_name);

			/* 构造目标路径名。 */
			if (segno == 0)
				snprintf(dstpath, sizeof(dstpath), "%s/%u",
						 dbspacedirname, relNumber);
			else
				snprintf(dstpath, sizeof(dstpath), "%s/%u.%u",
						 dbspacedirname, relNumber, segno);

			/* 准备就绪，执行实际复制。 */
			elog(DEBUG2, "copying %s to %s", srcpath, dstpath);
			copy_file(srcpath, dstpath);
		}

		FreeDir(dbspace_dir);

		/*
		 * 上面的 copy_file() 已经对其创建的文件调用了 pg_flush_data()。
		 * 现在我们需要对这些文件执行 fsync，因为在恢复期间检查点不会替我们
		 * 做这件事。我们在单独的一趟中来执行此操作，以便内核能够一次性执行
		 * 所有刷新（尤其是元数据刷新）。
		 */
		dbspace_dir = AllocateDir(dbspacedirname);
		while ((de = ReadDir(dbspace_dir, dbspacedirname)) != NULL)
		{
			RelFileNumber relNumber;
			ForkNumber	forkNum;
			unsigned	segno;
			char		mainpath[MAXPGPATH];

			/* 跳过不像是关系数据文件的内容。 */
			if (!parse_filename_for_nontemp_relation(de->d_name, &relNumber,
													 &forkNum, &segno))
				continue;

			/* 如果不是 init fork 也跳过。 */
			if (forkNum != INIT_FORKNUM)
				continue;

			/* 构造主 fork 路径名。 */
			if (segno == 0)
				snprintf(mainpath, sizeof(mainpath), "%s/%u",
						 dbspacedirname, relNumber);
			else
				snprintf(mainpath, sizeof(mainpath), "%s/%u.%u",
						 dbspacedirname, relNumber, segno);

			fsync_fname(mainpath, false);
		}

		FreeDir(dbspace_dir);

		/*
		 * 最后，对数据库目录本身执行 fsync，确保文件系统记住我们所做的
		 * 文件创建和删除。对于仅执行 UNLOGGED_RELATION_CLEANUP 的调用，
		 * 我们不关心这一步，因为如果恢复在我们执行 UNLOGGED_RELATION_INIT
		 * 之前崩溃，我们在下次启动尝试时也会重新执行清理步骤。
		 */
		fsync_fname(dbspacedirname, true);
	}
}

/*
 * 对可能的关联文件名进行基本解析。
 *
 * 如果文件看起来符合非临时关系的正确格式，此函数返回 true，否则返回 false。
 *
 * 如果返回 true，则将 *relnumber、*fork 和 *segno 设置为从文件名中提取的值。
 * 如果返回 false，这些值分别被设置为 InvalidRelFileNumber、InvalidForkNumber
 * 和 0。
 */
bool
parse_filename_for_nontemp_relation(const char *name, RelFileNumber *relnumber,
									ForkNumber *fork, unsigned *segno)
{
	unsigned long n,
				s;
	ForkNumber	f;
	char	   *endp;

	*relnumber = InvalidRelFileNumber;
	*fork = InvalidForkNumber;
	*segno = 0;

	/*
	 * 关系文件名应以非零数字开头。通过拒绝前导零的情况，调用者可以假设
	 * 对于任何给定的 *relnumber 值，只有一种可能的字符串表示。
	 *
	 * （需要明确的是，我们不期望像 0017.3 这样的文件名存在——但如果
	 * 0017.3 确实存在，它是一个非关系文件，而不是 relfilenode 17
	 * 的主 fork 的一部分。）
	 */
	if (name[0] < '1' || name[0] > '9')
		return false;

	/*
	 * 解析前导数字串。如果值超出范围，则断定这根本不是关系文件。
	 */
	errno = 0;
	n = strtoul(name, &endp, 10);
	if (errno || name == endp || n <= 0 || n > PG_UINT32_MAX)
		return false;
	name = endp;

	/* 检查 fork 名称。 */
	if (*name != '_')
		f = MAIN_FORKNUM;
	else
	{
		int			forkchar;

		forkchar = forkname_chars(name + 1, &f);
		if (forkchar <= 0)
			return false;
		name += forkchar + 1;
	}

	/* 检查段号。 */
	if (*name != '.')
		s = 0;
	else
	{
		/* 拒绝前导零，与 RelFileNumber 的处理方式一致。 */
		if (name[1] < '1' || name[1] > '9')
			return false;

		errno = 0;
		s = strtoul(name + 1, &endp, 10);
		if (errno || name + 1 == endp || s <= 0 || s > PG_UINT32_MAX)
			return false;
		name = endp;
	}

	/* 现在应该已到达字符串末尾。 */
	if (*name != '\0')
		return false;

	/* 设置输出参数并返回。 */
	*relnumber = (RelFileNumber) n;
	*fork = f;
	*segno = (unsigned) s;
	return true;
}
