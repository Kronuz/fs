// ctest for fs: the pure path normalizer plus a real mkdirs / exists / copy /
// delete round-trip on a scratch directory. Built with the default (no-op) logging,
// so this also proves fs links against only its io/strings/split/stringified deps.

#include "fs.hh"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "io.hh"

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


// The pointer-range normalize_path before it stopped forming end - 1, frozen: the oracle for every
// non-empty input (the new loop must give the same bytes), never edited.
static size_t frozen_normalize_path(const char* src, const char* end, char* dst, bool slashed, bool keep_slash) {
	int levels = 0;
	char* ret = dst;
	char ch = '\0';
	const char* last = keep_slash ? end - 1 : end;
	while (src <= last) {
		ch = src == end ? '/' : *src;
		++src;
		if (ch == '.' && (levels != 0 || dst == ret || *(dst - 1) == '/' )) {
			*dst++ = ch;
			++levels;
		} else if (ch == '/') {
			while (levels != 0 && dst > ret + 1) {
				if (*--dst == '/') {
					--levels;
				}
			}
			if (dst == ret || *(dst - 1) != '/') {
				*dst++ = ch;
			}
		} else {
			*dst++ = ch;
			levels = 0;
		}
	}
	if (ch == '.' && levels == 1) {
		ch = *--dst;
	}
	if (dst > ret + 1 && !keep_slash) {
		if (slashed) {
			if (ch != '/') {
				*dst++ = '/';
			}
		} else {
			if (ch == '/') {
				--dst;
			}
		}
	}
	return dst - ret;
}

static std::string frozen_normalize(std::string_view src, bool slashed, bool keep_slash) {
	std::string dst;
	dst.resize(src.size() + 1);
	dst.resize(frozen_normalize_path(src.data(), src.data() + src.size(), &dst[0], slashed, keep_slash));
	return dst;
}

// Runs f in a child process: its exit status, or -1 when a signal killed it (a crash).
template <typename F>
static int in_child(F f) {
	std::fflush(nullptr);
	pid_t pid = ::fork();
	if (pid == 0) {
		::_exit(f());
	}
	int status = 0;
	if (pid < 0 || ::waitpid(pid, &status, 0) != pid) {
		return -2;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// The class of paths whose result changed: a relative path where a ".." (or a longer run of dots, which pops
// one component fewer than it has dots) pops every component the output holds. The frozen loop never popped
// the first component, so "a/.." was "a" and "a/../b" was "a/b"; the output is now empty at that point
// ("." if nothing follows). Components are split at '/'; each one followed by '/' is resolved, and so is the
// last one unless keep_slash leaves it as it is. A leading run of dots stays as the "." the output starts
// with and is never popped.
static bool is_dots(std::string_view c) { return !c.empty() && c.find_first_not_of('.') == std::string_view::npos; }

static std::vector<std::string_view> split_components(std::string_view s) {
	std::vector<std::string_view> out;
	std::size_t at = 0;
	for (;;) {
		std::size_t slash = s.find('/', at);
		if (slash == std::string_view::npos) { out.push_back(s.substr(at)); return out; }
		out.push_back(s.substr(at, slash - at));
		at = slash + 1;
	}
}

static bool pops_first_component(std::string_view s, bool keep_slash) {
	if (s.empty() || s[0] == '/') { return false; }
	const auto comps = split_components(s);
	const std::size_t resolved = comps.size() - (keep_slash ? 1 : 0);
	std::size_t depth = 0;
	bool base = false, out_empty = true, hit = false;
	for (std::size_t i = 0; i < resolved; ++i) {
		const std::string_view c = comps[i];
		if (c.empty()) { continue; }
		if (!is_dots(c)) { ++depth; out_empty = false; continue; }
		if (out_empty) { base = true; out_empty = false; continue; }
		const std::size_t pops = c.size() - 1;
		if (pops == 0 || depth == 0) { continue; }
		if (pops >= depth && !base) { hit = true; depth = 0; out_empty = true; }
		else { depth = pops >= depth ? 0 : depth - pops; }
	}
	return hit;
}

// The oracle for the changed class where std::filesystem::path::lexically_normal and the library agree:
// components that are names or exactly "..", a result with no leading "..", and with keep_slash a last
// component that is not an unterminated "..". lexically_normal gives "." for what is left of "a/..", and
// the library's own trailing-slash policy is applied to its answer.
static bool oracle_applies(std::string_view s, bool keep_slash) {
	const auto comps = split_components(s);
	for (auto c : comps) {
		if (is_dots(c) && c != "..") { return false; }
	}
	if (keep_slash && is_dots(comps.back())) { return false; }
	const std::string ln = std::filesystem::path(std::string(s)).lexically_normal().generic_string();
	return ln != ".." && ln.compare(0, 3, "../") != 0;
}

static std::string oracle_normalize(std::string_view s, bool slashed, bool keep_slash) {
	std::string ln = std::filesystem::path(std::string(s)).lexically_normal().generic_string();
	while (ln.size() > 1 && ln.back() == '/') { ln.pop_back(); }
	return (keep_slash ? s.back() == '/' : slashed) ? ln + "/" : ln;
}

struct NormalizeCounts {
	std::size_t compared = 0, changed = 0, applicable = 0, old_wrong = 0;
	int reported = 0;
};

// One input, one flag combination: equal to the frozen loop outside the changed class; inside it, equal to
// the oracle where the oracle applies (and the frozen loop was not); a relative path stays relative.
static void compare_normalize(const std::string& s, bool slashed, bool keep_slash, NormalizeCounts& n) {
	const std::string now = normalize_path(s, slashed, keep_slash);
	const std::string old = frozen_normalize(s, slashed, keep_slash);
	const bool changed = pops_first_component(s, keep_slash);
	auto fail = [&](const char* what, const std::string& want) {
		if (n.reported++ < 20) {
			std::fprintf(stderr, "FAIL normalize_path(\"%s\", %d, %d) = \"%s\": %s \"%s\"\n", s.c_str(), slashed, keep_slash, now.c_str(), what, want.c_str());
		}
		++failures;
	};
	if (!changed) {
		if (now != old) { fail("frozen gives", old); }
	} else {
		++n.changed;
		if (oracle_applies(s, keep_slash)) {
			const std::string want = oracle_normalize(s, slashed, keep_slash);
			++n.applicable;
			if (now != want) { fail("oracle gives", want); }
			if (old != want) { ++n.old_wrong; }
		}
	}
	if (s[0] != '/' && now.substr(0, 1) == "/") { fail("made a relative path absolute, from", s); }
	++n.compared;
}

// normalize_path over every string of "a", "." and "/" up to length 8 and 30,000 random ones up to 24, in
// all four flag combinations, equals the frozen implementation except in two classes: the empty path
// without keep_slash (the frozen loop appended its closing '/' to nothing and returned the root;
// test_normalize_path_empty states it) and the paths where a ".." pops the first component (above).
// Beside the comparison, the result never starts with '/' unless the input does: relative stays relative.
static void test_normalize_path_frozen() {
	const char alphabet[] = {'a', '.', '/'};
	NormalizeCounts n;
	for (int length = 1; length <= 8; ++length) {
		std::size_t total = 1;
		for (int i = 0; i < length; ++i) { total *= 3; }
		for (std::size_t k = 0; k < total; ++k) {
			std::string s;
			for (std::size_t m = k, i = 0; i < static_cast<std::size_t>(length); ++i, m /= 3) { s.push_back(alphabet[m % 3]); }
			for (int flags = 0; flags < 4; ++flags) { compare_normalize(s, flags & 1, flags & 2, n); }
		}
	}
	std::uint64_t x = 0x9e3779b97f4a7c15ULL;
	for (int round = 0; round < 30000; ++round) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		std::string s;
		for (std::uint64_t m = x, i = 0; i < 1 + x % 24; ++i, m = m * 6364136223846793005ULL + 1442695040888963407ULL) { s.push_back(alphabet[(m >> 33) % 3]); }
		for (int flags = 0; flags < 4; ++flags) { compare_normalize(s, flags & 1, flags & 2, n); }
	}
	std::printf("normalize_path: %zu compared, %zu in the changed class, %zu with an oracle, the frozen loop wrong on %zu\n", n.compared, n.changed, n.applicable, n.old_wrong);
	CHECK(n.compared > 100000);
	// The class is not empty, the oracle reaches into it, and the frozen loop was wrong on some of what it reaches.
	CHECK(n.changed > 0);
	CHECK(n.applicable > 0);
	CHECK(n.old_wrong > 0);
}

// The first component of a relative path is popped by a ".." like any other: "a/.." is ".", "a/../b" is "b".
static void test_normalize_path_first_component() {
	CHECK(normalize_path("a/..") == ".");
	CHECK(normalize_path("a/..", true) == "./");
	CHECK(normalize_path("a/../") == ".");
	CHECK(normalize_path("a/../", false, true) == "./");
	CHECK(normalize_path("a/../b") == "b");
	CHECK(normalize_path("a/../b/") == "b");
	CHECK(normalize_path("a/../b/", true) == "b/");
	CHECK(normalize_path("a/../b/", false, true) == "b/");
	CHECK(normalize_path("a/b/../..") == ".");
	CHECK(normalize_path("a/b/../../c") == "c");
	CHECK(normalize_path("a/../b/..") == ".");
	CHECK(normalize_path(".a/..") == ".");
	CHECK(normalize_path("a./..") == ".");
	CHECK(normalize_path("abc/../x/y") == "x/y");
	// What is left alone: a deeper component, the root, a leading "." or "..", and a keep_slash tail.
	CHECK(normalize_path("a/b/..") == "a");
	CHECK(normalize_path("/a/..") == "/");
	CHECK(normalize_path("/a/../b") == "/b");
	CHECK(normalize_path("../a/..") == ".");
	CHECK(normalize_path("../a") == "./a");
	CHECK(normalize_path("./a/..") == ".");
	CHECK(normalize_path("a/..", false, true) == "a/..");
}

// The empty path names nothing, so it normalizes to nothing in all four flag combinations (the same as
// mkdirs(""), which is false). Oracle: std::filesystem::path::lexically_normal, which maps "" to "";
// os.path.normpath maps it to ".", which no caller wants (storage appends file names to the result
// of normalize_path(base, true), so "." would need a second spelling and "/" would write into the root).
// The frozen loop returned "/" for the two combinations without keep_slash: that class, and only
// that class, differs, so old != new there and new == oracle; with keep_slash old == new == "".
static void test_normalize_path_empty() {
	const std::string oracle = std::filesystem::path("").lexically_normal().string();
	CHECK(oracle.empty());
	for (int flags = 0; flags < 4; ++flags) {
		bool slashed = flags & 1, keep_slash = flags & 2;
		CHECK(normalize_path("", slashed, keep_slash) == oracle);
		CHECK(normalize_path(std::string_view(), slashed, keep_slash) == oracle);
		if (!keep_slash) {
			CHECK(frozen_normalize("", slashed, keep_slash) == "/");
			CHECK(frozen_normalize("", slashed, keep_slash) != normalize_path("", slashed, keep_slash));
		}
	}
	// What the empty path leaves behind is not the root: a name appended to it stays relative.
	CHECK(normalize_path("", true) + "vol.0" == "vol.0");
	// A path that is only separators or dots is not empty: it keeps its meaning.
	CHECK(normalize_path("/") == "/");
	CHECK(normalize_path(".") == ".");
	CHECK(normalize_path("./", true) == "./");
}

// A default-constructed std::string_view (null data pointer) is an empty path like "": the loop forms
// no pointer before the range, so it neither crashes nor reads outside the buffer.
static void test_normalize_path_null_view() {
	for (int flags = 0; flags < 4; ++flags) {
		bool slashed = flags & 1, keep_slash = flags & 2;
		int status = in_child([&] { return normalize_path(std::string_view(), slashed, keep_slash) == normalize_path("", slashed, keep_slash) ? 0 : 1; });
		if (status != 0) {
			std::fprintf(stderr, "FAIL normalize_path(std::string_view(), %d, %d): %s\n", slashed, keep_slash, status == -1 ? "crashed" : "differs from \"\"");
			++failures;
		}
	}
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


// An empty path names no directory: mkdir -p "" fails with ENOENT, so mkdirs and build_path_index
// return false and create nothing. mkdirs used to read front() of the empty view (undefined
// behavior: an abort under _GLIBCXX_ASSERTIONS, a crash for a default-constructed view, true
// otherwise). The parent of a file in the root, "/name", is "/", which exists.
static void test_empty_and_root_paths() {
	CHECK(!mkdirs(""));
	CHECK(!mkdirs(std::string_view()));
	CHECK(!build_path_index(""));
	CHECK(!build_path_index(std::string_view()));

	CHECK(mkdirs("/"));
	CHECK(build_path_index("/fs_test_no_such_file"));
	CHECK(!exists("/fs_test_no_such_file"));   // only the parent is ensured

	char tmpl[] = "fs_index_test.XXXXXX";
	char* tmp = ::mkdtemp(tmpl);
	CHECK(tmp != nullptr);
	if (tmp == nullptr) {
		return;
	}
	std::string base = tmp;
	CHECK(build_path_index(base + "/a/b/file"));   // creates a/b, not file
	CHECK(exists(base + "/a/b") && !exists(base + "/a/b/file"));
	::rmdir((base + "/a/b").c_str());
	::rmdir((base + "/a").c_str());
	::rmdir(base.c_str());
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


// A default-constructed std::string_view into every function that hands a path to stringified or the
// kernel: an empty path names nothing, so each returns its failure value and creates or removes nothing,
// and none crashes (stringified once read the byte after a view, a null dereference for this one).
static void test_null_view_paths() {
	char tmpl[] = "fs_null_view.XXXXXX";
	char* tmp = ::mkdtemp(tmpl);
	CHECK(tmp != nullptr);
	if (tmp == nullptr) {
		return;
	}
	std::string base = tmp;
	spit(base + "/f", "data");
	std::string_view n;

	struct Case { const char* what; int status; };
	Case cases[] = {
		{"exists", in_child([&] { return exists(n) ? 1 : 0; })},
		{"mkdir", in_child([&] { return mkdir(n) ? 1 : 0; })},
		{"mkdirs", in_child([&] { return mkdirs(n) ? 1 : 0; })},
		{"build_path_index", in_child([&] { return build_path_index(n) ? 1 : 0; })},
		{"opendir", in_child([&] { return opendir(n, false) != nullptr ? 1 : 0; })},
		{"opendir, create", in_child([&] { return opendir(n, true) != nullptr ? 1 : 0; })},
		{"delete_files", in_child([&] { delete_files(n); return 0; })},
		{"move_files, null source", in_child([&] { move_files(n, base); return 0; })},
		{"copy_file, null source", in_child([&] { return copy_file(n, base) == -1 ? 0 : 1; })},
		{"copy_file, null destination", in_child([&] { return copy_file(base, n, false) == -1 ? 0 : 1; })},
		{"copy_file, null destination, create", in_child([&] { return copy_file(base, n, true) == -1 ? 0 : 1; })},
		{"find_file_dir, null prefix", in_child([&] {
			DIR* dir = opendir(base, false);
			File_ptr p;
			find_file_dir(dir, p, n, true);
			int found = p.ent != nullptr && std::string_view(p.ent->d_name) == "f" ? 0 : 1;
			::closedir(dir);
			return found;
		})},
		{"find_file_dir, null suffix", in_child([&] {
			DIR* dir = opendir(base, false);
			File_ptr p;
			find_file_dir(dir, p, n, false);
			int found = p.ent != nullptr && std::string_view(p.ent->d_name) == "f" ? 0 : 1;
			::closedir(dir);
			return found;
		})},
	};
	for (const auto& c : cases) {
		if (c.status != 0) {
			std::fprintf(stderr, "FAIL null string_view into %s: %s\n", c.what, c.status == -1 ? "crashed" : "wrong result");
			++failures;
		}
	}
	CHECK(slurp(base + "/f") == "data");   // nothing was moved or deleted

	::unlink((base + "/f").c_str());
	::rmdir(base.c_str());
}

// A directory copy, or a named file copy, whose source and destination are one file must be
// refused (-1) and leave the source as it was. Oracle: cp(1) ("a and b are identical (not
// copied)"): a copy never destroys its source. Before the fix the open of the destination
// truncated the source to 0 bytes and the call returned 0.
static std::string upper(std::string s) {
	for (auto& c : s) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
	return s;
}

static std::string working_directory() {
	char buf[4096];
	return ::getcwd(buf, sizeof(buf)) != nullptr ? std::string(buf) : std::string();
}

static void test_copy_file_same_file() {
	char tmpl[] = "fs_copy_same.XXXXXX";
	char* tmp = ::mkdtemp(tmpl);
	CHECK(tmp != nullptr);
	if (tmp == nullptr) {
		return;
	}
	std::string base = tmp;
	std::string d = base + "/d";
	std::string e = base + "/e";
	CHECK(mkdirs(d) && mkdirs(e));

	std::string payload(5000, 'p');   // longer than one 4096-byte copy chunk
	for (std::size_t i = 0; i < payload.size(); ++i) { payload[i] = static_cast<char>('a' + i % 26); }
	const std::string other = "another file in the directory";

	// Each case starts from the same two files in d; the source is rewritten in place first so
	// that a case that damaged it cannot hide behind the next.
	auto refused = [&](const char* what, const std::string& src, const std::string& dst, std::string_view file, std::string_view as) {
		spit(d + "/f", payload);
		spit(d + "/other", other);
		int fd_before = next_fd();
		int rc = copy_file(src, dst, false, file, as);
		std::string f_after = slurp(d + "/f");
		std::string other_after = slurp(d + "/other");
		bool leaked = next_fd() != fd_before;
		if (rc != -1 || f_after != payload || other_after != other || leaked) {
			std::fprintf(stderr, "FAIL same file (%s): rc=%d, d/f %zu -> %zu bytes, d/other %zu -> %zu bytes%s\n", what, rc,
				payload.size(), f_after.size(), other.size(), other_after.size(), leaked ? ", leaked a descriptor" : "");
			++failures;
		}
	};

	// The same path.
	refused("directory onto itself", d, d, "", "");
	refused("one file onto itself", d, d, "f", "f");
	refused("one file onto itself, no create", d, d, "f", "");

	// Other spellings of the same path.
	refused("./path", d, "./" + d, "f", "");
	refused("path/../path", d, d + "/../d", "f", "");
	refused("doubled and trailing slashes", d, d + "//", "f", "");
	refused("absolute path", d, working_directory() + "/" + d, "f", "");
	refused("absolute path, directory copy", d, working_directory() + "/" + d, "", "");
	refused("both relative spellings", "./" + d, d + "/./", "f", "f");

	// A symlink to the directory, and to the file (one hop and a chain).
	CHECK(::symlink("d", (base + "/link_d").c_str()) == 0);
	refused("symlink to the directory", d, base + "/link_d", "f", "");
	refused("symlink to the directory, directory copy", base + "/link_d", d, "", "");
	CHECK(::symlink((working_directory() + "/" + d + "/f").c_str(), (e + "/f").c_str()) == 0);
	refused("symlink to the file", d, e, "f", "");
	refused("symlink to the file, directory copy", d, e, "", "");
	::unlink((e + "/f").c_str());
	CHECK(::symlink((working_directory() + "/" + d + "/f").c_str(), (e + "/g").c_str()) == 0);
	CHECK(::symlink("g", (e + "/f").c_str()) == 0);
	refused("chain of symlinks to the file", d, e, "f", "");
	::unlink((e + "/f").c_str());
	::unlink((e + "/g").c_str());

	// A hard link to the file, in another directory and in the same one.
	spit(d + "/f", payload);
	CHECK(::link((d + "/f").c_str(), (e + "/f").c_str()) == 0);
	refused("hard link", d, e, "f", "");
	refused("hard link, directory copy", d, e, "", "");
	CHECK(slurp(e + "/f") == payload);   // the link still reads the source's bytes
	::unlink((e + "/f").c_str());
	CHECK(::link((d + "/f").c_str(), (d + "/g").c_str()) == 0);
	refused("hard link in the same directory", d, d, "f", "g");
	::unlink((d + "/g").c_str());

	// The same file reached through a case-insensitive name (the default on APFS and NTFS).
	struct stat lower_st, upper_st;
	bool folds_case = ::stat(d.c_str(), &lower_st) == 0 && ::stat(upper(d).c_str(), &upper_st) == 0 &&
		lower_st.st_dev == upper_st.st_dev && lower_st.st_ino == upper_st.st_ino;
	if (folds_case) {
		refused("directory spelled in another case", d, upper(d), "f", "");
		refused("file name spelled in another case", d, d, "f", "F");
		refused("directory and file name spelled in another case", d, upper(d), "f", "F");
	} else {
		std::puts("fs: case-insensitive cases skipped (case-sensitive filesystem)");
	}

#if defined(__APPLE__)
	// The firmlink spelling of the same directory on macOS (the nearest thing to a bind mount
	// one can make without privileges): a different path to one inode.
	std::string firmlink = "/System/Volumes/Data" + working_directory() + "/" + d;
	if (::access(firmlink.c_str(), F_OK) == 0) {
		refused("firmlink spelling", d, firmlink, "f", "");
	}
#elif defined(__linux__)
	// The same directory through the process's working directory link.
	std::string via_proc = "/proc/self/cwd/" + d;
	if (::access(via_proc.c_str(), F_OK) == 0) {
		refused("/proc/self/cwd spelling", d, via_proc, "f", "");
	}
#endif

	// Controls: a different file in the same directory is an ordinary copy, and so is the
	// same name in another directory.
	spit(d + "/f", payload);
	::unlink((d + "/g").c_str());
	CHECK(copy_file(d, d, false, "f", "g") == 0);
	CHECK(slurp(d + "/g") == payload);
	CHECK(slurp(d + "/f") == payload);
	::unlink((e + "/f").c_str());
	CHECK(copy_file(d, e, false, "f") == 0);
	CHECK(slurp(e + "/f") == payload);
	CHECK(slurp(d + "/f") == payload);

	for (const char* name : {"f", "g", "other"}) {
		::unlink((d + "/" + name).c_str());
		::unlink((e + "/" + name).c_str());
	}
	::unlink((base + "/link_d").c_str());
	::rmdir(d.c_str());
	::rmdir(e.c_str());
	::rmdir(base.c_str());
}


// move_files renames, and rename(2) onto the same file is a no-op: moving a directory onto
// itself, or onto a directory holding a hard link to one of its files, never loses a byte.
static void test_move_files_same_file() {
	char tmpl[] = "fs_move_same.XXXXXX";
	char* tmp = ::mkdtemp(tmpl);
	CHECK(tmp != nullptr);
	if (tmp == nullptr) {
		return;
	}
	std::string base = tmp;
	std::string d = base + "/d";
	std::string e = base + "/e";
	CHECK(mkdirs(d) && mkdirs(e));
	const std::string payload(5000, 'm');

	spit(d + "/f", payload);
	move_files(d, d);
	CHECK(slurp(d + "/f") == payload);

	CHECK(::link((d + "/f").c_str(), (e + "/f").c_str()) == 0);
	move_files(d, e);
	CHECK(slurp(e + "/f") == payload);

	::unlink((d + "/f").c_str());
	::unlink((e + "/f").c_str());
	::rmdir(d.c_str());
	::rmdir(e.c_str());
	::rmdir(base.c_str());
}


// copy_file between directories on different devices. Oracle: cp(1). A plain copy works in both
// directions, over an existing file and as a directory copy, and a symlink on the other device
// that points at the source is still the source. The directories come from where a host usually
// keeps separate filesystems (the working directory, /tmp, /var/tmp, /dev/shm, $TMPDIR); with
// fewer than two devices the test says so and skips. Two files on different devices may carry
// the same inode number: the copy must not take that for one file (identity_test.cc builds that
// case by hand, and here it happens on real files whenever the host's filesystems collide).
static void test_copy_file_across_filesystems() {
	struct Scratch { std::string dir; dev_t dev; };
	std::vector<Scratch> scratch;
	std::vector<std::string> candidates{".", "/tmp", "/var/tmp", "/dev/shm"};
	if (const char* tmpdir = std::getenv("TMPDIR")) {
		candidates.push_back(tmpdir);
	}
	for (const auto& candidate : candidates) {
		std::string tmpl = candidate + "/fs_cross.XXXXXX";
		char* made = ::mkdtemp(tmpl.data());
		if (made == nullptr) {
			continue;
		}
		struct stat st;
		bool fresh = ::stat(made, &st) == 0 &&
			std::none_of(scratch.begin(), scratch.end(), [&](const Scratch& s) { return s.dev == st.st_dev; });
		if (fresh) {
			scratch.push_back({made, st.st_dev});
		} else {
			::rmdir(made);
		}
	}

	std::string payload(5000, 'x');
	for (std::size_t i = 0; i < payload.size(); ++i) { payload[i] = static_cast<char>('a' + i % 26); }
	const std::string other = "an existing destination that is longer than a short source";
	const std::string big(9000, 'h');

	if (scratch.size() < 2) {
		std::puts("fs: cross-filesystem copy skipped (one filesystem)");
	}
	for (const auto& from : scratch) {
		for (const auto& to : scratch) {
			if (&from == &to) {
				continue;
			}
			std::string x = from.dir + "/cd";
			std::string y = to.dir + "/cd";
			CHECK(mkdirs(x) && mkdirs(y));

			struct stat sx, sy;
			spit(x + "/f", payload);
			spit(y + "/f", other);
			CHECK(::stat((x + "/f").c_str(), &sx) == 0 && ::stat((y + "/f").c_str(), &sy) == 0);
			if (sx.st_ino == sy.st_ino) {
				std::printf("fs: %s and %s hand out the same inode number %llu on different devices\n",
					x.c_str(), y.c_str(), static_cast<unsigned long long>(sx.st_ino));
			}

			int fd_before = next_fd();
			int rc = copy_file(x, y, false, "f");
			if (rc != 0 || slurp(y + "/f") != payload || slurp(x + "/f") != payload || next_fd() != fd_before) {
				std::fprintf(stderr, "FAIL copy over an existing file %s -> %s: rc=%d\n", x.c_str(), y.c_str(), rc);
				++failures;
			}

			// The other way round.
			spit(y + "/f", other);
			spit(x + "/f", payload);
			rc = copy_file(y, x, false, "f");
			if (rc != 0 || slurp(x + "/f") != other || slurp(y + "/f") != other) {
				std::fprintf(stderr, "FAIL copy back %s -> %s: rc=%d\n", y.c_str(), x.c_str(), rc);
				++failures;
			}

			// A symlink on the other device to the source: one file, so refused, source intact.
			spit(x + "/f", payload);
			::unlink((y + "/f").c_str());
			std::string absolute = x;
			if (absolute[0] != '/') { absolute = working_directory() + "/" + x; }
			CHECK(::symlink((absolute + "/f").c_str(), (y + "/f").c_str()) == 0);
			rc = copy_file(x, y, false, "f");
			if (rc != -1 || slurp(x + "/f") != payload) {
				std::fprintf(stderr, "FAIL symlink on another device to the source %s: rc=%d, %zu bytes left\n",
					y.c_str(), rc, slurp(x + "/f").size());
				++failures;
				spit(x + "/f", payload);
			}
			::unlink((y + "/f").c_str());

			// A directory copy of several files.
			spit(x + "/g", "g");
			spit(x + "/h", big);
			rc = copy_file(x, y, false, "", "");
			if (rc != 0 || slurp(y + "/f") != payload || slurp(y + "/g") != "g" || slurp(y + "/h") != big) {
				std::fprintf(stderr, "FAIL directory copy %s -> %s: rc=%d\n", x.c_str(), y.c_str(), rc);
				++failures;
			}

			for (const char* name : {"f", "g", "h"}) {
				::unlink((x + "/" + name).c_str());
				::unlink((y + "/" + name).c_str());
			}
			::rmdir(x.c_str());
			::rmdir(y.c_str());
		}
	}
	for (const auto& s : scratch) {
		::rmdir(s.dir.c_str());
	}
}


// copy_file under injected write failures. Oracle: a copy that did not write every byte of
// the source is a failure (-1); one that wrote them all is 0 and byte-identical.
static void test_copy_file_faults() {
	char tmpl[] = "fs_copy_fault.XXXXXX";
	char* d = ::mkdtemp(tmpl);
	CHECK(d != nullptr);
	if (d == nullptr) {
		return;
	}
	std::string base = d;
	std::string src = base + "/src";
	std::string dst = base + "/dst";
	CHECK(mkdirs(src) && mkdirs(dst));
	std::string payload(6100, 'x');   // one full 4096-byte chunk and a 2004-byte tail
	for (std::size_t i = 0; i < payload.size(); ++i) { payload[i] = static_cast<char>('a' + i % 26); }
	spit(src + "/f", payload);
	int fd_before = next_fd();

	// RLIMIT_FSIZE: the second chunk is written only up to the limit (a short write, then
	// EFBIG), and the source ends there, so nothing after it would reveal the loss.
	{
		struct rlimit old{};
		CHECK(::getrlimit(RLIMIT_FSIZE, &old) == 0);
		auto old_handler = ::signal(SIGXFSZ, SIG_IGN);
		struct rlimit lim = old;
		lim.rlim_cur = 6000;
		CHECK(::setrlimit(RLIMIT_FSIZE, &lim) == 0);
		int rc = copy_file(src, dst, false, "f");
		::setrlimit(RLIMIT_FSIZE, &old);
		::signal(SIGXFSZ, old_handler);
		CHECK(rc == -1);
		CHECK(next_fd() == fd_before);   // the error path leaks nothing
		::unlink((dst + "/f").c_str());
	}

	// A destination that refuses every write (ENOSPC), where the platform has one.
	if (::access("/dev/full", W_OK) == 0) {
		CHECK(::symlink("/dev/full", (dst + "/f").c_str()) == 0);
		CHECK(copy_file(src, dst, false, "f") == -1);
		CHECK(next_fd() == fd_before);
		::unlink((dst + "/f").c_str());
	}

	// EINTR with io's own retry off: the copy retries the interrupted write itself. The
	// destination is a FIFO whose reader is slow, so the writer blocks in write() and the
	// timer signal (installed without SA_RESTART) interrupts it.
	{
		std::string big(1 << 20, 'q');
		for (std::size_t i = 0; i < big.size(); ++i) { big[i] = static_cast<char>(i * 131 + i / 7); }
		spit(src + "/f", big);
		CHECK(::mkfifo((dst + "/f").c_str(), 0600) == 0);

		struct sigaction sa{}, old_sa{};
		sa.sa_handler = [](int) {};
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;   // no SA_RESTART
		CHECK(::sigaction(SIGALRM, &sa, &old_sa) == 0);
		struct itimerval tv{{0, 2000}, {0, 2000}}, off{};
		bool was = io::ignore_eintr().exchange(false);

		std::string got;
		std::thread reader([&] {
			int fd = ::open((dst + "/f").c_str(), O_RDONLY);
			char buf[1024];
			::usleep(20000);   // let the writer fill the pipe and block
			while (fd >= 0) {
				ssize_t n = ::read(fd, buf, sizeof(buf));
				if (n > 0) { got.append(buf, static_cast<std::size_t>(n)); ::usleep(100); continue; }
				if (n == -1 && errno == EINTR) { continue; }
				break;
			}
			if (fd >= 0) { ::close(fd); }
		});
		::setitimer(ITIMER_REAL, &tv, nullptr);
		int rc = copy_file(src, dst, false, "f");
		::setitimer(ITIMER_REAL, &off, nullptr);
		reader.join();
		io::ignore_eintr().store(was);
		::sigaction(SIGALRM, &old_sa, nullptr);
		CHECK(rc == 0);
		CHECK(got == big);
		CHECK(next_fd() == fd_before);
		::unlink((dst + "/f").c_str());
	}

	::unlink((src + "/f").c_str());
	::rmdir(src.c_str());
	::rmdir(dst.c_str());
	::rmdir(base.c_str());
}


int main() {
	test_normalize_path();
	test_normalize_path_frozen();
	test_normalize_path_first_component();
	test_normalize_path_null_view();
	test_normalize_path_empty();
	test_dir_roundtrip();
	test_empty_and_root_paths();
	test_copy_file();
	test_null_view_paths();
	test_copy_file_same_file();
	test_move_files_same_file();
	test_copy_file_across_filesystems();
	test_copy_file_faults();

	if (failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	std::puts("all fs tests passed");
	return EXIT_SUCCESS;
}
