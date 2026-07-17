`src/backend/utils/adt` 是 PostgreSQL 后端中**数据类型（Abstract Data Types）实现的核心目录**。名称 `adt` 即 Abstract Data Types，这里集中存放了各种内置数据类型的 I/O 转换、运算符、比较函数、类型输入/输出、统计信息估算以及配套支持例程。

下面按功能大类梳理：

### 1. 基础标量类型
各文件实现对应类型的输入/输出、比较、算术、哈希等：
- `bool.c`、`char.c`、`name.c`、`int.c`、`int8.c`、`float.c`、`numeric.c`、`oid.c`、`xid.c`、`xid8funcs.c`、`cash.c`、`uuid.c`、`pg_lsn.c`、`varchar.c`、`varbit.c`、`mac.c`、`mac8.c`、`tid.c`
- `numutils.c`：数字文本解析/格式化的底层工具（`pg_strtoint*`、`pg_itoa` 等）

### 2. 日期与时间
- `date.c`、`datetime.c`、`timestamp.c`：`date`/`time`/`timestamp`/`timestamptz`/`interval` 的 I/O 与运算
- `formatting.c`：`to_char`/`to_date`/`to_timestamp` 等格式化函数

### 3. 网络类型
- `network.c` 及 `inet_net_pton.c`/`inet_cidr_ntop.c`：IPv4/IPv6、MAC 的解析与格式化
- `network_gist.c`/`network_spgist.c`/`network_selfuncs.c`：网络类型的索引与选择性估算

### 4. 几何类型
- `geo_ops.c`（点/线/圆/多边形等运算）、`geo_selfuncs.c`、`geo_spgist.c`

### 5. 数组（Array）
- `arrayfuncs.c`（核心 I/O 与操作）、`arrayutils.c`、`arraysubs.c`（下标切片）、`array_userfuncs.c`（用户函数如 `array_append`）、`array_expanded.c`（扩展 datum 表示）、`array_typanalyze.c`/`array_selfuncs.c`（统计）

### 6. 范围与多范围（Range / Multirange）
- `rangetypes.c`、`multirangetypes.c` 及对应的 `_gist.c`/`_spgist.c`/`_selfuncs.c`/`_typanalyze.c`

### 7. JSON 与 JSONPath
- `json.c`、`jsonb.c`/`jsonb_util.c`/`jsonb_op.c`/`jsonb_gin.c`/`jsonbsubs.c`、`jsonfuncs.c`（处理/构造函数）
- `jsonpath.c`/`jsonpath_exec.c` 及 `.y`/`.l` 语法与词法分析

### 8. 文本与字符串处理
- `varlena.c`（字符串运算符、分割、填充等）、`like.c`/`like_match.c`/`like_support.c`、`regexp.c`、`ascii.c`、`encode.c`、`quote.c`、`oracle_compat.c`（兼容函数如 `substr`、`decode`）、`levenshtein.c`（编辑距离）

### 9. 全文检索（Full Text Search）
- `tsvector.c`/`tsvector_parser.c`/`tsvector_op.c`、`tsquery*.c`、`tsrank.c`、`tsginidx.c`/`tsgistidx.c`/`tsquery_gist.c`

### 10. 枚举、域、伪类型与类型系统
- `enum.c`（枚举 I/O 与比较）、`domains.c`（域约束）、`pseudotypes.c`（伪类型）、`format_type.c`（类型名格式化）、`datum.c`（Datum 操作）

### 11. 扩展 Datum（内存优化表示）
- `expandeddatum.c`、`expandedrecord.c`：数组/记录的"扩展"可变表示，避免反复拷贝

### 12. 统计与选择性估算（selfuncs）
- `selfuncs.c` 及各类 `_selfuncs.c`、`typanalyze`：优化器用的行数/选择性估算

### 13. 访问控制、XML、本地化
- `acl.c`（权限检查）、`xml.c`、`pg_locale*.c`（libc/ICU/内置 locale 封装）

### 14. 管理/系统函数（Misc & Admin）
- `misc.c`、`dbsize.c`、`genfile.c`、`lockfuncs.c`、`mcxtfuncs.c`、`pgstatfuncs.c`、`waitfuncs.c`、`version.c`、`pg_upgrade_support.c`、`partitionfuncs.c`、`trigfuncs.c`、`ri_triggers.c`、`ruleutils.c`、`rowtypes.c`、`amutils.c`、`hbafuncs.c`、`cryptohashfuncs.c`、`pseudorandomfuncs.c`

### 15. 窗口函数与有序集聚合
- `windowfuncs.c`、`orderedsetaggs.c`

---

**整体作用总结**：该目录是 PostgreSQL "类型系统" 的后端实现层——每个 SQL 数据类型对应的 C 实现（C 代码中的 `input`/`output`/`send`/`receive`、比较、哈希、运算符、索引支持、选择性估算）几乎都在此。它上接 `fmgr`（函数管理器）与类型目录 `pg_type`，下接各 `access` 方法（GiST/SP-GiST/GIN）和 `optimizer`，是 SQL 值从文本到内部表示再到运算结果全链路的枢纽。

