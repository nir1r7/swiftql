#include "plan_nodes.h"

// SeqScanNode
SeqScanNode::SeqScanNode(std::vector<Row> rows, Schema schema) : rows_(std::move(rows)), schema_(std::move(schema)), cursor_(0) {}

void SeqScanNode::open() {}
Row* SeqScanNode::next() { return nullptr; }
void SeqScanNode::close() {}
const Schema& SeqScanNode::outputSchema() const { return schema_; }


// FilterNode
FilterNode::FilterNode(std::unique_ptr<PlanNode> child, std::unique_ptr<Expr> predicate) : child_(std::move(child)), predicate_(std::move(predicate)) {}

void FilterNode::open() {}
Row* FilterNode::next() { return nullptr; }
void FilterNode::close() {}
const Schema& FilterNode::outputSchema() const { return child_->outputSchema(); }


// ProjectNode
ProjectNode::ProjectNode(std::unique_ptr<PlanNode> child, std::vector<std::unique_ptr<Expr>> expressions, Schema output_schema) : child_(std::move(child)), expressions_(std::move(expressions)), output_schema_(std::move(output_schema)) {}

void ProjectNode::open() { child_->open(); }
Row* ProjectNode::next() { return child_->next(); }
void ProjectNode::close() {}
const Schema& ProjectNode::outputSchema() const { return output_schema_; }


// HashAggregateNode
HashAggregateNode::HashAggregateNode(std::unique_ptr<PlanNode> child,std::vector<std::string> group_by_cols, std::vector<AggregateSpec> aggregates, Schema output_schema) : child_(std::move(child)), group_by_cols_(std::move(group_by_cols)), aggregates_(std::move(aggregates)), output_schema_(std::move(output_schema)), cursor_(0) {}

void HashAggregateNode::open() {}
Row* HashAggregateNode::next() { return nullptr; }
void HashAggregateNode::close() {}
const Schema& HashAggregateNode::outputSchema() const { return output_schema_; }


// HavingNode
HavingNode::HavingNode(std::unique_ptr<PlanNode> child, std::unique_ptr<Expr> predicate) : child_(std::move(child)), predicate_(std::move(predicate)) {}

void HavingNode::open() {}
Row* HavingNode::next() { return nullptr; }
void HavingNode::close() {}
const Schema& HavingNode::outputSchema() const { return child_->outputSchema(); }


// DistinctNode
DistinctNode::DistinctNode(std::unique_ptr<PlanNode> child) : child_(std::move(child)) {}

void DistinctNode::open() {}
Row* DistinctNode::next() { return nullptr; }
void DistinctNode::close() {}
const Schema& DistinctNode::outputSchema() const { return child_->outputSchema(); }


// SortNode
SortNode::SortNode(std::unique_ptr<PlanNode> child, std::vector<std::string> sort_cols) : child_(std::move(child)), sort_cols_(std::move(sort_cols)), cursor_(0) {}

void SortNode::open() {}
Row* SortNode::next() { return nullptr; }
void SortNode::close() {}
const Schema& SortNode::outputSchema() const { return child_->outputSchema(); }


// HashJoinNode
LimitNode::LimitNode(std::unique_ptr<PlanNode> child, int limit) : child_(std::move(child)), limit_(limit), count_(0) {}

void LimitNode::open() {}
Row* LimitNode::next() { return nullptr; }
void LimitNode::close() {}
const Schema& LimitNode::outputSchema() const { return child_->outputSchema(); }


// HashJoinNode
HashJoinNode::HashJoinNode(std::unique_ptr<PlanNode> left, std::unique_ptr<PlanNode> right, std::string left_col, std::string right_col, Schema output_schema) : left_(std::move(left)), right_(std::move(right)), left_col_(std::move(left_col)), right_col_(std::move(right_col)), output_schema_(std::move(output_schema)) {}

void HashJoinNode::open() {}
Row* HashJoinNode::next() { 
    throw std::runtime_error("Hash join not yet implemented in Phase 1"); 
}
void HashJoinNode::close() {}
const Schema& HashJoinNode::outputSchema() const { return output_schema_; }