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
#include <cstdio>
#include <array>
#include <streambuf>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace oceanbase::sql::file_sql {

// Own the opened file, rather than trusting a path check followed by an unrelated open.
class SecureFileBuffer : public std::streambuf {
public:
  SecureFileBuffer(const std::filesystem::path &path, const std::filesystem::path &allowed_root)
  {
    const auto root = std::filesystem::canonical(allowed_root);
    const auto file = authorize_input(path, root, true);
#ifdef _WIN32
    file_ = _wfopen(file.c_str(), L"rb");
    if (file_ == nullptr) { throw std::runtime_error("cannot open input file"); }
    try {
      const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file_)));
      std::vector<wchar_t> resolved(32768);
      const DWORD length = GetFinalPathNameByHandleW(handle, resolved.data(),
                                    static_cast<DWORD>(resolved.size()), FILE_NAME_NORMALIZED);
      if (length == 0 || length >= resolved.size()) { throw std::runtime_error("cannot verify opened file path"); }
      std::wstring final_name(resolved.data(), length);
      if (final_name.compare(0,8,L"\\\\?\\UNC\\") == 0) { final_name = L"\\\\" + final_name.substr(8); }
      else if (final_name.compare(0,4,L"\\\\?\\") == 0) { final_name.erase(0,4); }
      std::filesystem::path opened(final_name);
      authorize_input(opened, root, true);
    } catch (...) {
      std::fclose(file_);
      file_ = nullptr;
      throw;
    }
#else
    int directory = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) { throw std::runtime_error("cannot open allowed directory"); }
    int descriptor = -1;
    try {
#ifdef __linux__
      if (std::filesystem::canonical("/proc/self/fd/" + std::to_string(directory)) != root) {
        throw std::runtime_error("allowed directory changed while opening");
      }
#endif
      const auto relative = file.lexically_relative(root);
      std::vector<std::filesystem::path> parts;
      for (const auto &part : relative) {
        if (part == ".." || part == "." || part.empty()) { throw std::runtime_error("invalid relative input path"); }
        parts.push_back(part);
      }
      if (parts.empty()) { throw std::runtime_error("input is not a file"); }
      for (std::size_t i = 0; i+1 < parts.size(); ++i) {
        const int next = ::openat(directory, parts[i].c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) { throw std::runtime_error("cannot securely open input directory"); }
        ::close(directory);
        directory = next;
      }
      descriptor = ::openat(directory, parts.back().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
      if (descriptor < 0) { throw std::runtime_error("cannot securely open input file"); }
      struct stat status;
      if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        throw std::runtime_error("input is not a regular file");
      }
      file_ = ::fdopen(descriptor, "rb");
      if (file_ == nullptr) { throw std::runtime_error("cannot create input stream"); }
      descriptor = -1;
      ::close(directory);
    } catch (...) {
      if (descriptor >= 0) { ::close(descriptor); }
      ::close(directory);
      throw;
    }
#endif
    setg(buffer_.data(), buffer_.data(), buffer_.data());
  }
  ~SecureFileBuffer() override { if (file_ != nullptr) { std::fclose(file_); } }
  SecureFileBuffer(const SecureFileBuffer &) = delete;
  SecureFileBuffer &operator=(const SecureFileBuffer &) = delete;
protected:
  pos_type seekoff(off_type offset, std::ios_base::seekdir direction,
                   std::ios_base::openmode mode = std::ios_base::in) override
  {
    if ((mode & std::ios_base::out) != 0) { return pos_type(off_type(-1)); }
    const int origin = direction == std::ios_base::beg ? SEEK_SET
                     : direction == std::ios_base::end ? SEEK_END : SEEK_CUR;
    if (direction == std::ios_base::cur) { offset -= egptr()-gptr(); }
#ifdef _WIN32
    if (_fseeki64(file_, offset, origin) != 0) { return pos_type(off_type(-1)); }
    const auto position = _ftelli64(file_);
#else
    if (::fseeko(file_, offset, origin) != 0) { return pos_type(off_type(-1)); }
    const auto position = ::ftello(file_);
#endif
    setg(buffer_.data(), buffer_.data(), buffer_.data());
    return position < 0 ? pos_type(off_type(-1)) : pos_type(position);
  }
  pos_type seekpos(pos_type position, std::ios_base::openmode mode = std::ios_base::in) override
  { return seekoff(static_cast<off_type>(position), std::ios_base::beg, mode); }
  int_type underflow() override
  {
    if (gptr() < egptr()) { return traits_type::to_int_type(*gptr()); }
    const auto count = std::fread(buffer_.data(), 1, buffer_.size(), file_);
    if (count == 0) {
      if (std::ferror(file_)) { throw std::runtime_error("input read failed"); }
      return traits_type::eof();
    }
    setg(buffer_.data(), buffer_.data(), buffer_.data()+count);
    return traits_type::to_int_type(*gptr());
  }
private:
  std::FILE *file_ = nullptr;
  std::array<char, 65536> buffer_;
};

class SecureInputStream : public std::istream {
public:
  SecureInputStream(const std::filesystem::path &path, const std::filesystem::path &allowed_root)
      : std::istream(nullptr), buffer_(path, allowed_root)
  { rdbuf(&buffer_); }
private:
  SecureFileBuffer buffer_;
};
} // namespace oceanbase::sql::file_sql
