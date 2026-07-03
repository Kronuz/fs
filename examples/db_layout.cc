/*
 * db_layout: a runnable, self-checking example that drives `fs` the way
 * Xapiand lays out, switches, quarantines, and removes database shard files.
 *
 * It doubles as a REGRESSION TEST: every filesystem step asserts real state,
 * and main() returns non-zero on any failure, so CMake wires it into ctest.
 */

#include "fs.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <limits.h>
#include <string>
#include <string_view>
#include <unistd.h>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)


static std::string make_scratch_dir(const char* stem) {
	char tmpl[PATH_MAX + 1];
	std::snprintf(tmpl, sizeof(tmpl), "%s.XXXXXX", stem);
	char* d = ::mkdtemp(tmpl);
	if (d == nullptr) {
		std::perror("mkdtemp");
		std::exit(2);
	}
	return std::string(d);
}


static bool write_file(const std::string& path, const char* contents) {
	FILE* fp = std::fopen(path.c_str(), "w");
	if (fp == nullptr) {
		return false;
	}
	bool ok = std::fputs(contents, fp) >= 0;
	ok = std::fclose(fp) == 0 && ok;
	return ok;
}


static bool has_file_matching(std::string_view path, std::string_view pattern, bool prefix_match) {
	DIR* dir = opendir(path);
	CHECK(dir != nullptr);
	if (dir == nullptr) {
		return false;
	}

	File_ptr fptr;
	find_file_dir(dir, fptr, pattern, prefix_match);
	bool found = fptr.ent != nullptr;
	::closedir(dir);
	return found;
}


static std::string find_child_with_prefix(std::string_view path, std::string_view prefix) {
	DIR* dir = opendir(path);
	CHECK(dir != nullptr);
	if (dir == nullptr) {
		return {};
	}

	std::string found;
	struct dirent* ent;
	while ((ent = ::readdir(dir)) != nullptr) {
		std::string_view name(ent->d_name);
		if (name != "." && name != ".." && name.starts_with(prefix)) {
			found.assign(name);
			break;
		}
	}
	::closedir(dir);
	return found;
}


static void remove_empty_dir(const std::string& path) {
	CHECK(::rmdir(path.c_str()) == 0);
}


int main() {
	const std::string scratch = make_scratch_dir("fs_db_layout");

	// Endpoint/working-directory setup: Xapiand normalizes the chosen data dir
	// and keeps the current working directory with a trailing slash.
	const std::string work_dir = normalize_path(scratch + "/cluster//db/../db");
	CHECK(work_dir == scratch + "/cluster/db");
	CHECK(normalize_path(work_dir, /*slashed=*/true) == work_dir + "/");

	const std::string xapiand_dir = work_dir + "/.xapiand";
	CHECK(mkdirs(xapiand_dir));
	CHECK(exists(work_dir));
	CHECK(exists(xapiand_dir));
	CHECK(mkdir(xapiand_dir));  // idempotent, like reopening an existing work dir

	DIR* opened = opendir(xapiand_dir);
	CHECK(opened != nullptr);
	if (opened != nullptr) {
		::closedir(opened);
	}

	// Replication builds the parent chain for a mkdtemp template, then receives
	// files into an isolated switch directory before moving them into place.
	const std::string switch_template = scratch + "/incoming/switch.XXXXXX";
	CHECK(build_path_index(switch_template));
	CHECK(exists(scratch + "/incoming"));

	char switch_buffer[PATH_MAX + 1];
	CHECK(switch_template.size() < sizeof(switch_buffer));
	std::strncpy(switch_buffer, switch_template.c_str(), sizeof(switch_buffer));
	switch_buffer[sizeof(switch_buffer) - 1] = '\0';
	char* switch_raw = ::mkdtemp(switch_buffer);
	CHECK(switch_raw != nullptr);
	if (switch_raw == nullptr) {
		return 1;
	}
	const std::string switch_dir(switch_raw);

	CHECK(write_file(switch_dir + "/iamglass", "glass marker\n"));
	CHECK(write_file(switch_dir + "/postlist.glass", "postlist\n"));
	CHECK(write_file(switch_dir + "/termlist.glass", "termlist\n"));
	CHECK(write_file(switch_dir + "/flintlock", "lock\n"));
	CHECK(write_file(switch_dir + "/wal.0001", "wal\n"));
	CHECK(write_file(switch_dir + "/notes.tmp", "not part of the db cleanup glob\n"));
	CHECK(has_file_matching(switch_dir, "iam", /*prefix_match=*/true));
	CHECK(has_file_matching(switch_dir, "glass", /*prefix_match=*/false));

	const std::string shard_dir = xapiand_dir + "/shards/0";
	CHECK(mkdirs(shard_dir));
	CHECK(write_file(shard_dir + "/old.glass", "old glass file\n"));
	CHECK(write_file(shard_dir + "/wal.0000", "old wal\n"));
	CHECK(write_file(shard_dir + "/flintlock", "old lock\n"));
	CHECK(write_file(shard_dir + "/metadata.keep", "keeps the shard dir alive while old files are removed\n"));

	delete_files(shard_dir, {"wal.*"});
	CHECK(!exists(shard_dir + "/wal.0000"));
	delete_files(shard_dir, {"*glass", "flintlock"});
	CHECK(!exists(shard_dir + "/old.glass"));
	CHECK(!exists(shard_dir + "/flintlock"));
	CHECK(exists(shard_dir + "/metadata.keep"));

	move_files(switch_dir, shard_dir);
	CHECK(!exists(switch_dir));
	CHECK(exists(shard_dir + "/iamglass"));
	CHECK(exists(shard_dir + "/postlist.glass"));
	CHECK(exists(shard_dir + "/termlist.glass"));
	CHECK(exists(shard_dir + "/flintlock"));
	CHECK(exists(shard_dir + "/wal.0001"));
	CHECK(exists(shard_dir + "/notes.tmp"));

	// WAL corruption/replay failures quarantine matching WAL files instead of
	// deleting them outright, leaving other shard files untouched.
	quarantine_files(shard_dir, {"wal.*"});
	CHECK(!exists(shard_dir + "/wal.0001"));
	const std::string quarantine_name = find_child_with_prefix(shard_dir, ".quarantine.");
	CHECK(!quarantine_name.empty());
	const std::string quarantine_dir = shard_dir + "/" + quarantine_name;
	CHECK(exists(quarantine_dir + "/wal.0001"));
	CHECK(exists(shard_dir + "/iamglass"));

	// Stalled shard removal deletes the database file globs and lock, but keeps
	// unrelated files and quarantine state.
	delete_files(shard_dir, {"*glass", "flintlock"});
	CHECK(!exists(shard_dir + "/iamglass"));
	CHECK(!exists(shard_dir + "/postlist.glass"));
	CHECK(!exists(shard_dir + "/termlist.glass"));
	CHECK(!exists(shard_dir + "/flintlock"));
	CHECK(exists(shard_dir + "/metadata.keep"));
	CHECK(exists(shard_dir + "/notes.tmp"));
	CHECK(exists(quarantine_dir + "/wal.0001"));

	delete_files(shard_dir, {"metadata.keep", "notes.tmp"});
	CHECK(!exists(shard_dir + "/metadata.keep"));
	CHECK(!exists(shard_dir + "/notes.tmp"));
	CHECK(exists(shard_dir));  // the quarantine dir still keeps it non-empty

	delete_files(quarantine_dir);
	CHECK(!exists(quarantine_dir));
	delete_files(shard_dir);
	CHECK(!exists(shard_dir));

	remove_empty_dir(xapiand_dir + "/shards");
	remove_empty_dir(xapiand_dir);
	remove_empty_dir(work_dir);
	remove_empty_dir(scratch + "/cluster");
	remove_empty_dir(scratch + "/incoming");
	remove_empty_dir(scratch);

	if (failures == 0) {
		std::puts("db_layout: all checks passed (normalize + mkdirs + mkdtemp + list + delete/quarantine + move)");
	}
	return failures == 0 ? 0 : 1;
}
