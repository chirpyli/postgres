#----------------------------------------------------------------------
#
# Catalog.pm
#    Perl 模块，用于将目录文件中的信息提取到 Perl 数据结构中
#    数据结构
#
# Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/backend/catalog/Catalog.pm
#
#----------------------------------------------------------------------

package Catalog;

use strict;
use warnings FATAL => 'all';

use File::Compare;


# 将目录头文件解析为描述该目录结构（schema）的数据结构。
sub ParseHeader
{
	my $input_file = shift;

	# 有少数类型在 C 源码中使用一个名字，但在 SQL 层面使用另一个名字。
	# 这里将它们一一列举出来。
	my %RENAME_ATTTYPE = (
		'int16' => 'int2',
		'int32' => 'int4',
		'int64' => 'int8',
		'Oid' => 'oid',
		'NameData' => 'name',
		'TransactionId' => 'xid',
		'XLogRecPtr' => 'pg_lsn');

	my %catalog;
	my $declaring_attributes = 0;
	my $is_varlen = 0;
	my $is_client_code = 0;

	$catalog{columns} = [];
	$catalog{toasting} = [];
	$catalog{indexing} = [];
	$catalog{other_oids} = [];
	$catalog{foreign_keys} = [];
	$catalog{client_code} = [];

	open(my $ifh, '<', $input_file) || die "$input_file: $!";

	# 扫描输入文件。
	while (<$ifh>)
	{

		# 当我们处于某些代码段时，设置相应的标志。
		if (/^#/)
		{
			$is_varlen = 1 if /^#ifdef\s+CATALOG_VARLEN/;
			if (/^#ifdef\s+EXPOSE_TO_CLIENT_CODE/)
			{
				$is_client_code = 1;
				next;
			}
			next if !$is_client_code;
		}

		if (!$is_client_code)
		{
			# 去除 C 风格的注释。
			s;/\*(.|\n)*\*/;;g;
			if (m;/\*;)
			{

				# 正确处理多行注释。
				my $next_line = <$ifh>;
				die "$input_file: ends within C-style comment\n"
				  if !defined $next_line;
				$_ .= $next_line;
				redo;
			}

			# 去除无用的空白字符和行尾分号。
			chomp;
			s/^\s+//;
			s/;\s*$//;
			s/\s+/ /g;
		}

		# 将数据压入相应的数据结构。
		# 注意：在新增可识别的 OID 定义宏时，
		# 也要同步更新 src/include/catalog/renumber_oids.pl。
		if (/^DECLARE_TOAST\(\s*
			 (?<parent_table>\w+),\s*
			 (?<toast_oid>\d+),\s*
			 (?<toast_index_oid>\d+)\s*
			 \)/x
		  )
		{
			push @{ $catalog{toasting} }, {%+};
		}
		elsif (
			/^DECLARE_TOAST_WITH_MACRO\(\s*
			 (?<parent_table>\w+),\s*
			 (?<toast_oid>\d+),\s*
			 (?<toast_index_oid>\d+),\s*
			 (?<toast_oid_macro>\w+),\s*
			 (?<toast_index_oid_macro>\w+)\s*
			 \)/x
		  )
		{
			push @{ $catalog{toasting} }, {%+};
		}
		elsif (
			/^DECLARE_(UNIQUE_)?INDEX(_PKEY)?\(\s*
			 (?<index_name>\w+),\s*
			 (?<index_oid>\d+),\s*
			 (?<index_oid_macro>\w+),\s*
			 (?<table_name>\w+),\s*
			 (?<index_decl>.+)\s*
			 \)/x
		  )
		{
			push @{ $catalog{indexing} },
			  {
				is_unique => $1 ? 1 : 0,
				is_pkey => $2 ? 1 : 0,
				%+,
			  };
		}
		elsif (
			/^MAKE_SYSCACHE\(\s*
			(?<syscache_name>\w+),\s*
			(?<index_name>\w+),\s*
			(?<syscache_nbuckets>\w+)\s*
			\)/x
		  )
		{
			push @{ $catalog{syscaches} }, {%+};
		}
		elsif (
			/^DECLARE_OID_DEFINING_MACRO\(\s*
			 (?<other_name>\w+),\s*
			 (?<other_oid>\d+)\s*
			 \)/x
		  )
		{
			push @{ $catalog{other_oids} }, {%+};
		}
		elsif (
			/^DECLARE_(ARRAY_)?FOREIGN_KEY(_OPT)?\(\s*
			 \((?<fk_cols>[^)]+)\),\s*
			 (?<pk_table>\w+),\s*
			 \((?<pk_cols>[^)]+)\)\s*
			 \)/x
		  )
		{
			push @{ $catalog{foreign_keys} },
			  {
				is_array => $1 ? 1 : 0,
				is_opt => $2 ? 1 : 0,
				%+,
			  };
		}
		elsif (
			/^CATALOG\(\s*
			 (?<catname>\w+),\s*
			 (?<relation_oid>\d+),\s*
			 (?<relation_oid_macro>\w+)\s*
			 \)/x
		  )
		{
			@catalog{ keys %+ } = values %+;

			$catalog{bootstrap} = /BKI_BOOTSTRAP/ ? ' bootstrap' : '';
			$catalog{shared_relation} =
			  /BKI_SHARED_RELATION/ ? ' shared_relation' : '';
			if (/BKI_ROWTYPE_OID\(\s*
				 (?<rowtype_oid>\d+),\s*
				 (?<rowtype_oid_macro>\w+)\s*
				 \)/x
			  )
			{
				@catalog{ keys %+ } = values %+;
				$catalog{rowtype_oid_clause} = " rowtype_oid $+{rowtype_oid}";
			}
			else
			{
				$catalog{rowtype_oid} = '';
				$catalog{rowtype_oid_clause} = '';
				$catalog{rowtype_oid_macro} = '';
			}
			$catalog{schema_macro} = /BKI_SCHEMA_MACRO/ ? 1 : 0;
			$declaring_attributes = 1;
		}
		elsif ($is_client_code)
		{
			if (/^#endif/)
			{
				$is_client_code = 0;
			}
			else
			{
				push @{ $catalog{client_code} }, $_;
			}
		}
		elsif ($declaring_attributes)
		{
			next if (/^{|^$/);
			if (/^}/)
			{
				$declaring_attributes = 0;
			}
			else
			{
				my %column;
				my @attopts = split /\s+/, $_;
				my $atttype = shift @attopts;
				my $attname = shift @attopts;
				die "parse error ($input_file)"
				  unless ($attname and $atttype);

				if (exists $RENAME_ATTTYPE{$atttype})
				{
					$atttype = $RENAME_ATTTYPE{$atttype};
				}

				# 如果 C 名字以 '[]' 或 '[数字]' 结尾，说明这是一个数组类型，
				# 因此我们从名字中去掉该后缀，并在类型前加上 '_'。
				if ($attname =~ /(\w+)\[\d*\]/)
				{
					$attname = $1;
					$atttype = '_' . $atttype;
				}

				$column{type} = $atttype;
				$column{name} = $attname;
				$column{is_varlen} = 1 if $is_varlen;

				foreach my $attopt (@attopts)
				{
					if ($attopt eq 'BKI_FORCE_NULL')
					{
						$column{forcenull} = 1;
					}
					elsif ($attopt eq 'BKI_FORCE_NOT_NULL')
					{
						$column{forcenotnull} = 1;
					}

					# 对于 \0 和 \054 这样的值，我们使用引号包裹，
					# 以确保所有编译器和语法高亮工具都能正确识别它们。
					elsif ($attopt =~ /BKI_DEFAULT\(['"]?([^'"]+)['"]?\)/)
					{
						$column{default} = $1;
					}
					elsif (
						$attopt =~ /BKI_ARRAY_DEFAULT\(['"]?([^'"]+)['"]?\)/)
					{
						$column{array_default} = $1;
					}
					elsif ($attopt =~ /BKI_LOOKUP(_OPT)?\((\w+)\)/)
					{
						$column{lookup} = $2;
						$column{lookup_opt} = $1 ? 1 : 0;
						# BKI_LOOKUP 会隐式地创建一个外键（FK）引用
						push @{ $catalog{foreign_keys} },
						  {
							is_array => (
								$atttype eq 'oidvector' || $atttype eq '_oid')
							? 1
							: 0,
							is_opt => $column{lookup_opt},
							fk_cols => $attname,
							pk_table => $column{lookup},
							pk_cols => 'oid'
						  };
					}
					else
					{
						die
						  "unknown or misformatted column option $attopt on column $attname";
					}

					if ($column{forcenull} and $column{forcenotnull})
					{
						die "$attname is forced both null and not null";
					}
				}
				push @{ $catalog{columns} }, \%column;
			}
		}
	}
	close $ifh;
	return \%catalog;
}

# 解析一个包含 Perl 数据结构字面量的文件，返回可用的数据。
#
# 调用者若希望处理数据文件中的非数据行（例如注释和空行），
# 需要设置 $preserve_comments 参数。如果调用者只想消费数据，则保持未设置即可。
# （当该参数被请求时，非数据行会作为字符串（而非哈希）的数组条目返回，
# 因此需要额外的代码来处理这种情况。）
sub ParseData
{
	my ($input_file, $schema, $preserve_comments) = @_;

	open(my $ifd, '<', $input_file) || die "$input_file: $!";
	$input_file =~ /(\w+)\.dat$/
	  or die "Input file $input_file needs to be a .dat file.\n";
	my $catname = $1;
	my $data = [];

	# 扫描输入文件。
	while (<$ifd>)
	{
		my $hash_ref;

		if (/{/)
		{
			# 捕获哈希引用
			# 注意：假设下一个哈希引用不会在当前哈希结束的同一行上开始。
			# 这并非万无一失，但我们应当不需要一个完整的解析器，
			# 因为我们预期输入相对规整。

			# 这是一个快速处理技巧，用于检测我们是否已经拿到一个完整的哈希引用可供解析。
			# 我们不能简单地使用正则，因为 pg_aggregate 和 pg_proc 中存在像 '{0,0}' 这样的值。
			# 如果我们以后需要允许字段值中出现不平衡的大括号，这里还需要改进。
			my $lcnt = tr/{//;
			my $rcnt = tr/}//;

			if ($lcnt == $rcnt)
			{
				# 我们把输入行当作一段 Perl 代码来处理，因此这里需要使用字符串 eval。
				# 告诉 perlcritic 我们清楚自己在做什么。
				eval "\$hash_ref = $_";    ## no critic (ProhibitStringyEval)
				if (!ref $hash_ref)
				{
					die "$input_file: error parsing line $.:\n$_\n";
				}

				# 用源代码行号为每个哈希做标注。
				$hash_ref->{line_number} = $.;

				# 将元组展开为完整表示形式。
				AddDefaultValues($hash_ref, $schema, $catname);
			}
			else
			{
				my $next_line = <$ifd>;
				die "$input_file: file ends within Perl hash\n"
				  if !defined $next_line;
				$_ .= $next_line;
				redo;
			}
		}

		# 如果我们找到了哈希引用，就保留它，除非它被标记为自动生成；
		# 否则它会与我们在下面自动生成的条目重复。（这使得 reformat_dat_file.pl
		# 在配合 --full-tuples 时打印自动生成的条目变得安全，而这看起来
		# 对调试是有用的行为。）
		#
		# 否则，我们得到的是一个非数据字符串，仅在调用者要求时才保留。
		if (defined $hash_ref)
		{
			push @$data, $hash_ref if !$hash_ref->{autogenerated};
		}
		else
		{
			push @$data, $_ if $preserve_comments;
		}
	}

	close $ifd;

	# 如果这是 pg_type，还要自动生成数组类型。
	GenerateArrayTypes($schema, $data) if $catname eq 'pg_type';

	return $data;
}

# 使用给定的结构（schema）填充记录的默认值。
# 调用者有责任在此之前指定其他值。
sub AddDefaultValues
{
	my ($row, $schema, $catname) = @_;
	my @missing_fields;

	# 计算特殊情形的列值。
	# 注意：如果你在这里新增了情形，也必须教会
	# include/catalog/reformat_dat_file.pl 中的 strip_default_values()
	# 将它们删除。
	if ($catname eq 'pg_proc')
	{
		# pg_proc.pronargs 可以由 proargtypes 推导得出。
		if (defined $row->{proargtypes})
		{
			my @proargtypes = split /\s+/, $row->{proargtypes};
			$row->{pronargs} = scalar(@proargtypes);
		}
	}

	# 现在填充默认值，并记录仍未定义的列。
	foreach my $column (@$schema)
	{
		my $attname = $column->{name};

		# 如果该字段已有值，则无需处理。
		next if defined $row->{$attname};

		# 忽略 'oid' 列，它们在其他地方处理。
		next if $attname eq 'oid';

		# 如果该列有默认值，则填入该默认值。
		if (defined $column->{default})
		{
			$row->{$attname} = $column->{default};
			next;
		}

		# 未能为该字段找到值。
		push @missing_fields, $attname;
	}

	# 未能提供全部列是致命错误。
	if (@missing_fields)
	{
		die sprintf "missing values for field(s) %s in %s.dat line %s\n",
		  join(', ', @missing_fields), $catname, $row->{line_number};
	}
}

# 如果一个 pg_type 条目带有 array_type_oid 元数据字段，
# 则为其数组类型自动生成一个条目。
sub GenerateArrayTypes
{
	my $pgtype_schema = shift;
	my $types = shift;
	my @array_types;

	foreach my $elem_type (@$types)
	{
		next if !(ref $elem_type eq 'HASH');
		next if !defined($elem_type->{array_type_oid});

		my %array_type;

		# 为数组类型设置元数据字段。
		$array_type{oid} = $elem_type->{array_type_oid};
		$array_type{autogenerated} = 1;
		$array_type{line_number} = $elem_type->{line_number};

		# 设置从元素类型派生的列值。
		$array_type{typname} = '_' . $elem_type->{typname};
		$array_type{typelem} = $elem_type->{typname};

		# 数组需要 INT 对齐，除非元素类型要求 DOUBLE 对齐。
		$array_type{typalign} = $elem_type->{typalign} eq 'd' ? 'd' : 'i';

		# 填充数组条目其余的字段。
		foreach my $column (@$pgtype_schema)
		{
			my $attname = $column->{name};

			# 如果上面已经设置过，则跳过。
			next if defined $array_type{$attname};

			# 如果存在 BKI_ARRAY_DEFAULT 设置则应用它，
			# 否则从元素类型复制该字段。
			if (defined $column->{array_default})
			{
				$array_type{$attname} = $column->{array_default};
			}
			else
			{
				$array_type{$attname} = $elem_type->{$attname};
			}
		}

		# 最后，将数组与元素类型相互链接。
		$elem_type->{typarray} = $array_type{typname};

		push @array_types, \%array_type;
	}

	push @$types, @array_types;

	return;
}

# 将临时文件重命名为最终名称。
# 调用此函数时传入最终文件名和 .tmp 扩展名。
#
# 如果最终文件已存在且内容完全相同，则不要覆盖它；
# 这种行为可以避免由于未改动头文件的修改日期被更新而引发不必要的重新编译。
#
# 注意：建议使用的扩展名是 ".tmp$$"，这样并行的 make 步骤
# 就不会使用相同的临时文件。
sub RenameTempFile
{
	my $final_name = shift;
	my $extension = shift;
	my $temp_name = $final_name . $extension;

	if (-f $final_name
		&& compare($temp_name, $final_name) == 0)
	{
		unlink($temp_name) || die "unlink: $temp_name: $!";
	}
	else
	{
		rename($temp_name, $final_name) || die "rename: $temp_name: $!";
	}
	return;
}

# 在特定的头文件中查找已定义的符号并提取其值。
# include_path 应为 src/include/ 的路径。
sub FindDefinedSymbol
{
	my ($catalog_header, $include_path, $symbol) = @_;
	my $value;

	# 确保包含路径以斜杠结尾。
	if (substr($include_path, -1) ne '/')
	{
		$include_path .= '/';
	}
	my $file = $include_path . $catalog_header;
	open(my $find_defined_symbol, '<', $file) || die "$file: $!";
	while (<$find_defined_symbol>)
	{
		if (/^#define\s+\Q$symbol\E\s+(\S+)/)
		{
			$value = $1;
			last;
		}
	}
	close $find_defined_symbol;
	return $value if defined $value;
	die "$file: no definition found for $symbol\n";
}

# 与 FindDefinedSymbol 类似，但从 bootstrap 元数据中查找。
sub FindDefinedSymbolFromData
{
	my ($data, $symbol) = @_;
	foreach my $row (@{$data})
	{
		if ($row->{oid_symbol} eq $symbol)
		{
			return $row->{oid};
		}
	}
	die "no definition found for $symbol\n";
}

# 提取指定目录头文件及其关联数据文件（如果有）中
# 分配的所有 OID 组成的数组。
# 注意：genbki.pl 中包含等价逻辑；如果你需要改动这里，也要同步改动它。
sub FindAllOidsFromHeaders
{
	my @input_files = @_;

	my @oids = ();

	foreach my $header (@input_files)
	{
		$header =~ /(.+)\.h$/
		  or die "Input files need to be header files.\n";
		my $datfile = "$1.dat";

		my $catalog = Catalog::ParseHeader($header);

		# 对于 bootstrap 目录，我们忽略其 pg_class OID 和行类型（rowtype）OID，
		# 因为这两个值预期会出现在 pg_class 和 pg_type 的初始数据中。
		# 对于普通目录，则包含这些 OID。
		if (!$catalog->{bootstrap})
		{
			push @oids, $catalog->{relation_oid}
			  if ($catalog->{relation_oid});
			push @oids, $catalog->{rowtype_oid} if ($catalog->{rowtype_oid});
		}

		# 并非所有目录都有数据文件。
		if (-e $datfile)
		{
			my $catdata =
			  Catalog::ParseData($datfile, $catalog->{columns}, 0);

			foreach my $row (@$catdata)
			{
				push @oids, $row->{oid} if defined $row->{oid};
			}
		}

		foreach my $toast (@{ $catalog->{toasting} })
		{
			push @oids, $toast->{toast_oid}, $toast->{toast_index_oid};
		}
		foreach my $index (@{ $catalog->{indexing} })
		{
			push @oids, $index->{index_oid};
		}
		foreach my $other (@{ $catalog->{other_oids} })
		{
			push @oids, $other->{other_oid};
		}
	}

	return \@oids;
}

1;
