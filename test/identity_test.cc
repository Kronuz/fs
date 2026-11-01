// ctest for the file identity copy_file refuses on: two stat results are one file when
// both the device and the inode number match. The test includes fs.cc to reach the
// file-local predicate, so it links the dependencies and not the fs library.
//
// An inode number alone is not an identity: a hard link or a symlink shares a device and
// an inode, but two files on separate filesystems can carry the same inode number (a
// freshly made tmpfs and an APFS volume both hand out low numbers). The real filesystems
// of a build host rarely collide, so the cases here build the stat results by hand.
// Oracle: POSIX, a file is identified by the pair (st_dev, st_ino).

#include "fs.cc"

#include <cstdio>
#include <cstring>
#include <fcntl.h>

static int failures = 0;
#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                          \
		}                                                                        \
	} while (0)

static struct stat made(dev_t dev, ino_t ino) {
	struct stat st;
	std::memset(&st, 0, sizeof(st));
	st.st_dev = dev;
	st.st_ino = ino;
	return st;
}

static void test_hand_made() {
	struct stat a = made(7, 4242);

	CHECK(same_file(a, a));
	CHECK(same_file(a, made(7, 4242)));          // a hard link, a symlink, another spelling

	CHECK(!same_file(a, made(7, 4243)));         // another file on the same device
	CHECK(!same_file(a, made(8, 4242)));         // the same number on another device: not one file
	CHECK(!same_file(made(8, 4242), a));         // and the other way round
	CHECK(!same_file(a, made(8, 4243)));

	CHECK(same_file(made(0, 0), made(0, 0)));
	CHECK(!same_file(made(0, 5), made(1, 5)));
	CHECK(!same_file(made(1, 0), made(1, 5)));
}

// The predicate on real files: one file, a hard link to it, and a different file.
static void test_real_files() {
	char tmpl[] = "fs_identity.XXXXXX";
	char* tmp = ::mkdtemp(tmpl);
	CHECK(tmp != nullptr);
	if (tmp == nullptr) {
		return;
	}
	std::string base = tmp;
	for (const char* name : {"/a", "/b"}) {
		int fd = ::open((base + name).c_str(), O_CREAT | O_WRONLY, 0644);
		CHECK(fd != -1);
		if (fd != -1) { ::close(fd); }
	}
	CHECK(::link((base + "/a").c_str(), (base + "/link").c_str()) == 0);

	struct stat a, link, b;
	CHECK(::stat((base + "/a").c_str(), &a) == 0);
	CHECK(::stat((base + "/link").c_str(), &link) == 0);
	CHECK(::stat((base + "/b").c_str(), &b) == 0);
	CHECK(same_file(a, link));
	CHECK(!same_file(a, b));

	::unlink((base + "/a").c_str());
	::unlink((base + "/b").c_str());
	::unlink((base + "/link").c_str());
	::rmdir(base.c_str());
}

int main() {
	test_hand_made();
	test_real_files();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::puts("all fs identity tests passed");
	return 0;
}
