/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_OPT
#include "sql/optimizer/ob_log_file_scan.h"

namespace oceanbase
{
namespace sql
{

int FileScanLogicalOperator::get_plan_item_info(PlanText &plan_text, ObSqlPlanItem &plan_item)
{
  int ret = ObLogFunctionTable::get_plan_item_info(plan_text, plan_item);
  if (OB_SUCC(ret)) {
    BUF_PRINT_STR("FILE", plan_item.object_type_, plan_item.object_type_len_);
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
