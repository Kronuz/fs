// ctest for fs: the pure path normalizer plus a real mkdirs / exists / copy /
// delete round-trip on a scratch directory. Built with the default (no-op) logging,
// so this also proves fs links against only its io/strings/split/stringified deps.

#include "fs.hh"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <fcntl.h>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

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
	test_dir_roundtrip();
	test_copy_file();
	test_copy_file_same_file();
	test_move_files_same_file();
	test_copy_file_faults();

	if (failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	std::puts("all fs tests passed");
	return EXIT_SUCCESS;
}
