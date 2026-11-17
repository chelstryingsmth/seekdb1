/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include "file_source.h"
#include <algorithm>

namespace oceanbase::sql::file_sql {
inline bool glob_match(const std::string &pattern, const std::string &text)
{
  std::size_t p = 0, t = 0, star = std::string::npos, retry = 0;
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) { ++p; ++t; }
    else if (p < pattern.size() && pattern[p] == '*') { star = p++; retry = t; }
    else if (star != std::string::npos) { p = star+1; t = ++retry; }
    else { return false; }
  }
  while (p < pattern.size() && pattern[p] == '*') { ++p; }
  return p == pattern.size();
}

inline std::filesystem::path authorize_glob(const std::filesystem::path &pattern,
                                          const std::filesystem::path &allowed_root)
{
  if (pattern.native().find(typename std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos
      || allowed_root.native().find(typename std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos) {
    throw std::runtime_error("NUL in glob path");
  }
  if (allowed_root.empty()) { throw std::runtime_error("glob requires an allowed directory"); }
  const auto parent = pattern.parent_path().empty() ? std::filesystem::path(".") : pattern.parent_path();
  if (utf8_path_text(parent).find_first_of("*?") != std::string::npos) {
    throw std::runtime_error("wildcards in glob directories are unsupported");
  }
  const auto root = std::filesystem::canonical(allowed_root);
  const auto directory = std::filesystem::canonical(parent);
  auto part = directory.begin();
  for (auto base = root.begin(); base != root.end(); ++base, ++part) {
    if (part == directory.end()) { throw std::runtime_error("glob is outside allowed directory"); }
#ifdef _WIN32
    const bool same = _wcsicmp(base->c_str(), part->c_str()) == 0;
#else
    const bool same = *base == *part;
#endif
    if (!same) { throw std::runtime_error("glob is outside allowed directory"); }
  }
  if (!std::filesystem::is_directory(directory)) { throw std::runtime_error("glob parent is not a directory"); }
  return directory / pattern.filename();
}

inline std::vector<std::string> discover_files(const std::filesystem::path &pattern,
                                             const std::filesystem::path &allowed_root)
{
  const auto safe = authorize_glob(pattern, allowed_root);
  const auto match = utf8_path_text(safe.filename());
  std::vector<std::string> files;
  for (const auto &entry : std::filesystem::directory_iterator(safe.parent_path())) {
    if (entry.is_regular_file() && glob_match(match, utf8_path_text(entry.path().filename()))) {
      // Resolve each match separately: symlinks must not disclose paths outside the root.
      const auto file = authorize_input(entry.path(), allowed_root, true);
      files.push_back(utf8_path_text(file));
      if (files.size() > 100000) { throw std::runtime_error("glob result limit exceeded"); }
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}
} // namespace oceanbase::sql::file_sql
