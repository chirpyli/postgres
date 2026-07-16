#!/usr/bin/perl

# 检查 gram.y 和 kwlist.h 中的关键字列表是否合理。
# 用法：check_keywords.pl gram.y kwlist.h

# src/backend/parser/check_keywords.pl
# Copyright (c) 2009-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

my $gram_filename = $ARGV[0];
my $kwlist_filename = $ARGV[1];

my $errors = 0;

sub error
{
	print STDERR @_;
	$errors = 1;
	return;
}

# 检查一组关键字符号是否按字母顺序排列
# （注意：这些并不是实际的关键字字符串）
sub check_alphabetical_order
{
	my ($listname, $list) = @_;
	my $prevkword = '';

	foreach my $kword (@$list)
	{
		# 部分符号带有 _P 后缀，比较时将其去掉。
		my $bare_kword = $kword;
		$bare_kword =~ s/_P$//;
		if ($bare_kword le $prevkword)
		{
			error
			  "'$bare_kword' after '$prevkword' in $listname list is misplaced";
		}
		$prevkword = $bare_kword;
	}
	return;
}

$, = ' ';     # 设置输出字段分隔符
$\ = "\n";    # 设置输出记录分隔符

my %keyword_categories;
$keyword_categories{'unreserved_keyword'} = 'UNRESERVED_KEYWORD';
$keyword_categories{'col_name_keyword'} = 'COL_NAME_KEYWORD';
$keyword_categories{'type_func_name_keyword'} = 'TYPE_FUNC_NAME_KEYWORD';
$keyword_categories{'reserved_keyword'} = 'RESERVED_KEYWORD';

open(my $gram, '<', $gram_filename) || die("Could not open : $gram_filename");

my $kcat;
my $in_bare_labels;
my $comment;
my @arr;
my %keywords;
my @bare_label_keywords;

line: while (my $S = <$gram>)
{
	chomp $S;    # strip record separator

	my $s;

	# 确保花括号被分割开
	$s = '{', $S =~ s/$s/ { /g;
	$s = '}', $S =~ s/$s/ } /g;

	# 将任何注释分割开
	$s = '[/][*]', $S =~ s#$s# /* #g;
	$s = '[*][/]', $S =~ s#$s# */ #g;

	if (!($kcat) && !($in_bare_labels))
	{

		# 这是否是一个关键字列表的开头？
		foreach my $k (keys %keyword_categories)
		{
			if ($S =~ m/^($k):/)
			{
				$kcat = $k;
				next line;
			}
		}

		# 这是否是 bare_label_keyword 列表的开头？
		$in_bare_labels = 1 if ($S =~ m/^bare_label_keyword:/);

		next line;
	}

	# 现在将该行拆分为单独的字段
	my $n = (@arr = split(' ', $S));

	# 好，我们已经处于一个关键字列表中，逐个字段处理
	for (my $fieldIndexer = 0; $fieldIndexer < $n; $fieldIndexer++)
	{
		if ($arr[$fieldIndexer] eq '*/' && $comment)
		{
			$comment = 0;
			next;
		}
		elsif ($comment)
		{
			next;
		}
		elsif ($arr[$fieldIndexer] eq '/*')
		{

			# 多行注释的开始
			$comment = 1;
			next;
		}
		elsif ($arr[$fieldIndexer] eq '//')
		{
			next line;
		}

		if ($arr[$fieldIndexer] eq ';')
		{

			# 关键字列表结束
			undef $kcat;
			undef $in_bare_labels;
			next;
		}

		if ($arr[$fieldIndexer] eq '|')
		{
			next;
		}

		# 将该关键字放入正确的列表中
		if ($in_bare_labels)
		{
			push @bare_label_keywords, $arr[$fieldIndexer];
		}
		else
		{
			push @{ $keywords{$kcat} }, $arr[$fieldIndexer];
		}
	}
}
close $gram;

# 检查每个关键字列表是否按字母顺序排列（仅出于整洁性考虑）
check_alphabetical_order($_, $keywords{$_}) for (keys %keyword_categories);
check_alphabetical_order('bare_label_keyword', \@bare_label_keywords);

# 将关键字列表转换为哈希表。
# kwhashes 是一个哈希的哈希，以关键字类别 id（例如 UNRESERVED_KEYWORD）作为键。
# 每个内层哈希以关键字 id（例如 ABORT_P）作为键，取值为占位值。
my %kwhashes;
while (my ($kcat, $kcat_id) = each(%keyword_categories))
{
	@arr = @{ $keywords{$kcat} };

	my $hash;
	foreach my $item (@arr) { $hash->{$item} = 1; }

	$kwhashes{$kcat_id} = $hash;
}
my %bare_label_keywords = map { $_ => 1 } @bare_label_keywords;

# 现在读入 kwlist.h

open(my $kwlist, '<', $kwlist_filename)
  || die("Could not open : $kwlist_filename");

my $prevkwstring = '';
my $bare_kwname;
my %kwhash;
kwlist_line: while (<$kwlist>)
{
	my ($line) = $_;

	if ($line =~ /^PG_KEYWORD\(\"(.*)\", (.*), (.*), (.*)\)/)
	{
		my ($kwstring) = $1;
		my ($kwname) = $2;
		my ($kwcat_id) = $3;
		my ($collabel) = $4;

		# 检查列表是否按字母顺序排列（关键！）
		if ($kwstring le $prevkwstring)
		{
			error
			  "'$kwstring' after '$prevkwstring' in kwlist.h is misplaced";
		}
		$prevkwstring = $kwstring;

		# 检查关键字字符串是否合法：必须全部为小写 ASCII 字符
		if ($kwstring !~ /^[a-z_]+$/)
		{
			error
			  "'$kwstring' is not a valid keyword string, must be all lower-case ASCII chars";
		}

		# 检查关键字名称是否合法：必须全部为大写 ASCII 字符
		if ($kwname !~ /^[A-Z_]+$/)
		{
			error
			  "'$kwname' is not a valid keyword name, must be all upper-case ASCII chars";
		}

		# 检查关键字字符串是否与关键字名称匹配
		$bare_kwname = $kwname;
		$bare_kwname =~ s/_P$//;
		if ($bare_kwname ne uc($kwstring))
		{
			error
			  "keyword name '$kwname' doesn't match keyword string '$kwstring'";
		}

		# 检查该关键字是否存在于正确的类别列表中
		%kwhash = %{ $kwhashes{$kwcat_id} };

		if (!(%kwhash))
		{
			error "Unknown keyword category: $kwcat_id";
		}
		else
		{
			if (!($kwhash{$kwname}))
			{
				error "'$kwname' not present in $kwcat_id section of gram.y";
			}
			else
			{

				# 将其从哈希中移除，以便最后检查是否还有
				# 未被 kwlist.h 匹配到的关键字残留
				delete $kwhashes{$kwcat_id}->{$kwname};
			}
		}

		# 检查关键字的 collabel 属性是否与 gram.y 一致
		if ($collabel eq 'BARE_LABEL')
		{
			unless ($bare_label_keywords{$kwname})
			{
				error
				  "'$kwname' is marked as BARE_LABEL in kwlist.h, but it is missing from gram.y's bare_label_keyword rule";
			}
		}
		elsif ($collabel eq 'AS_LABEL')
		{
			if ($bare_label_keywords{$kwname})
			{
				error
				  "'$kwname' is marked as AS_LABEL in kwlist.h, but it is listed in gram.y's bare_label_keyword rule";
			}
		}
		else
		{
			error
			  "'$collabel' not recognized in kwlist.h.  Expected either 'BARE_LABEL' or 'AS_LABEL'";
		}
	}
}
close $kwlist;

# 检查是否已将 gram.y 中的所有关键字与 kwlist.h 中的行一一配对
while (my ($kwcat, $kwcat_id) = each(%keyword_categories))
{
	%kwhash = %{ $kwhashes{$kwcat_id} };

	for my $kw (keys %kwhash)
	{
		error "'$kw' found in gram.y $kwcat category, but not in kwlist.h";
	}
}

exit $errors;
