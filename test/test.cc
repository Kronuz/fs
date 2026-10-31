// ctest for fs: the pure path normalizer plus a real mkdirs / exists / copy /
// delete round-trip on a scratch directory. Built with the default (no-op) logging,
// so this also proves fs links against only its io/strings/split/stringified deps.

#include "fs.hh"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
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
	test_copy_file_faults();

	if (failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	std::puts("all fs tests passed");
	return EXIT_SUCCESS;
}
