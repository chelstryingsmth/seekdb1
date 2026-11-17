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
#include "glob_reader.h"
#include "secure_input.h"
#include <random>
#include <cstring>
#include <chrono>

namespace oceanbase::sql::file_sql {
struct CsvOutputOptions {
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string null_text;
};
inline std::string csv_output_record(const std::vector<CsvField> &fields, const CsvOutputOptions &options)
{
  std::string row;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) { row.push_back(options.delimiter); }
    const auto &field = fields[i];
    if (!valid_utf8(field.text)) { throw std::runtime_error("invalid UTF-8 COPY value"); }
    if (!field.quoted && field.text.empty()) { row += options.null_text; }
    else {
      row.push_back(options.quote);
      for (const char ch : field.text) {
        if (ch == options.quote || ch == options.escape) { row.push_back(options.escape); }
        row.push_back(ch);
        if (row.size() > 16*1024*1024) { throw std::runtime_error("COPY record limit exceeded"); }
      }
      row.push_back(options.quote);
    }
  }
  row.push_back('\n');
  if (row.size() > 16*1024*1024) { throw std::runtime_error("COPY record limit exceeded"); }
  return row;
}

class AtomicCsvOutput {
public:
  AtomicCsvOutput(const std::filesystem::path &destination, const std::filesystem::path &root)
  {
    // authorize_glob validates the parent against the configured root without creating a file.
    destination_ = authorize_glob(destination,root);
    if (destination_.filename().empty() || utf8_path_text(destination_.filename()).find_first_of("*?") != std::string::npos) {
      throw std::runtime_error("invalid COPY destination");
    }
    if (std::filesystem::exists(destination_)) { throw std::runtime_error("COPY destination already exists"); }
    const auto token = std::to_string(std::random_device{}()) + "_"
                     + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    temporary_ = destination_.parent_path() / (".seekdb-copy-"+token+".tmp");
#ifdef _WIN32
    directory_ = CreateFileW(destination_.parent_path().c_str(), FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory_ == INVALID_HANDLE_VALUE) { throw std::runtime_error("cannot open COPY directory"); }
    try {
      auto ancestor = destination_.parent_path().parent_path();
      while (!ancestor.empty()) {
        HANDLE pin = CreateFileW(ancestor.c_str(), FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (pin == INVALID_HANDLE_VALUE) { throw std::runtime_error("cannot pin COPY directory ancestry"); }
        try { ancestor_pins_.push_back(pin); } catch (...) { CloseHandle(pin); throw; }
        const auto next = ancestor.parent_path();
        if (next == ancestor) { break; }
        ancestor = next;
      }
      std::vector<wchar_t> resolved(32768);
      const DWORD length = GetFinalPathNameByHandleW(directory_, resolved.data(),
                        static_cast<DWORD>(resolved.size()),FILE_NAME_NORMALIZED);
      if (length == 0 || length >= resolved.size()) { throw std::runtime_error("cannot authorize COPY directory"); }
      std::wstring name(resolved.data(),length);
      if (name.compare(0,8,L"\\\\?\\UNC\\") == 0) { name = L"\\\\" + name.substr(8); }
      else if (name.compare(0,4,L"\\\\?\\") == 0) { name.erase(0,4); }
      authorize_glob(std::filesystem::path(name)/"__seekdb_authorize__",root);
    } catch (...) { cleanup(); throw; }
    file_ = CreateFileW(temporary_.c_str(), GENERIC_WRITE | 0x00010000u, 0, nullptr, CREATE_NEW,
                        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) { cleanup(); throw std::runtime_error("cannot create COPY temporary file"); }
    try {
      std::vector<wchar_t> resolved(32768);
      const DWORD length = GetFinalPathNameByHandleW(file_,resolved.data(),static_cast<DWORD>(resolved.size()),FILE_NAME_NORMALIZED);
      if (length == 0 || length >= resolved.size()) { throw std::runtime_error("cannot authorize COPY handle"); }
      std::wstring name(resolved.data(),length);
      if (name.compare(0,8,L"\\\\?\\UNC\\") == 0) { name = L"\\\\" + name.substr(8); }
      else if (name.compare(0,4,L"\\\\?\\") == 0) { name.erase(0,4); }
      authorize_input(std::filesystem::path(name),root,true);
    } catch (...) { cleanup(); throw; }
#else
    directory_ = ::open(destination_.parent_path().c_str(),O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_ < 0) { throw std::runtime_error("cannot open COPY directory"); }
#ifdef __linux__
    std::error_code directory_error;
    const auto opened_directory = std::filesystem::canonical("/proc/self/fd/"+std::to_string(directory_), directory_error);
    if (directory_error || opened_directory != destination_.parent_path()) {
      ::close(directory_); directory_ = -1; throw std::runtime_error("COPY directory changed");
    }
#endif
    file_ = ::openat(directory_,temporary_.filename().c_str(),O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,0600);
    if (file_ < 0) { ::close(directory_); directory_ = -1; throw std::runtime_error("cannot create COPY temporary file"); }
#endif
  }
  ~AtomicCsvOutput() { cleanup(); }
  AtomicCsvOutput(const AtomicCsvOutput &) = delete;
  AtomicCsvOutput &operator=(const AtomicCsvOutput &) = delete;
  void write(const std::string &bytes)
  {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
#ifdef _WIN32
      DWORD count = 0;
      const DWORD length = static_cast<DWORD>(std::min<std::size_t>(bytes.size()-offset,1024*1024));
      if (!WriteFile(file_,bytes.data()+offset,length,&count,nullptr) || count == 0) { throw std::runtime_error("COPY write failed"); }
#else
      const auto count = ::write(file_,bytes.data()+offset,bytes.size()-offset);
      if (count < 0 && errno == EINTR) { continue; }
      if (count <= 0) { throw std::runtime_error("COPY write failed"); }
#endif
      offset += static_cast<std::size_t>(count);
    }
  }
  void commit()
  {
    if (committed_) { throw std::runtime_error("COPY output already committed"); }
#ifdef _WIN32
    if (!FlushFileBuffers(file_)) { throw std::runtime_error("COPY flush failed"); }
    const std::wstring name = destination_.native();
    const std::size_t bytes = offsetof(FILE_RENAME_INFO,FileName) + (name.size()+1)*sizeof(wchar_t);
    std::vector<unsigned char> storage(bytes);
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(name.size()*sizeof(wchar_t));
    std::memcpy(rename->FileName,name.data(),rename->FileNameLength);
    if (!SetFileInformationByHandle(file_,FileRenameInfo,rename,static_cast<DWORD>(bytes))) {
      throw std::runtime_error("COPY exclusive publication failed: " + std::to_string(GetLastError()));
    }
#else
    if (::fsync(file_) != 0) { throw std::runtime_error("COPY flush failed"); }
    if (::linkat(directory_,temporary_.filename().c_str(),directory_,destination_.filename().c_str(),0) != 0) {
      throw std::runtime_error("COPY exclusive publication failed: " + std::to_string(errno));
    }
    ::unlinkat(directory_,temporary_.filename().c_str(),0);
#endif
    committed_ = true;
  }
private:
  void cleanup() noexcept
  {
#ifdef _WIN32
    if (file_ != INVALID_HANDLE_VALUE) {
      if (!committed_) { FILE_DISPOSITION_INFO disposition{TRUE}; SetFileInformationByHandle(file_,FileDispositionInfo,&disposition,sizeof(disposition)); }
      CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
    }
    if (directory_ != INVALID_HANDLE_VALUE) { CloseHandle(directory_); directory_ = INVALID_HANDLE_VALUE; }
    for (HANDLE pin : ancestor_pins_) { CloseHandle(pin); }
    ancestor_pins_.clear();
#else
    if (file_ >= 0) { ::close(file_); file_ = -1; }
    if (directory_ >= 0) {
      if (!committed_) { ::unlinkat(directory_,temporary_.filename().c_str(),0); }
      ::close(directory_); directory_ = -1;
    }
#endif
  }
  std::filesystem::path destination_, temporary_;
  bool committed_ = false;
#ifdef _WIN32
  HANDLE file_ = INVALID_HANDLE_VALUE;
  HANDLE directory_ = INVALID_HANDLE_VALUE;
  std::vector<HANDLE> ancestor_pins_;
#else
  int file_ = -1, directory_ = -1;
#endif
};
} // namespace oceanbase::sql::file_sql
