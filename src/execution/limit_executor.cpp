//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// limit_executor.cpp
//
// Identification: src/execution/limit_executor.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/executors/limit_executor.h"

namespace bustub {

LimitExecutor::LimitExecutor(ExecutorContext *exec_ctx, const LimitPlanNode *plan,
                             std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {}

void LimitExecutor::Init() {
  limit_ = plan_->GetLimit();
  cnt_ = 0;
  child_executor_->Init();
  if (limit_ <= 0) {
    UNREACHABLE("limit <= 0");
  }
}

bool LimitExecutor::Next(Tuple *tuple, RID *rid) { return cnt_++ < limit_ && child_executor_->Next(tuple, rid); }

}  // namespace bustub