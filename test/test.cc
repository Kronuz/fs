// ctest for fs: the pure path normalizer plus a real mkdirs / exists / copy /
// delete round-trip on a scratch directory. Built with the default (no-op) logging,
// so this also proves fs links against only its io/strings/split/stringified deps.

#include "fs.hh"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                          \
		}                                                                        \
	} while (0)


static void test_normalize_path() {
	// Collapses "." and ".." and duplicate slashes.
	CHECK(normalize_path("a/b/../c") == "a/c");
	CHECK(normalize_path("a/./b") == "a/b");
	CHECK(normalize_path("a//b///c") == "a/b/c");
	CHECK(normalize_path("/a/b/../../c") == "/c");
	// slashed=true keeps a trailing slash.
	CHECK(normalize_path("a/b", /*slashed=*/true) == "a/b/");
}


static void test_dir_roundtrip() {
	char tmpl[] = "fs_test.XXXXXX";
	char* d = ::mkdtemp(tmpl);
	CHECK(d != nullptr);
	if (d == nullptr) {
		return;
	}

	std::string base = d;
	std::string nested = base + "/x/y/z";

	// mkdirs creates the whole chain; exists observes it; a second mkdirs is idempotent.
	CHECK(mkdirs(nested));
	CHECK(exists(nested));
	CHECK(exists(base));
	CHECK(mkdirs(nested));  // already there -> still true

	// Drop two files in the tree; glob-delete only the .txt.
	std::string ftxt = nested + "/data.txt";
	std::string fkeep = nested + "/keep.dat";
	for (const auto& f : {ftxt, fkeep}) {
		FILE* fp = std::fopen(f.c_str(), "w");
		CHECK(fp != nullptr);
		if (fp) { std::fputs("hello", fp); std::fclose(fp); }
	}
	CHECK(exists(ftxt) && exists(fkeep));

	delete_files(nested, {"*.txt"});
	CHECK(!exists(ftxt));        // the .txt is gone
	CHECK(exists(fkeep));        // the non-match is kept
	CHECK(exists(nested));       // dir is non-empty (keep.dat) -> retained

	// delete_files with the default "*" removes everything, then rmdir's the emptied dir.
	delete_files(nested);
	CHECK(!exists(fkeep));
	CHECK(!exists(nested));      // emptied -> removed

	// Clean up the rest of the scratch tree.
	::rmdir((base + "/x/y").c_str());
	::rmdir((base + "/x").c_str());
	::rmdir(base.c_str());
	CHECK(!exists(base));
}


static std::string slurp(const std::string& path) {
	std::string out;
	if (FILE* fp = std::fopen(path.c_str(), "r")) {
		char buf[256];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
			out.append(buf, n);
		}
		std::fclose(fp);
	}
	return out;
}

static void spit(const std::string& path, const std::string& data) {
	FILE* fp = std::fopen(path.c_str(), "w");
	CHECK(fp != nullptr);
	if (fp) { std::fwrite(data.data(), 1, data.size(), fp); std::fclose(fp); }
}

// The lowest free descriptor number moves when a descriptor leaks.
static int next_fd() {
	int fd = ::dup(0);
	::close(fd);
	return fd;
}

static void test_copy_file() {
	char tmpl[] = "fs_copy_test.XXXXXX";
	char* d = ::mkdtemp(tmpl);
	CHECK(d != nullptr);
	if (d == nullptr) {
		return;
	}
	std::string base = d;
	std::string src = base + "/src";
	std::string dst = base + "/dst";
	CHECK(mkdirs(src) && mkdirs(dst));

	// A shorter copy over a longer existing file leaves no old tail.
	spit(src + "/f", "short");
	spit(dst + "/f", "a much longer existing destination");
	CHECK(copy_file(src, dst, false, "f") == 0);
	CHECK(slurp(dst + "/f") == "short");

	// A failing destination open reports the error and leaks no descriptor.
	std::string blocked = base + "/blocked";
	CHECK(mkdirs(blocked + "/f"));
	int fd_before = next_fd();
	CHECK(copy_file(src, blocked, false, "f") == -1);
	CHECK(next_fd() == fd_before);

	// A successful copy leaks nothing either.
	CHECK(copy_file(src, dst, false, "f") == 0);
	CHECK(next_fd() == fd_before);

	::unlink((src + "/f").c_str());
	::unlink((dst + "/f").c_str());
	::rmdir((blocked + "/f").c_str());
	::rmdir(blocked.c_str());
	::rmdir(src.c_str());
	::rmdir(dst.c_str());
	::rmdir(base.c_str());
}


int main() {
	test_normalize_path();
	test_dir_roundtrip();
	test_copy_file();

	if (failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	std::puts("all fs tests passed");
	return EXIT_SUCCESS;
}
