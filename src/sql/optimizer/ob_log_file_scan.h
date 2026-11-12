/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "sql/optimizer/ob_log_function_table.h"

namespace oceanbase
{
namespace sql
{

// File tables share expression allocation with table functions, while retaining
// an independent access-path, logical type, and vectorized physical operator.
class FileScanLogicalOperator : public ObLogFunctionTable
{
public:
  explicit FileScanLogicalOperator(ObLogPlan &plan) : ObLogFunctionTable(plan) {}
  const char *get_name() const override { return "FILE_SCAN"; }
  int get_plan_item_info(PlanText &plan_text, ObSqlPlanItem &plan_item) override;
};

} // namespace sql
} // namespace oceanbase
