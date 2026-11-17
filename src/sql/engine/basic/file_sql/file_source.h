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

#include "csv_reader.h"
#include <filesystem>
#include <fstream>
#include <set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace oceanbase::sql::file_sql {

inline std::filesystem::path path_from_utf8(const std::string &text)
{
#if defined(__cpp_lib_char8_t)
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
#else
  return std::filesystem::u8path(text);
#endif
}

// Authorization must happen before opening either the inference or scan stream.
inline std::filesystem::path authorize_input(const std::filesystem::path &path,
                                            const std::filesystem::path &allowed_root,
                                            bool enabled)
{
  if (path.native().find(typename std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos
      || allowed_root.native().find(typename std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos) {
    throw std::runtime_error("NUL in file path");
  }
  if (!enabled) { throw std::runtime_error("file SQL is disabled"); }
  if (allowed_root.empty()) { throw std::runtime_error("file SQL requires an allowed directory"); }
  const auto root = std::filesystem::canonical(allowed_root);
  const auto file = std::filesystem::canonical(path);
  auto file_part = file.begin();
  for (auto root_part = root.begin(); root_part != root.end(); ++root_part, ++file_part) {
    bool equal = file_part != file.end();
    if (equal) {
#ifdef _WIN32
      equal = _wcsicmp(root_part->c_str(), file_part->c_str()) == 0;
#else
      equal = *root_part == *file_part;
#endif
    }
    if (!equal) {
      throw std::runtime_error("file is outside the allowed directory");
    }
  }
  if (!std::filesystem::is_regular_file(file)) {
    throw std::runtime_error("input is not a regular file");
  }
  return file;
}

inline std::string utf8_path_text(const std::filesystem::path &path)
{
  const auto text = path.generic_u8string();
  return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

struct FileSnapshot {
  std::uintmax_t size;
  std::filesystem::file_time_type modified;
  std::uint64_t device = 0, identity = 0;
  std::int64_t changed = 0, changed_fraction = 0;
  static FileSnapshot capture(const std::filesystem::path &path)
  {
    FileSnapshot result{std::filesystem::file_size(path), std::filesystem::last_write_time(path)};
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if (handle == INVALID_HANDLE_VALUE) { throw std::runtime_error("cannot inspect input file identity"); }
    BY_HANDLE_FILE_INFORMATION identity;
    FILE_BASIC_INFO basic;
    const bool valid = GetFileInformationByHandle(handle,&identity)
                    && GetFileInformationByHandleEx(handle,FileBasicInfo,&basic,sizeof(basic));
    CloseHandle(handle);
    if (!valid) { throw std::runtime_error("cannot inspect input file change time"); }
    result.device = identity.dwVolumeSerialNumber;
    result.identity = (std::uint64_t(identity.nFileIndexHigh) << 32) | identity.nFileIndexLow;
    result.changed = basic.ChangeTime.QuadPart;
#else
    struct stat status;
    if (::stat(path.c_str(),&status) != 0) { throw std::runtime_error("cannot inspect input file identity"); }
    result.device = status.st_dev; result.identity = status.st_ino;
#ifdef __APPLE__
    result.changed = status.st_ctimespec.tv_sec; result.changed_fraction = status.st_ctimespec.tv_nsec;
#else
    result.changed = status.st_ctim.tv_sec; result.changed_fraction = status.st_ctim.tv_nsec;
#endif
#endif
    return result;
  }
  void verify(const std::filesystem::path &path) const
  {
    const auto current = capture(path);
    if (size != current.size || modified != current.modified || identity != current.identity
        || device != current.device || changed != current.changed || changed_fraction != current.changed_fraction) {
      throw std::runtime_error("input file changed during query");
    }
  }
};

inline std::vector<std::string> csv_column_names(const std::vector<CsvField> &header)
{
  std::vector<std::string> names;
  std::set<std::string> used;
  for (std::size_t i = 0; i < header.size(); ++i) {
    const std::string base = header[i].text.empty() ? "column" + std::to_string(i + 1) : header[i].text;
    if (base.find(char(0)) != std::string::npos) { throw std::runtime_error("NUL in CSV column name"); }
    const auto folded = [](std::string value) {
      for (char &ch : value) { if (ch >= 'A' && ch <= 'Z') { ch += 'a'-'A'; } }
      return value;
    };
    std::string name = base;
    for (std::size_t suffix = 2; !used.insert(folded(name)).second; ++suffix) {
      name = base + "__" + std::to_string(suffix);
    }
    names.push_back(std::move(name));
  }
  return names;
}

} // namespace oceanbase::sql::file_sql
