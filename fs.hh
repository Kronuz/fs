/*
 * Copyright © 2026 Germán Méndez Bravo (Kronuz)
 * Copyright (c) 2015-2019 Dubalu LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

#include <dirent.h>              // for DIR, readdir, opendir, closedir
#include <string>                // for std::string
#include <string_view>           // for std::string_view
#include <vector>                // for std::vector


struct File_ptr {
	struct dirent *ent;

	File_ptr()
		: ent(nullptr) { }
};


void delete_files(std::string_view path, const std::vector<std::string>& patterns = {"*"});

void quarantine_files(std::string_view path, const std::vector<std::string>& patterns = {"*"}, std::string_view template_suffix = ".quarantine.XXXXXX");

void move_files(std::string_view src, std::string_view dst);

bool exists(std::string_view path);

bool mkdir(std::string_view path);

bool mkdirs(std::string_view path);   // false for an empty path

bool build_path_index(std::string_view path_index);

DIR* opendir(std::string_view path, bool create = false);

void find_file_dir(DIR* dir, File_ptr& fptr, std::string_view pattern, bool pre_suf_fix);

// Copy the regular files of directory src into directory dst (subdirectories are not entered):
// all of them, or only file_name (stored as new_name when given). Returns 0 only when every file
// was written in full and closed cleanly; -1 at the first failure, including a short write (a
// partial file is left) and a source and destination that are one file (same device and inode:
// the same path, a hard link, a symlink, a case-insensitive spelling). That file is not touched.
int copy_file(std::string_view src, std::string_view dst, bool create = true, std::string_view file_name = "", std::string_view new_name = "");

// The lexical normal form of os.path.normpath (no filesystem access): drops "." and empty components,
// resolves "..", keeps a leading ".." of a relative path, and spells the current directory "." however it
// is written (also ""). Drops a trailing "/", adds one with `slashed`, keeps the input's with `keep_slash`.
std::string normalize_path(std::string_view src, bool slashed = false, bool keep_slash = false);
