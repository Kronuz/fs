// ctest for normalize_path against the loop it replaced.
//
// `head_normalize_path` is the pointer-range loop of 8934dbc, frozen: a component at a time, a ".." found
// by scanning back for the last '/'. Its results are the specification (os.path.normpath's normal form,
// checked against Python and a model by test/test.cc). The library's loop must give the same bytes for
// every input in all four flag combinations: every string of '/', '.', 'a' and 'b' up to length 10, every
// string of '/', '.' and 'a' up to length 12, random strings of components (names from one byte to forty,
// dots, runs of slashes) and of bytes (among them the ones next to '/' and '.' in the byte table, NUL and the
// high half), and every length of a path across the size the string overload keeps on the stack. Source
// and destination are exact-size heap blocks, so an AddressSanitizer build fails on a byte read past the
// end of the input or written past `(end - src) + 2` bytes, the room the contract gives.

#include "fs.hh"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

// The pointer-range loop of fs.cc (the header declares only the string_view overload).
size_t normalize_path(const char* src, const char* end, char* dst, bool slashed, bool keep_slash);

static int failures = 0;

// The two loops under test, by one name each, so the code below reads the same in storage and url-parser.
static size_t np_ptr(const char* src, const char* end, char* dst, bool slashed, bool keep_slash) {
	return normalize_path(src, end, dst, slashed, keep_slash);
}
static std::string np_str(std::string_view src, bool slashed, bool keep_slash) {
	return normalize_path(src, slashed, keep_slash);
}

// 8934dbc, never edited.
static size_t head_normalize_path(const char* src, const char* end, char* dst, bool slashed, bool keep_slash) {
	char* const ret = dst;
	const bool absolute = src < end && *src == '/';
	const bool had_slash = src < end && end[-1] == '/';
	if (absolute) {
		*dst++ = '/';
	}
	char* const base = dst;  // the output after the root
	while (src < end) {
		const char* name = src;
		while (src < end && *src != '/') {
			++src;
		}
		const size_t size = static_cast<size_t>(src - name);
		if (src < end) {
			++src;
		}
		if (size == 0 || (size == 1 && name[0] == '.')) {
			continue;
		}
		if (size == 2 && name[0] == '.' && name[1] == '.') {
			char* last = dst;  // the start of the last component of the output
			while (last > base && last[-1] != '/') {
				--last;
			}
			if (dst > base && !(dst - last == 2 && last[0] == '.' && last[1] == '.')) {
				dst = last > base ? last - 1 : base;  // drop it and the '/' before it
				continue;
			}
			if (absolute) {
				continue;
			}
		}
		if (dst > base) {
			*dst++ = '/';
		}
		for (size_t i = 0; i < size; ++i) {
			*dst++ = name[i];
		}
	}
	if (dst == ret) {
		*dst++ = '.';
	}
	if ((keep_slash ? had_slash : slashed) && dst[-1] != '/') {
		*dst++ = '/';
	}
	return static_cast<size_t>(dst - ret);
}

// A heap block of exactly `n` bytes (at least one is allocated, so a zero-size block is a real pointer).
struct Block {
	char* data;
	explicit Block(size_t n) : data(static_cast<char*>(std::malloc(n ? n : 1))) {}
	~Block() { std::free(data); }
	Block(const Block&) = delete;
	Block& operator=(const Block&) = delete;
};

struct Counts {
	size_t compared = 0;
	int reported = 0;
};

// The input in the four flag combinations through both loops, with the input in a block of exactly its size
// and each output in a block of exactly `n + 2`; the empty input is also tried as a null range.
static void compare_one(const char* bytes, size_t n, Counts& c) {
	Block src(n);
	if (n) { std::memcpy(src.data, bytes, n); }
	Block want(n + 2), got(n + 2);
	for (int flags = 0; flags < 4; ++flags) {
		const bool slashed = flags & 1, keep_slash = flags & 2;
		const size_t a = head_normalize_path(src.data, src.data + n, want.data, slashed, keep_slash);
		const size_t b = np_ptr(src.data, src.data + n, got.data, slashed, keep_slash);
		++c.compared;
		if (a != b || std::memcmp(want.data, got.data, a) != 0) {
			if (c.reported++ < 20) {
				std::fprintf(stderr, "FAIL normalize_path(\"%.*s\" (%zu bytes), %d, %d) = \"%.*s\", the loop it replaced gives \"%.*s\"\n",
					static_cast<int>(n), bytes, n, slashed, keep_slash, static_cast<int>(b), got.data, static_cast<int>(a), want.data);
			}
			++failures;
		}
	}
	if (n == 0) {
		for (int flags = 0; flags < 4; ++flags) {
			const bool slashed = flags & 1, keep_slash = flags & 2;
			const size_t a = head_normalize_path(nullptr, nullptr, want.data, slashed, keep_slash);
			const size_t b = np_ptr(nullptr, nullptr, got.data, slashed, keep_slash);
			++c.compared;
			if (a != b || std::memcmp(want.data, got.data, a) != 0) {
				std::fprintf(stderr, "FAIL normalize_path(nullptr, nullptr, %d, %d)\n", slashed, keep_slash);
				++failures;
			}
		}
	}
}

// Every string of `alphabet` (a list of one-byte symbols) from length 0 up to `max_length`.
static void exhaustive(const std::string& alphabet, int max_length, Counts& c) {
	const size_t base = alphabet.size();
	char buffer[32];
	for (int length = 0; length <= max_length; ++length) {
		size_t total = 1;
		for (int i = 0; i < length; ++i) { total *= base; }
		for (size_t k = 0; k < total; ++k) {
			size_t m = k;
			for (int i = 0; i < length; ++i, m /= base) { buffer[i] = alphabet[m % base]; }
			compare_one(buffer, static_cast<size_t>(length), c);
		}
	}
}

struct Random {
	std::uint64_t x = 0x9e3779b97f4a7c15ULL;
	std::uint64_t next() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; }
	size_t below(size_t n) { return static_cast<size_t>((next() >> 11) % n); }
};

// Every exhaustive string above has names of at most ten letters. These are a name of any length (up to forty
// letters), joined by one to three slashes, with "." and ".." and names of dots, a leading and a trailing run
// of slashes, now and then.
static void components(Random& r, int rounds, Counts& c) {
	static const char* const names[] = {
		".", "..", "...", ".a", "a.", "..a", "a..", ".git", "a", "b", "ab", "abc", "abcdefg", "abcdefgh", "abcdefghi",
		"abcdefghijklmnop", "abcdefghijklmnopq", "0123456789abcdefghijklmnopqrstuvwxyzABCD", "\xaf", "\x2e\xaf", "\xff\xff\xff\xff\xff\xff\xff\xff",
	};
	const size_t count = sizeof(names) / sizeof(names[0]);
	std::string s;
	for (int round = 0; round < rounds; ++round) {
		s.clear();
		if (r.below(3) == 0) { s.append(1 + r.below(3), '/'); }
		const size_t parts = 1 + r.below(12);
		for (size_t i = 0; i < parts; ++i) {
			if (i) { s.append(1 + (r.below(8) == 0 ? r.below(3) : 0), '/'); }
			if (r.below(4) == 0) {
				s.append(1 + r.below(40), static_cast<char>("abz.0"[r.below(5)]));  // a name of one repeated byte
			} else {
				s += names[r.below(count)];
			}
		}
		if (r.below(3) == 0) { s.append(1 + r.below(3), '/'); }
		compare_one(s.data(), s.size(), c);
	}
}

// Bytes on either side of '/' (0x2f) and '.' (0x2e) in the table, high bytes, NUL and newline, among them.
static void bytes(Random& r, int rounds, Counts& c) {
	static const unsigned char alphabet[] = {'/', '/', '/', '.', '.', '.', 'a', 'b', 0x00, 0x0a, 0x2d, 0x30, 0x2e, 0x2f, 0xaf, 0xae, 0x80, 0xff};
	std::string s;
	for (int round = 0; round < rounds; ++round) {
		s.clear();
		const size_t n = r.below(100);
		for (size_t i = 0; i < n; ++i) { s.push_back(static_cast<char>(alphabet[r.below(sizeof(alphabet))])); }
		compare_one(s.data(), s.size(), c);
	}
}

// The string_view overload builds its result in a buffer on the stack up to a size and on the heap above it:
// every length across that size, with inputs that give the longest result (a name and a trailing "." or a
// closing '/'), against the loop it replaced.
static void sizes(Counts& c) {
	for (size_t n = 0; n <= 600; ++n) {
		std::string inputs[5];
		inputs[0].assign(n, 'a');                                   // one name
		inputs[1] = std::string(n ? n - 1 : 0, 'a') + (n ? "/" : "");   // one name and its closing '/'
		for (size_t i = 0; i < n; ++i) { inputs[2].push_back(i % 8 == 7 ? '/' : 'a'); }   // names of seven bytes
		for (size_t i = 0; i < n; ++i) { inputs[3].push_back(i % 2 ? '/' : '.'); }   // "./././" and the like
		for (size_t i = 0; i < n; ++i) { inputs[4].push_back("a/b/c/../"[i % 9]); }
		for (const std::string& s : inputs) {
			for (int flags = 0; flags < 4; ++flags) {
				const bool slashed = flags & 1, keep_slash = flags & 2;
				Block want(s.size() + 2);
				const size_t a = head_normalize_path(s.data(), s.data() + s.size(), want.data, slashed, keep_slash);
				const std::string got = np_str(std::string_view(s), slashed, keep_slash);
				++c.compared;
				if (got != std::string_view(want.data, a)) {
					std::fprintf(stderr, "FAIL normalize_path(string_view of %zu bytes, %d, %d) differs from the loop it replaced\n", s.size(), slashed, keep_slash);
					++failures;
				}
			}
		}
	}
	for (int flags = 0; flags < 4; ++flags) {
		const bool slashed = flags & 1, keep_slash = flags & 2;
		if (np_str(std::string_view(), slashed, keep_slash) != np_str(std::string_view(""), slashed, keep_slash)) {
			std::fprintf(stderr, "FAIL normalize_path(std::string_view(), %d, %d) is not the empty path's\n", slashed, keep_slash);
			++failures;
		}
	}
}

int main() {
	Counts c;
	exhaustive("/.ab", 10, c);
	const size_t a = c.compared;
	exhaustive("/.a", 12, c);   // fewer symbols, longer strings: a name of up to twelve letters between the dots
	const size_t b = c.compared - a;
	Random r;
	components(r, 400000, c);
	bytes(r, 400000, c);
	sizes(c);
	std::printf("normalize_path: %zu inputs and flags compared with the loop it replaced (%zu on \"/.ab\" up to 10 bytes, %zu on \"/.a\" up to 12)\n", c.compared, a, b);
	if (c.compared < 10000000) {
		std::fprintf(stderr, "FAIL only %zu comparisons\n", c.compared);
		++failures;
	}
	if (failures) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("all normalize_path tests passed\n");
	return 0;
}
