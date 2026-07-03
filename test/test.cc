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


int main() {
	test_normalize_path();
	test_dir_roundtrip();

	if (failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	std::puts("all fs tests passed");
	return EXIT_SUCCESS;
}
