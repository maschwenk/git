#!/bin/sh

test_description='opt-in copy-on-write checkout cache'

. ./test-lib.sh
. "$TEST_DIRECTORY/lib-parallel-checkout.sh"
. "$TEST_DIRECTORY/lib-encoding.sh"

# Probe the filesystem independently of checkout, so a broken implementation
# cannot turn the clone assertions into skipped tests.
test_lazy_prereq COW_FS '
	test-tool genrandom cow-probe 8192 >source || return 125
	case "$uname_s" in
	Darwin) cp -c source destination ;;
	Linux) cp --reflink=always source destination ;;
	*) return 1 ;;
	esac &&
	test_cmp_bin source destination
'

cache_root () {
	local common &&
	common=$(git -C "$1" rev-parse --path-format=absolute --git-common-dir) &&
	echo "$common/checkout-cache"
}

cache_path () {
	local root format &&
	root=$(cache_root "$1") &&
	format=$(git -C "$1" rev-parse --show-object-format) &&
	echo "$root/v1/$format/$(test_oid_to_path "$2")"
}

setup_repo () {
	git init "$1" &&
	(
		cd "$1" &&
		git config core.autocrlf false &&
		git config checkout.cowCache true &&
		git config checkout.workers 1 &&
		git config checkout.thresholdForParallelism 0 &&
		test-tool genrandom cow-file 8192 >file &&
		test-tool genrandom cow-other 8192 >other &&
		cp file executable &&
		git add file other &&
		test_chmod +x executable &&
		git commit -m fixture
	)
}

setup_text_repo () {
	git init "$1" &&
	(
		cd "$1" &&
		git config core.autocrlf false &&
		git config checkout.cowCache true &&
		git config checkout.workers 1 &&
		git config checkout.thresholdForParallelism 0 &&
		printf 'one line\n$Id$\nsecond line\n' >file &&
		cp file ident &&
		cp file filtered &&
		cp file text &&
		git add file ident filtered text &&
		git commit -m text
	)
}

checkout_with_trace () {
	local trace="$TRASH_DIRECTORY/$1" &&
	shift &&
	rm -f "$trace" &&
	(
		GIT_TRACE2_EVENT="$trace" &&
		export GIT_TRACE2_EVENT &&
		"$@"
	)
}

assert_cloned () {
	test_trace2_data checkout cow_cache/clone "$2" <"$1"
}

assert_no_clones () {
	test_grep ! '"category":"checkout","key":"cow_cache/clone"' "$1"
}

# A worker-start count alone does not prove that the workers used the cache.
assert_clone_workers () {
	sed -n '/"event":"cmd_name".*"name":"checkout--worker"/s/.*"sid":"\([^"]*\)".*/\1/p' "$1" |
		sort -u >expect-workers &&
	test_line_count = 2 expect-workers &&
	sed -n '/"category":"checkout","key":"cow_cache\/clone"/s/.*"sid":"\([^"]*\)".*/\1/p' "$1" |
		sort -u >actual-workers &&
	test_cmp expect-workers actual-workers
}

assert_fixture () {
	local oid actual &&
	for path in file other executable
	do
		oid=$(git -C "$1" rev-parse "HEAD:$path") &&
		actual=$(git -C "$1" hash-object "$path") &&
		test "$oid" = "$actual" || return 1
	done &&
	verify_checkout "$1"
}

# Even with --force, checkout-index can skip a file whose stat data matches
# the index. Remove every target before the checkout under test.
test_expect_success 'checkout cache is disabled by default' '
	setup_repo default &&
	git -C default config --unset checkout.cowCache &&
	rm default/file default/other default/executable &&
	checkout_with_trace default.trace \
		git -C default checkout-index --index --all --force >default.out &&
	test_must_be_empty default.out &&
	test_path_is_missing "$(cache_root default)" &&
	assert_no_clones default.trace &&
	assert_fixture default
'

test_expect_success 'checkout.cowCache=false does not create a cache' '
	setup_repo disabled &&
	rm disabled/file disabled/other disabled/executable &&
	checkout_with_trace disabled.trace \
		git -C disabled -c checkout.cowCache=false \
		checkout-index --index --all --force >disabled.out &&
	test_must_be_empty disabled.out &&
	test_path_is_missing "$(cache_root disabled)" &&
	assert_no_clones disabled.trace &&
	assert_fixture disabled
'

for workers in 1 2
do
	case $workers in
	1) expected_workers=0 ;;
	2) expected_workers=2 ;;
	esac

	test_expect_success COW_FS "cold cache clones with $workers checkout workers" '
		repo=basic-$workers &&
		setup_repo "$repo" &&
		git -C "$repo" config checkout.workers $workers &&
		oid=$(git -C "$repo" rev-parse HEAD:file) &&
		other_oid=$(git -C "$repo" rev-parse HEAD:other) &&
		rm "$repo/file" "$repo/other" "$repo/executable" &&
		checkout_with_trace cold-$workers.trace \
			test_checkout_workers $expected_workers \
			git -C "$repo" checkout-index --index --all --force >cold-$workers.out &&
		test_must_be_empty cold-$workers.out &&
		test_grep "\"category\":\"checkout\",\"key\":\"cow_cache/clone\"" cold-$workers.trace &&
		if test "$workers" = 1
		then
			assert_cloned cold-$workers.trace "$oid" &&
			assert_cloned cold-$workers.trace "$other_oid" &&
			test_path_is_file_not_symlink "$(cache_path "$repo" "$oid")" &&
			test_path_is_file_not_symlink "$(cache_path "$repo" "$other_oid")"
		fi &&
		for path in file other
		do
			cache=$(cache_path "$repo" "$(git -C "$repo" rev-parse HEAD:$path)") &&
			if test -f "$cache"
			then
				test_path_is_file_not_symlink "$cache" &&
				test_cmp_bin "$repo/$path" "$cache"
			fi || return 1
		done &&
		find "$(cache_root "$repo")" -type f ! -name writer.lock >cache-files &&
		test_line_count -gt 0 cache-files &&
		test_line_count -le 2 cache-files &&
		assert_fixture "$repo"
	'

	test_expect_success COW_FS "warm cache clones with $workers checkout workers" '
		repo=basic-$workers &&
		oid=$(git -C "$repo" rev-parse HEAD:file) &&
		other_oid=$(git -C "$repo" rev-parse HEAD:other) &&
		rm "$repo/file" "$repo/other" "$repo/executable" &&
		git -C "$repo" -c checkout.workers=1 checkout-index --index --all --force &&
		rm "$repo/file" "$repo/other" "$repo/executable" &&
		checkout_with_trace warm-$workers.trace \
			test_checkout_workers $expected_workers \
			git -C "$repo" checkout-index --index --all >warm-$workers.out &&
		test_must_be_empty warm-$workers.out &&
		assert_cloned warm-$workers.trace "$oid" &&
		assert_cloned warm-$workers.trace "$other_oid" &&
		test_cmp_bin "$repo/file" "$(cache_path "$repo" "$oid")" &&
		test_cmp_bin "$repo/other" "$(cache_path "$repo" "$other_oid")" &&
		find "$(cache_root "$repo")" -type f ! -name writer.lock >cache-files &&
		test_line_count = 2 cache-files &&
		if test "$workers" = 2
		then
			assert_clone_workers warm-$workers.trace
		fi &&
		assert_fixture "$repo"
	'

	test_expect_success "same-size corrupt cache falls back with $workers workers" '
		repo=corrupt-$workers &&
		setup_repo "$repo" &&
		oid=$(git -C "$repo" rev-parse HEAD:file) &&
		cache=$(cache_path "$repo" "$oid") &&
		(umask 077 && mkdir -p "${cache%/*}") &&
		test-tool genrandom poison 8192 >"$cache" &&
		chmod 444 "$cache" &&
		test "$(test_file_size "$cache")" = "$(test_file_size "$repo/file")" &&
		test "$oid" != "$(git -C "$repo" hash-object "$cache")" &&
		git -C "$repo" cat-file blob HEAD:file >expect &&
		rm "$repo/file" "$repo/executable" &&
		checkout_with_trace corrupt-$workers.trace \
			git -C "$repo" -c checkout.workers=$workers \
			checkout-index --index --force file executable &&
		assert_no_clones corrupt-$workers.trace &&
		test_cmp_bin expect "$repo/file" &&
		test_cmp_bin expect "$repo/executable" &&
		assert_fixture "$repo"
	'

	test_expect_success SYMLINKS "cache symlink is not followed with $workers workers" '
		repo=symlink-$workers &&
		setup_repo "$repo" &&
		oid=$(git -C "$repo" rev-parse HEAD:file) &&
		cache=$(cache_path "$repo" "$oid") &&
		(umask 077 && mkdir -p "${cache%/*}") &&
		cp "$repo/file" symlink-target-$workers &&
		chmod 444 symlink-target-$workers &&
		ln -s "$TRASH_DIRECTORY/symlink-target-$workers" "$cache" &&
		rm "$repo/file" "$repo/executable" &&
		checkout_with_trace symlink-$workers.trace \
			git -C "$repo" -c checkout.workers=$workers \
			checkout-index --index --force file executable &&
		assert_no_clones symlink-$workers.trace &&
		test -h "$cache" &&
		test_cmp_bin "$repo/file" symlink-target-$workers &&
		assert_fixture "$repo"
	'

	test_expect_success "unavailable cache falls back with $workers workers" '
		repo=unavailable-$workers &&
		setup_repo "$repo" &&
		root=$(cache_root "$repo") &&
		echo obstruction >"$root" &&
		rm "$repo/file" "$repo/other" "$repo/executable" &&
		checkout_with_trace unavailable-$workers.trace \
			git -C "$repo" -c checkout.workers=$workers \
			checkout-index --index --all --force &&
		assert_no_clones unavailable-$workers.trace &&
		test_grep obstruction "$root" &&
		assert_fixture "$repo"
	'

	test_expect_success SANITY "read-only cache falls back with $workers workers" '
		repo=readonly-$workers &&
		setup_repo "$repo" &&
		root=$(cache_root "$repo") &&
		mkdir "$root" &&
		test_when_finished "chmod u+w \"$root\"" &&
		chmod 500 "$root" &&
		rm "$repo/file" "$repo/other" "$repo/executable" &&
		checkout_with_trace readonly-$workers.trace \
			git -C "$repo" -c checkout.workers=$workers \
			checkout-index --index --all --force &&
		assert_no_clones readonly-$workers.trace &&
		find "$root" -type f >cache-files &&
		test_must_be_empty cache-files &&
		assert_fixture "$repo"
	'

	test_expect_success "changed attributes bypass warmed blobs with $workers workers" '
		repo=attributes-$workers &&
		setup_text_repo "$repo" &&
		oid=$(git -C "$repo" rev-parse HEAD:file) &&
		rm "$repo/file" "$repo/ident" "$repo/filtered" "$repo/text" &&
		checkout_with_trace attr-warm-$workers.trace \
			git -C "$repo" checkout-index --index --all --force &&
		if test_have_prereq COW_FS
		then
			assert_cloned attr-warm-$workers.trace "$oid"
		fi &&
		git -C "$repo" config filter.rot13.clean "tr a-zA-Z n-za-mN-ZA-M" &&
		git -C "$repo" config filter.rot13.smudge "tr a-zA-Z n-za-mN-ZA-M" &&
		git -C "$repo" config filter.rot13.required true &&
		cat >"$repo/.git/info/attributes" <<-EOF &&
		file text eol=crlf
		ident -text ident
		filtered -text filter=rot13
		EOF
		rm "$repo/file" "$repo/ident" "$repo/filtered" &&
		checkout_with_trace attributes-$workers.trace \
			test_checkout_workers $expected_workers \
			git -C "$repo" -c checkout.workers=$workers \
			checkout-index --index --force file ident filtered &&
		assert_no_clones attributes-$workers.trace &&
		printf "one line\r\n\$Id\$\r\nsecond line\r\n" >expect-crlf &&
		printf "one line\n\$Id: %s \$\nsecond line\n" "$oid" >expect-ident &&
		printf "bar yvar\n\$Vq\$\nfrpbaq yvar\n" >expect-filtered &&
		git -C "$repo" cat-file blob HEAD:file >expect-raw &&
		test_cmp_bin expect-crlf "$repo/file" &&
		test_cmp expect-ident "$repo/ident" &&
		test_cmp expect-filtered "$repo/filtered" &&
		test_cmp expect-raw "$repo/text" &&
		echo "text text eol=lf" >>"$repo/.git/info/attributes" &&
		rm "$repo/text" &&
		checkout_with_trace text-input-$workers.trace \
			git -C "$repo" -c checkout.workers=$workers checkout-index --index --force text &&
		if test_have_prereq COW_FS
		then
			assert_cloned text-input-$workers.trace "$oid"
		fi &&
		test_cmp expect-raw "$repo/text" &&
		echo "text text=auto eol=lf" >>"$repo/.git/info/attributes" &&
		rm "$repo/text" &&
		checkout_with_trace auto-input-$workers.trace \
			git -C "$repo" -c checkout.workers=$workers checkout-index --index --force text &&
		if test_have_prereq COW_FS
		then
			assert_cloned auto-input-$workers.trace "$oid"
		fi &&
		test_cmp expect-raw "$repo/text" &&
		verify_checkout "$repo"
	'

	test_expect_success "core.autocrlf bypasses warmed blobs with $workers workers" '
		repo=autocrlf-$workers &&
		setup_text_repo "$repo" &&
		rm "$repo/file" "$repo/ident" "$repo/filtered" "$repo/text" &&
		git -C "$repo" checkout-index --index --all --force &&
		git -C "$repo" config core.autocrlf true &&
		rm "$repo/file" "$repo/ident" "$repo/filtered" "$repo/text" &&
		checkout_with_trace autocrlf-$workers.trace \
			test_checkout_workers $expected_workers \
			git -C "$repo" -c checkout.workers=$workers \
			checkout-index --index --all --force &&
		assert_no_clones autocrlf-$workers.trace &&
		printf "one line\r\n\$Id\$\r\nsecond line\r\n" >expect &&
		for path in file ident filtered text
		do
			test_cmp_bin expect "$repo/$path" || return 1
		done &&
		verify_checkout "$repo"
	'

	test_expect_success ICONV "encoding bypasses warmed blobs with $workers workers" '
		repo=encoding-$workers &&
		setup_text_repo "$repo" &&
		rm "$repo/file" "$repo/ident" "$repo/filtered" "$repo/text" &&
		git -C "$repo" checkout-index --index --all --force &&
		echo "* working-tree-encoding=UTF-16" >"$repo/.git/info/attributes" &&
		git -C "$repo" cat-file blob HEAD:file >utf8 &&
		write_utf16 <utf8 >expect &&
		rm "$repo/file" "$repo/ident" "$repo/filtered" "$repo/text" &&
		checkout_with_trace encoding-$workers.trace \
			test_checkout_workers $expected_workers \
			git -C "$repo" -c checkout.workers=$workers \
			checkout-index --index --all --force &&
		assert_no_clones encoding-$workers.trace &&
		for path in file ident filtered text
		do
			test_cmp_bin expect "$repo/$path" || return 1
		done &&
		verify_checkout "$repo"
	'

	test_expect_success "replacement blobs bypass cache with $workers workers" '
		repo=replace-$workers &&
		setup_repo "$repo" &&
		rm "$repo/file" "$repo/other" "$repo/executable" &&
		git -C "$repo" checkout-index --index --all --force &&
		oid=$(git -C "$repo" rev-parse HEAD:file) &&
		echo replacement-content >replacement &&
		replacement_oid=$(git -C "$repo" hash-object -w "$TRASH_DIRECTORY/replacement") &&
		git -C "$repo" replace "$oid" "$replacement_oid" &&
		rm "$repo/file" "$repo/executable" &&
		checkout_with_trace replace-$workers.trace \
			git -C "$repo" -c checkout.workers=$workers \
			checkout-index --index --force file executable &&
		assert_no_clones replace-$workers.trace &&
		test_cmp replacement "$repo/file" &&
		test_cmp replacement "$repo/executable" &&
		git -C "$repo" status --porcelain >enabled-status &&
		rm "$repo/file" "$repo/executable" &&
		git -C "$repo" -c checkout.cowCache=false \
			checkout-index --index --force file executable &&
		git -C "$repo" status --porcelain >disabled-status &&
		test_cmp enabled-status disabled-status
	'
done

test_expect_success !COW_FS 'unsupported filesystem retains ordinary checkout behavior' '
	setup_repo unsupported &&
	rm unsupported/file unsupported/other unsupported/executable &&
	checkout_with_trace unsupported.trace \
		git -C unsupported checkout-index --index --all --force &&
	assert_no_clones unsupported.trace &&
	assert_fixture unsupported
'

test_expect_success SYMLINKS 'symlinked cache directories are not traversed' '
	for component in root version hash fanout
	do
		repo=symlink-dir-$component &&
		setup_repo "$repo" &&
		oid=$(git -C "$repo" rev-parse HEAD:file) &&
		root=$(cache_root "$repo") &&
		cache=$(cache_path "$repo" "$oid") &&
		case $component in
		root) dir=$root ;;
		version) dir=$root/v1 ;;
		hash) dir=${cache%/*/*} ;;
		fanout) dir=${cache%/*} ;;
		esac &&
		(umask 077 && mkdir -p "${dir%/*}") &&
		(umask 077 && mkdir outside-$component) &&
		ln -s "$TRASH_DIRECTORY/outside-$component" "$dir" &&
		rm "$repo/file" "$repo/executable" &&
		checkout_with_trace symlink-dir-$component.trace \
			git -C "$repo" checkout-index --index --force file executable &&
		assert_no_clones symlink-dir-$component.trace &&
		test -h "$dir" &&
		find outside-$component >outside-files &&
		echo outside-$component >expect-outside &&
		test_cmp expect-outside outside-files &&
		assert_fixture "$repo" || return 1
	done
'

test_expect_success COW_FS 'linked worktrees share cache and isolate in-place writes and truncation' '
	setup_repo shared &&
	oid=$(git -C shared rev-parse HEAD:file) &&
	checkout_with_trace shared-a.trace \
		git -C shared worktree add --detach "$TRASH_DIRECTORY/shared-a" HEAD &&
	checkout_with_trace shared-b.trace \
		git -C shared worktree add --detach "$TRASH_DIRECTORY/shared-b" HEAD &&
	assert_cloned shared-a.trace "$oid" &&
	assert_cloned shared-b.trace "$oid" &&
	cache=$(cache_path shared "$oid") &&
	test "$cache" = "$(cache_path shared-a "$oid")" &&
	test "$cache" = "$(cache_path shared-b "$oid")" &&
	git -C shared cat-file blob HEAD:file >expect &&
	printf changed | dd of=shared-a/file bs=1 conv=notrunc &&
	test_cmp_bin expect shared-b/file &&
	test_cmp_bin expect "$cache" &&
	: >shared-b/file &&
	test_file_not_empty shared-a/file &&
	test_cmp_bin expect shared-a/executable &&
	test_cmp_bin expect shared-b/executable &&
	test_cmp_bin expect "$cache" &&
	checkout_with_trace shared-after.trace \
		git -C shared worktree add --detach "$TRASH_DIRECTORY/shared-after" HEAD &&
	assert_cloned shared-after.trace "$oid" &&
	assert_fixture shared-after &&
	assert_fixture shared &&
	git -C shared-a diff --name-only >actual-a &&
	git -C shared-b diff --name-only >actual-b &&
	echo file >expect-dirty &&
	test_cmp expect-dirty actual-a &&
	test_cmp expect-dirty actual-b
'

test_expect_success COW_FS,POSIXPERM,FILEMODE 'mode changes are independent of cache and sibling worktrees' '
	cache=$(cache_path shared "$(git -C shared rev-parse HEAD:file)") &&
	before_file=$(test_modebits shared-after/file) &&
	before_exec=$(test_modebits shared-after/executable) &&
	chmod 700 shared-a/file &&
	chmod 600 shared-a/executable &&
	test "$(test_modebits "$cache")" = -r--r--r-- &&
	test "$(test_modebits shared-after/file)" = "$before_file" &&
	test "$(test_modebits shared-after/executable)" = "$before_exec" &&
	test "$(test_modebits shared-b/executable)" = "$before_exec" &&
	assert_fixture shared-after &&
	assert_fixture shared &&
	rm shared-after/file shared-after/other shared-after/executable &&
	checkout_with_trace mode-after.trace \
		git -C shared-after checkout-index --index --all --force &&
	assert_cloned mode-after.trace "$(git -C shared rev-parse HEAD:file)" &&
	test "$(test_modebits shared-after/file)" = "$before_file" &&
	test "$(test_modebits shared-after/executable)" = "$before_exec" &&
	assert_fixture shared-after
'

for mask in 0027 0077
do
	case $mask in
	0027) file_mode=-rw-r----- exec_mode=-rwxr-x--- ;;
	0077) file_mode=-rw------- exec_mode=-rwx------ ;;
	esac

	test_expect_success COW_FS,POSIXPERM "cache is private and checkout respects umask $mask" '
		repo=umask-$mask &&
		setup_repo "$repo" &&
		git -C "$repo" config core.sharedRepository group &&
		oid=$(git -C "$repo" rev-parse HEAD:file) &&
		rm "$repo/file" "$repo/other" "$repo/executable" &&
		(
			umask $mask &&
			checkout_with_trace umask-$mask.trace \
				git -C "$repo" checkout-index --index --all --force
		) &&
		assert_cloned umask-$mask.trace "$oid" &&
		test "$(test_modebits "$repo/file")" = "$file_mode" &&
		test "$(test_modebits "$repo/executable")" = "$exec_mode" &&
		cache=$(cache_path "$repo" "$oid") &&
		test "$(test_modebits "$cache")" = -r--r--r-- &&
		for dir in "$(cache_root "$repo")" "${cache%/*/*/*}" "${cache%/*/*}" "${cache%/*}"
		do
			test "$(test_modebits "$dir")" = drwx------ || return 1
		done &&
		assert_fixture "$repo"
	'
done

run_concurrent_checkouts () {
	(
		cd race-a &&
		checkout_with_trace race-a.trace \
			test_checkout_workers 2 git reset --hard HEAD
	) >race-a.out 2>race-a.err &
	local pid_a=$!
	(
		cd race-b &&
		checkout_with_trace race-b.trace \
			test_checkout_workers 2 git reset --hard HEAD
	) >race-b.out 2>race-b.err &
	local pid_b=$!
	local status_a=0 status_b=0
	wait "$pid_a" || status_a=$?
	wait "$pid_b" || status_b=$?
	test "$status_a" = 0 && test "$status_b" = 0
}

test_expect_success COW_FS 'concurrent worktrees publish complete reusable cache entries' '
	setup_repo race &&
	git -C race config checkout.workers 2 &&
	git -C race worktree add --detach --no-checkout "$TRASH_DIRECTORY/race-a" HEAD &&
	git -C race worktree add --detach --no-checkout "$TRASH_DIRECTORY/race-b" HEAD &&
	run_concurrent_checkouts &&
	assert_fixture race-a &&
	assert_fixture race-b &&
	cat race-a.trace race-b.trace >race.trace &&
	test_grep "\"category\":\"checkout\",\"key\":\"cow_cache/clone\"" race.trace &&
	for path in file other
	do
		oid=$(git -C race rev-parse HEAD:$path) &&
		cache=$(cache_path race "$oid") &&
		if test -f "$cache"
		then
			test_path_is_file_not_symlink "$cache" &&
			test_cmp_bin "race/$path" "$cache"
		fi || return 1
	done &&
	find "$(cache_root race)" -type f ! -name writer.lock >cache-files &&
	test_line_count -gt 0 cache-files &&
	test_line_count -le 2 cache-files &&
	rm race/file race/other race/executable &&
	git -C race -c checkout.workers=1 checkout-index --index --all --force &&
	rm race-a/file race-a/other race-a/executable &&
	checkout_with_trace race-after.trace \
		test_checkout_workers 2 git -C race-a checkout-index --index --all --force &&
	assert_clone_workers race-after.trace &&
	assert_fixture race-a &&
	assert_fixture race-b
'

for limit in bytes entries
do
	case $limit in
	bytes) key=checkout.cowCacheMaxBytes value=8192 ;;
	entries) key=checkout.cowCacheMaxEntries value=1 ;;
	esac

	test_expect_success COW_FS "$limit limit blocks new blobs but retains existing hits" '
		repo=limit-$limit &&
		setup_repo "$repo" &&
		git -C "$repo" config "$key" "$value" &&
		oid=$(git -C "$repo" rev-parse HEAD:file) &&
		other_oid=$(git -C "$repo" rev-parse HEAD:other) &&
		rm "$repo/file" &&
		checkout_with_trace limit-fill-$limit.trace \
			git -C "$repo" checkout-index --index --force file &&
		assert_cloned limit-fill-$limit.trace "$oid" &&
		test_cmp_bin "$repo/file" "$(cache_path "$repo" "$oid")" &&
		rm "$repo/other" &&
		checkout_with_trace limit-miss-$limit.trace \
			git -C "$repo" checkout-index --index --force other &&
		assert_no_clones limit-miss-$limit.trace &&
		test_path_is_missing "$(cache_path "$repo" "$other_oid")" &&
		git -C "$repo" config "$key" 0 &&
		rm "$repo/file" "$repo/executable" &&
		checkout_with_trace limit-zero-hit-$limit.trace \
			git -C "$repo" checkout-index --index --force file executable &&
		assert_cloned limit-zero-hit-$limit.trace "$oid" &&
		rm "$repo/other" &&
		checkout_with_trace limit-zero-miss-$limit.trace \
			git -C "$repo" checkout-index --index --force other &&
		assert_no_clones limit-zero-miss-$limit.trace &&
		test_path_is_missing "$(cache_path "$repo" "$other_oid")" &&
		assert_fixture "$repo"
	'
done

test_expect_success COW_FS 'clearing shared cache preserves worktrees and allows repopulation' '
	setup_repo clear &&
	rm clear/file clear/other clear/executable &&
	git -C clear checkout-index --index --all --force &&
	oid=$(git -C clear rev-parse HEAD:file) &&
	cache=$(cache_path clear "$oid") &&
	test_path_is_file_not_symlink "$cache" &&
	checkout_with_trace clear-linked.trace \
		git -C clear worktree add --detach "$TRASH_DIRECTORY/clear-linked" HEAD &&
	assert_cloned clear-linked.trace "$oid" &&
	echo abandoned >"${cache%/*}/tmp-abandoned" &&
	find "$(cache_root clear)" -type d | sort >dirs-before &&
	git -C clear-linked checkout-cache --clear &&
	find "$(cache_root clear)" -type d | sort >dirs-after &&
	test_cmp dirs-before dirs-after &&
	test_path_is_file_not_symlink "${cache%/*/*}/writer.lock" &&
	find "$(cache_root clear)" -type f ! -name writer.lock >cache-files &&
	test_must_be_empty cache-files &&
	assert_fixture clear &&
	assert_fixture clear-linked &&
	rm clear-linked/file clear-linked/other clear-linked/executable &&
	checkout_with_trace clear-after.trace \
		git -C clear-linked checkout-index --index --all --force &&
	assert_cloned clear-after.trace "$oid" &&
	test_cmp_bin clear/file "$cache" &&
	assert_fixture clear &&
	assert_fixture clear-linked
'

test_expect_success COW_FS 'cached content cannot hide a missing object' '
	setup_repo missing-object &&
	oid=$(git -C missing-object rev-parse HEAD:file) &&
	rm missing-object/file &&
	checkout_with_trace missing-warm.trace \
		git -C missing-object checkout-index --index --force file &&
	assert_cloned missing-warm.trace "$oid" &&
	test_path_is_file_not_symlink "$(cache_path missing-object "$oid")" &&
	rm "missing-object/.git/objects/$(test_oid_to_path "$oid")" &&
	test_must_fail git -C missing-object cat-file -e "$oid" &&
	rm missing-object/file &&
	checkout_with_trace missing-object.trace \
		test_must_fail git -C missing-object checkout-index --index --force file &&
	assert_no_clones missing-object.trace
'

test_expect_success COW_FS 'zero byte limit rejects empty misses but allows existing empty hits' '
	setup_repo empty-limit &&
	>empty-limit/empty &&
	git -C empty-limit add empty &&
	git -C empty-limit commit -m empty &&
	oid=$(git -C empty-limit rev-parse HEAD:empty) &&
	git -C empty-limit config checkout.cowCacheMaxBytes 0 &&
	rm empty-limit/empty &&
	checkout_with_trace empty-miss.trace \
		git -C empty-limit checkout-index --index empty &&
	assert_no_clones empty-miss.trace &&
	test_path_is_missing "$(cache_path empty-limit "$oid")" &&
	test_must_be_empty empty-limit/empty &&
	git -C empty-limit config checkout.cowCacheMaxBytes 1 &&
	rm empty-limit/empty &&
	checkout_with_trace empty-fill.trace \
		git -C empty-limit checkout-index --index empty &&
	assert_cloned empty-fill.trace "$oid" &&
	git -C empty-limit config checkout.cowCacheMaxBytes 0 &&
	rm empty-limit/empty &&
	checkout_with_trace empty-hit.trace \
		git -C empty-limit checkout-index --index empty &&
	assert_cloned empty-hit.trace "$oid" &&
	test_must_be_empty empty-limit/empty &&
	verify_checkout empty-limit
'

test_lazy_prereq COW_MAC_XATTR '
	test "$uname_s" = Darwin &&
	>metadata-probe &&
	xattr -w com.git.checkout-cow probe metadata-probe &&
	xattr -p com.git.checkout-cow metadata-probe >actual &&
	echo probe >expect &&
	test_cmp expect actual &&
	xattr -d com.git.checkout-cow metadata-probe
'

test_lazy_prereq COW_MAC_FLAGS '
	test "$uname_s" = Darwin &&
	>flags-probe &&
	chflags uchg flags-probe &&
	chflags nouchg flags-probe
'

for metadata in xattr flags
do
	case $metadata in
	xattr) prereq=COW_MAC_XATTR ;;
	flags) prereq=COW_MAC_FLAGS ;;
	esac

	test_expect_success COW_FS,$prereq "macOS $metadata rejects clones without altering cache metadata" '
		repo=metadata-$metadata &&
		setup_repo "$repo" &&
		oid=$(git -C "$repo" rev-parse HEAD:file) &&
		rm "$repo/file" &&
		checkout_with_trace metadata-warm-$metadata.trace \
			git -C "$repo" checkout-index --index file &&
		assert_cloned metadata-warm-$metadata.trace "$oid" &&
		cache=$(cache_path "$repo" "$oid") &&
		case $metadata in
		xattr)
			chmod u+w "$cache" &&
			xattr -w com.git.checkout-cow probe "$cache" &&
			chmod 0444 "$cache"
			;;
		flags)
			test_when_finished "chflags nouchg \"$cache\"" &&
			chflags uchg "$cache"
			;;
		esac &&
		rm "$repo/file" &&
		checkout_with_trace metadata-fallback-$metadata.trace \
			git -C "$repo" checkout-index --index file &&
		assert_no_clones metadata-fallback-$metadata.trace &&
		assert_fixture "$repo" &&
		find "$repo" -name ".git-cow-*" >temps &&
		test_must_be_empty temps &&
		case $metadata in
		xattr)
			xattr -p com.git.checkout-cow "$cache" >actual &&
			echo probe >expect &&
			test_cmp expect actual &&
			chmod u+w "$cache" &&
			xattr -d com.git.checkout-cow "$cache" &&
			chmod 0444 "$cache"
			;;
		flags)
			ls -lO "$cache" >flags &&
			test_grep uchg flags &&
			chflags nouchg "$cache"
			;;
		esac &&
		rm "$repo/file" &&
		checkout_with_trace metadata-recovery-$metadata.trace \
			git -C "$repo" checkout-index --index file &&
		assert_cloned metadata-recovery-$metadata.trace "$oid" &&
		assert_fixture "$repo"
	'
done

test_done
