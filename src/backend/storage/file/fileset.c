/*-------------------------------------------------------------------------
 *
 * fileset.c
 *	  命名临时文件的管理。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/fileset.c
 *
 * FileSet 提供了一个临时命名空间（类似于目录），以便可以通过名称来
 * 查找文件。
 *
 * 当临时文件需要多次打开/关闭，且底层文件需要在事务间持续存在时，
 * 后端可以使用 FileSet。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "commands/tablespace.h"
#include "common/file_utils.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/fileset.h"

static void FileSetPath(char *path, FileSet *fileset, Oid tablespace);
static void FilePath(char *path, FileSet *fileset, const char *name);
static Oid	ChooseTablespace(const FileSet *fileset, const char *name);

/*
 * 初始化一个临时文件空间。此 API 可用于共享 fileset，也可用于临时
 * 文件仅由单个后端使用、但文件需要多次打开关闭且底层文件需要在事务间
 * 持续存在的场景。
 *
 * 调用者需要通过 FileSetDelete/FileSetDeleteAll 显式删除这些文件。
 *
 * 文件将分布在 temp_tablespaces 配置的表空间中。
 *
 * 本质上，该集合是一个或多个最终将被删除的目录。
 */
void
FileSetInit(FileSet *fileset)
{
	static uint32 counter = 0;

	fileset->creator_pid = MyProcPid;
	fileset->number = counter;
	counter = (counter + 1) % INT_MAX;

	/* 捕获表空间 OID，使所有后端对其达成一致。 */
	PrepareTempTablespaces();
	fileset->ntablespaces =
		GetTempTablespaces(&fileset->tablespaces[0],
						   lengthof(fileset->tablespaces));
	if (fileset->ntablespaces == 0)
	{
		/* 如果 GUC 为空，使用当前数据库的默认表空间 */
		fileset->tablespaces[0] = MyDatabaseTableSpace;
		fileset->ntablespaces = 1;
	}
	else
	{
		int			i;

		/*
		 * InvalidOid 条目表示使用当前数据库的默认表空间。现在替换它，
		 * 以确保 FileSet 的所有使用者对行为达成一致。
		 */
		for (i = 0; i < fileset->ntablespaces; i++)
		{
			if (fileset->tablespaces[i] == InvalidOid)
				fileset->tablespaces[i] = MyDatabaseTableSpace;
		}
	}
}

/*
 * 在给定集合中创建一个新文件。
 */
File
FileSetCreate(FileSet *fileset, const char *name)
{
	char		path[MAXPGPATH];
	File		file;

	FilePath(path, fileset, name);
	file = PathNameCreateTemporaryFile(path, false);

	/* 如果失败，检查是否需要按需创建目录。 */
	if (file <= 0)
	{
		char		tempdirpath[MAXPGPATH];
		char		filesetpath[MAXPGPATH];
		Oid			tablespace = ChooseTablespace(fileset, name);

		TempTablespacePath(tempdirpath, tablespace);
		FileSetPath(filesetpath, fileset, tablespace);
		PathNameCreateTemporaryDir(tempdirpath, filesetpath);
		file = PathNameCreateTemporaryFile(path, true);
	}

	return file;
}

/*
 * 打开通过 FileSetCreate() 创建的文件 */
File
FileSetOpen(FileSet *fileset, const char *name, int mode)
{
	char		path[MAXPGPATH];
	File		file;

	FilePath(path, fileset, name);
	file = PathNameOpenTemporaryFile(path, mode);

	return file;
}

/*
 * 删除通过 FileSetCreate() 创建的文件。
 *
 * 如果文件存在返回 true，不存在返回 false。
 */
bool
FileSetDelete(FileSet *fileset, const char *name,
			  bool error_on_failure)
{
	char		path[MAXPGPATH];

	FilePath(path, fileset, name);

	return PathNameDeleteTemporaryFile(path, error_on_failure);
}

/*
 * 删除集合中的所有文件。
 */
void
FileSetDeleteAll(FileSet *fileset)
{
	char		dirpath[MAXPGPATH];
	int			i;

	/*
	 * 删除我们在每个表空间中创建的目录。不会失败，因为我们在错误
	 * 清理路径中使用此函数，但发生 IO 错误时会生成 LOG 消息。
	 */
	for (i = 0; i < fileset->ntablespaces; ++i)
	{
		FileSetPath(dirpath, fileset, fileset->tablespaces[i]);
		PathNameDeleteTemporaryDir(dirpath);
	}
}

/*
 * 构建给定表空间中存放 FileSet 文件的目录路径。
 */
static void
FileSetPath(char *path, FileSet *fileset, Oid tablespace)
{
	char		tempdirpath[MAXPGPATH];

	TempTablespacePath(tempdirpath, tablespace);
	snprintf(path, MAXPGPATH, "%s/%s%lu.%u.fileset",
			 tempdirpath, PG_TEMP_FILE_PREFIX,
			 (unsigned long) fileset->creator_pid, fileset->number);
}

/*
 * 确定给定的临时文件归属于哪个表空间。
 */
static Oid
ChooseTablespace(const FileSet *fileset, const char *name)
{
	uint32		hash = hash_any((const unsigned char *) name, strlen(name));

	return fileset->tablespaces[hash % fileset->ntablespaces];
}

/*
 * 计算 FileSet 中文件的完整路径。
 */
static void
FilePath(char *path, FileSet *fileset, const char *name)
{
	char		dirpath[MAXPGPATH];

	FileSetPath(dirpath, fileset, ChooseTablespace(fileset, name));
	snprintf(path, MAXPGPATH, "%s/%s", dirpath, name);
}
