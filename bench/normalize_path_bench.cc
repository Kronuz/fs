// normalize_path micro-bench: the `np_*` rows of the gate's micro.cc (campaign/gate/raw/fs/micro.cc), alone.
//
// Three binaries are built from this file, one implementation each, so ab_compare.py can run any two of them
// against each other with the same rows:
//   normalize_path_bench        the library's normalize_path (fs.cc)
//   normalize_path_bench_head   the loop of 8934dbc, frozen here (the specification, the loop before the one
//                               in fs.cc), through the same string wrapper
//   normalize_path_bench_start  the loop of ce16118, frozen here, the campaign's START: it gives the same bytes
//                               as the others on the rows of the default mode, and different bytes on the rows
//                               of `diff` (an empty path, a ".." that pops the first component)
//
// Usage: normalize_path_bench [diff|dump]
//   (none)  the rows whose result is the same for all three, one line each: `label  X ns  chk HEX`, ns per
//           call, best of 7 repeats inside one run; the checksum is the FNV-1a of the result and agrees
//           between the implementations (ab_compare.py voids a row whose checksums differ).
//   diff    the rows whose result differs from START's (the fixes), time only, no checksum.
//   dump    every result, one line each (a way to see the bytes).
// The timed loop takes the size and two bytes of every result and nothing more: the gate's micro.cc folded
// every byte of every result into an FNV-1a (a chain of 4 cycles a byte), which was most of the time of a long
// path and hid the difference between loops.
#include "fs.hh"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined(NP_FROZEN_START)
// ce16118, never edited.
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

static std::string normalize(std::string_view src, bool slashed, bool keep_slash) {
	size_t src_size = src.size();
	const char* src_str = src.data();
	std::string dst;
	dst.resize(src_size + 1);
	auto dst_size = frozen_normalize_path(src_str, src_str + src_size, &dst[0], slashed, keep_slash);
	dst.resize(dst_size);
	return dst;
}
#elif defined(NP_FROZEN_HEAD)
// 8934dbc, never edited.
static size_t frozen_normalize_path(const char* src, const char* end, char* dst, bool slashed, bool keep_slash) {
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

static std::string normalize(std::string_view src, bool slashed, bool keep_slash) {
	size_t src_size = src.size();
	const char* src_str = src.data();
	std::string dst;
	dst.resize(src_size + 2);
	auto dst_size = frozen_normalize_path(src_str, src_str + src_size, &dst[0], slashed, keep_slash);
	dst.resize(dst_size);
	return dst;
}
#else
static std::string normalize(std::string_view src, bool slashed, bool keep_slash) {
	return normalize_path(src, slashed, keep_slash);
}
#endif

using Clock = std::chrono::steady_clock;

static uint64_t fold(const std::string& s) {
	uint64_t h = 1469598103934665603ull;
	for (unsigned char c : s) { h = (h ^ c) * 1099511628211ull; }
	return h;
}

struct Shape { const char* label; std::string src; bool slashed, keep; bool differs; };

// One row: `calls` calls of normalize on the shape, ns per call of the best of 7 repeats, and the checksum
// of the result (the first call's, outside the timer) unless the mode is `diff`.
static void row(const Shape& s, size_t calls, bool with_chk) {
	double best = 1e30;
	uint64_t sink = 0;
	for (int repeat = 0; repeat < 7; ++repeat) {
		const auto t0 = Clock::now();
		for (size_t i = 0; i < calls; ++i) {
			const std::string r = normalize(s.src, s.slashed, s.keep);
			sink += r.size() + static_cast<unsigned char>(r[r.size() / 2]) + static_cast<unsigned char>(r.back());
		}
		const auto t1 = Clock::now();
		best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(calls));
	}
	if (with_chk) {
		std::printf("%-26s %9.1f ns  chk %016llx\n", s.label, best, static_cast<unsigned long long>(fold(normalize(s.src, s.slashed, s.keep))));
	} else {
		std::printf("%-26s %9.1f ns\n", s.label, best);
	}
	if (sink == 1) { std::fputc('\n', stderr); }  // keeps `sink` observable
}

int main(int argc, char** argv) {
	const std::string mode = argc > 1 ? argv[1] : "cmp";
	std::string longp, longdots;
	for (int i = 0; i < 40; ++i) { longp += "/component" + std::to_string(i); }
	for (int i = 0; i < 20; ++i) { longdots += "/seg" + std::to_string(i) + "/./x/../y"; }
	const std::vector<Shape> shapes = {
		{"np_plain", "/a/b/c/d", false, false, false},
		{"np_relative", "a/b/c/d", false, false, false},
		{"np_dots", "/a/./b/../c/d/../e", false, false, false},
		{"np_trailing_slash", "/usr/local/lib/", false, false, false},
		{"np_slashed", "/usr/local/lib", true, false, false},
		{"np_repeated_slashes", "//usr///local//lib", false, false, false},
		{"np_keep_slash", "/usr/local/lib/", false, true, false},
		{"np_xapiand_path", "/var/db//xapiand/./data/../data/index/", false, false, false},
		{"np_long", longp, false, false, false},
		{"np_long_dots", longdots, false, false, false},
		{"np_leading_dot", "./a/b/c", false, false, false},
		{"np_leading_dotdot", "../a/b/c", false, false, false},
		{"np_dot", ".", false, false, false},
		{"np_root", "/", false, false, false},
		{"np_abs_dotdot_end", "/a/b/c/..", false, false, false},
		{"np_abs_dotdot_root", "/a/../../b", false, false, false},
		{"np_empty", "", false, false, true},
		{"np_relative_pop", "a/../b/c", false, false, true},
		{"np_relative_pop_all", "a/b/../..", false, false, true},
	};
	if (mode == "dump") {
		for (const Shape& s : shapes) {
			std::printf("%-22s '%s' [%d%d] -> '%s'\n", s.label, s.src.size() > 60 ? "<long>" : s.src.c_str(), s.slashed, s.keep, normalize(s.src, s.slashed, s.keep).c_str());
		}
		return 0;
	}
	if (mode != "cmp" && mode != "diff") {
		std::fprintf(stderr, "usage: %s [diff|dump]\n", argv[0]);
		return 2;
	}
	const bool diff = mode == "diff";
	for (const Shape& s : shapes) {
		if (s.differs == diff) {
			row(s, s.src.size() > 100 ? 200000 : 2000000, !diff);
		}
	}
	return 0;
}
