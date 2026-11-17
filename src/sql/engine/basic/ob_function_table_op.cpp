/*
 * Copyright (c) 2025 OceanBase.
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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/basic/file_sql/jsonl_reader.h"
#include "sql/engine/basic/file_sql/secure_input.h"
#include "sql/engine/basic/file_sql/glob_reader.h"
#include "sql/engine/basic/file_sql/parquet_reader.h"
#include <memory>
#include <new>
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_,
  is_file_table_, file_format_, file_header_, file_delimiter_, file_quote_, file_escape_, file_null_text_, file_column_count_, file_size_, file_mtime_, file_device_, file_identity_, file_changed_, file_changed_fraction_, file_column_indices_, file_column_names_);


struct FileTableScanState {
  std::filesystem::path path;
  std::unique_ptr<file_sql::SecureInputStream> input;
  file_sql::FileSnapshot snapshot{0, {}};
  std::unique_ptr<file_sql::CsvReader> reader;
  std::vector<file_sql::CsvField> row;
  file_sql::JsonRecord json_record;
  std::size_t json_line = 0;
  std::vector<std::string> files;
  std::unique_ptr<file_sql::ParquetReader> parquet;
  std::size_t file_index = 0;
  FileTableScanState(const std::filesystem::path &name, const std::filesystem::path &root,
                     char delimiter, char quote, char escape, int64_t format) : path(name)
  {
    if (format == 2) {
      files = file_sql::discover_files(name, root);
    } else {
      input = std::make_unique<file_sql::SecureInputStream>(name, root);
      if (!*input) { throw std::runtime_error("cannot open file input"); }
      snapshot = file_sql::FileSnapshot::capture(name);
      reader = std::make_unique<file_sql::CsvReader>(*input, delimiter, 16*1024*1024, quote, escape);
    }
  }
};

void ObFunctionTableOp::close_file_scan()
{
  delete file_scan_;
  file_scan_ = nullptr;
}

int ObFunctionTableOp::open_file_scan()
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  ObString allowed;
  ObDatum *path_value = nullptr;
  if (ctx_.get_my_session() == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.get_my_session()->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(ctx_.get_my_session()->get_secure_file_priv(allowed))) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, path_value))) {
  } else if (path_value == nullptr || path_value->is_null()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    try {
      const ObString name = path_value->get_string();
      const auto root = file_sql::path_from_utf8(std::string(allowed.ptr(), allowed.length()));
      const auto requested = file_sql::path_from_utf8(std::string(name.ptr(), name.length()));
      const auto path = MY_SPEC.file_format_ == 2 ? file_sql::authorize_glob(requested, root)
                                                : file_sql::authorize_input(requested, root, enabled);
      file_scan_ = new FileTableScanState(path, file_sql::path_from_utf8(std::string(allowed.ptr(), allowed.length())),
                                         static_cast<char>(MY_SPEC.file_delimiter_), static_cast<char>(MY_SPEC.file_quote_),
                                         static_cast<char>(MY_SPEC.file_escape_), MY_SPEC.file_format_);
      if (MY_SPEC.file_format_ != 2 && (file_scan_->snapshot.size != MY_SPEC.file_size_
          || file_scan_->snapshot.modified.time_since_epoch().count() != MY_SPEC.file_mtime_
          || file_scan_->snapshot.device != MY_SPEC.file_device_ || file_scan_->snapshot.identity != MY_SPEC.file_identity_
          || file_scan_->snapshot.changed != MY_SPEC.file_changed_ || file_scan_->snapshot.changed_fraction != MY_SPEC.file_changed_fraction_)) {
        throw std::runtime_error("input file changed after schema inference");
      }
      if (MY_SPEC.file_format_ == 3) {
        std::vector<std::size_t> selected;
        for (int64_t i = 0; i < MY_SPEC.file_column_indices_.count(); ++i) {
          selected.push_back(static_cast<std::size_t>(MY_SPEC.file_column_indices_.at(i)));
        }
        file_scan_->parquet = std::make_unique<file_sql::ParquetReader>(path,root,selected);
      }
      if (MY_SPEC.file_format_ == 0 && MY_SPEC.file_header_ && !file_scan_->reader->next(file_scan_->row)) {
        throw std::runtime_error("CSV header disappeared");
      }
    } catch (const std::exception &error) {
      close_file_scan();
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("file SQL scan open failed", K(ret), "detail", error.what());
      LOG_USER_ERROR(OB_INVALID_ARGUMENT,error.what());
    }
  }
  return ret;
}

// Gregorian day number relative to 1970-01-01, matching seekdb DATE storage.
static int32_t file_sql_date_days(const std::string &text)
{
  int year = std::stoi(text.substr(0,4));
  const unsigned month = std::stoi(text.substr(5,2));
  const unsigned day = std::stoi(text.substr(8,2));
  year -= month <= 2;
  const int era = (year >= 0 ? year : year-399) / 400;
  const unsigned yoe = static_cast<unsigned>(year-era*400);
  const unsigned doy = (153*(month > 2 ? month-3 : month+9)+2)/5+day-1;
  return era*146097 + static_cast<int>(yoe*365+yoe/4-yoe/100+doy)-719468;
}

int ObFunctionTableOp::inner_get_next_file_row()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (OB_FAIL(ctx_.check_status())) {
  } else if (file_scan_ == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    try {
      if (MY_SPEC.file_format_ != 2) { file_scan_->snapshot.verify(file_scan_->path); }
      bool found = false;
      if (MY_SPEC.file_format_ == 3) {
        found = file_scan_->parquet->next(file_scan_->row);
      } else if (MY_SPEC.file_format_ == 2) {
        found = file_scan_->file_index < file_scan_->files.size();
        if (found) { file_scan_->row = {{file_scan_->files[file_scan_->file_index++], true}}; }
      } else {
        found = MY_SPEC.file_format_ == 1
          ? file_sql::read_json_record(*file_scan_->input, file_scan_->json_record,&file_scan_->json_line)
          : file_scan_->reader->next(file_scan_->row);
      }
      if (found && MY_SPEC.file_format_ == 0) {
        file_sql::csv_null_markers(file_scan_->row,std::string(MY_SPEC.file_null_text_.ptr(),MY_SPEC.file_null_text_.length()));
      }
      if (!found) {
        ret = OB_ITER_END;
      } else if (MY_SPEC.file_format_ == 0 && file_scan_->row.size() != static_cast<std::size_t>(MY_SPEC.file_column_count_)) {
        throw std::runtime_error("CSV row width changed");
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
          ObExpr *expr = MY_SPEC.column_exprs_.at(i);
          file_sql::CsvField missing;
          const file_sql::CsvField *field_ptr = &missing;
          if (MY_SPEC.file_format_ == 0 || MY_SPEC.file_format_ == 2) {
            field_ptr = &file_scan_->row.at(MY_SPEC.file_column_indices_.at(i));
          } else if (MY_SPEC.file_format_ == 3) {
            field_ptr = &file_scan_->row.at(i);
          } else {
            const ObString &name = MY_SPEC.file_column_names_.at(i);
            for (std::size_t j = 0; j < file_scan_->json_record.names.size(); ++j) {
              const auto &key = file_scan_->json_record.names[j];
              if (key.size() == static_cast<std::size_t>(name.length())
                  && MEMCMP(key.data(), name.ptr(), key.size()) == 0) {
                field_ptr = &file_scan_->json_record.fields[j];
                break;
              }
            }
          }
          const auto &field = *field_ptr;
          const auto &text = field.text;
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (!field.quoted && text.empty()) {
            datum.set_null();
          } else if (field.has_temporal_value && expr->datum_meta_.type_ == ObDateType) {
            if (field.temporal_value < -719162 || field.temporal_value > 2932896) {
              throw std::runtime_error("Parquet DATE out of range");
            }
            datum.set_date(static_cast<int32_t>(field.temporal_value));
          } else if (field.has_temporal_value && expr->datum_meta_.type_ == ObDateTimeType) {
            datum.set_datetime(field.temporal_value);
          } else if (expr->datum_meta_.type_ == ObIntType) {
            int64_t value = 0;
            if (!file_sql::bigint_value(text,value)) {
              throw std::runtime_error("CSV BIGINT conversion failed");
            }
            datum.set_int(value);
          } else if (expr->datum_meta_.type_ == ObTinyIntType) {
            const int value = file_sql::boolean_text(text);
            if (value < 0) { throw std::runtime_error("invalid BOOLEAN input"); }
            datum.set_int(value);
          } else if (expr->datum_meta_.type_ == ObDoubleType) {
            datum.set_double(std::stod(text));
          } else if (expr->datum_meta_.type_ == ObDateType) {
            if (!file_sql::valid_date(text)) { throw std::runtime_error("invalid CSV DATE"); }
            datum.set_date(file_sql_date_days(text));
          } else if (expr->datum_meta_.type_ == ObDateTimeType) {
            if (!file_sql::valid_date(text) && !file_sql::valid_datetime(text)) {
              throw std::runtime_error("invalid CSV DATETIME");
            }
            int64_t seconds = static_cast<int64_t>(file_sql_date_days(text))*86400;
            if (text.size() >= 19) {
              seconds += std::stoi(text.substr(11,2))*3600+std::stoi(text.substr(14,2))*60
                         +std::stoi(text.substr(17,2));
            }
            int64_t fraction = 0;
            if (text.size() > 20) {
              fraction = std::stoll(text.substr(20));
              for (std::size_t digits = text.size()-20; digits < 6; ++digits) { fraction *= 10; }
            }
            datum.set_datetime(seconds*1000000+fraction);
          } else {
            char *buffer = expr->get_str_res_mem(eval_ctx_, text.size());
            if (buffer == nullptr && !text.empty()) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              if (!text.empty()) { MEMCPY(buffer, text.data(), text.size()); }
              datum.set_string(buffer, text.size());
            }
          }
          if (OB_SUCC(ret)) { expr->set_evaluated_projected(eval_ctx_); }
        }
      }
    } catch (const std::exception &error) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("file SQL scan failed", K(ret), "detail", error.what());
      LOG_USER_ERROR(OB_INVALID_ARGUMENT,error.what());
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (MY_SPEC.is_file_table_) {
    return open_file_scan();
  }
  if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (MY_SPEC.is_file_table_) {
    close_file_scan();
    if (OB_FAIL(ObOperator::inner_rescan())) {
    } else {
      ret = open_file_scan();
    }
    return ret;
  }
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    node_idx_ = 0;
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  close_file_scan();
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  return ret;
}

void ObFunctionTableOp::destroy()
{
  close_file_scan();
  ObOperator::destroy();
}


int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return MY_SPEC.is_file_table_ ? inner_get_next_file_row() : (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
