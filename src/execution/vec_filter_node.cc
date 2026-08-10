#include "execution/vec_filter_node.h"
#include "execution/columnar_eval.h"
#include "parser/expr_totality.h"
#include "parser/expr_utils.h"
#include <chrono>
#include <utility>


VecFilterNode::VecFilterNode(std::unique_ptr<VecPlanNode> child, std::unique_ptr<Expr> predicate) : child_(std::move(child)), predicate_(std::move(predicate)) {}

void VecFilterNode::open(){
    child_->open();
}

void VecFilterNode::close(){
    child_->close();
}

// WEEK 38 — THE BLOOM PUSHDOWN STOPS AT A PREDICATE THAT CAN RAISE, and this is
// ChunkPruner's rule applied to a second mechanism rather than a new one.
//
// A Bloom filter installed BELOW this node removes rows before the predicate
// above is evaluated on them. Where that predicate can THROW, the removed rows
// are exactly the ones whose evaluation would have thrown, so a query that
// raises today would answer instead — a changed answer, and in the direction
// nobody looks, because an error becoming a result reads as an improvement.
//
// The rule (ChunkPruner::shouldSkip, predicate_pushdown.cc's screen, and
// firstMayRaise in parser/expr_totality.h are the other three sites): a conjunct
// is evaluated on the rows every conjunct written BEFORE it kept, so only
// something written ahead of every raising conjunct may prove a skip. The join's
// membership test is being moved BELOW this whole predicate, i.e. ahead of all
// of it, so ANY raising conjunct here freezes the pushdown.
//
// The real case, from python_tools/compare_against_sqlite.py's
// SCREENING_REFUSED_VEC_ONLY:
//
//   SELECT COUNT(*) FROM laps l
//   WHERE l.lap_id * 9223372036854775807 > 0
//     AND l.driver_id IN (SELECT d.driver_id FROM drivers d WHERE d.age > 999)
//
// The IN lowers to a semi join whose build side is EMPTY, so the pushed filter
// rejects every probe row at the scan and the overflow is never computed:
// "integer overflow in '*'" became 0 rows. The harness has the conjunct-swapped
// twin pinned right beside it, answering 0 with no error — the pair exists to
// say that WRITTEN ORDER decides, which is precisely what a filter pushed under
// the first conjunct erases.
//
// Conservative in one direction on purpose: a raising conjunct written AFTER the
// subquery's own conjunct would be safe to skip past, and this refuses it too.
// The physical node no longer knows which side of the lowered subquery each
// conjunct came from, and a screen may say "I do not know" — the one answer it
// must never give is a confident wrong one.
void VecFilterNode::pushBloomFilter(const std::vector<int>& key_indices,
                                    std::shared_ptr<const BloomFilter> filter) {
    if (conjunctMayRaise(predicate_.get(), child_->outputSchema())) return;
    child_->pushBloomFilter(key_indices, std::move(filter));
}

DataChunk* VecFilterNode::nextChunk(){
    DataChunk* raw = child_->nextChunk();
    if (!raw){
        return nullptr;
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    // late materialization: stamp the SelectionVector onto the child's chunk
    // and pass the pointer through — no column data is copied. evalPredicate
    // returns a fresh vector by value, so reading raw->sel as input_sel (rows
    // already rejected upstream) before the assignment below is safe.
    const SelectionVector* input_sel = raw->filter_applied ? &raw->sel : nullptr;
    SelectionVector out = evalPredicate(predicate_.get(), *raw, child_->outputSchema(),
                                        input_sel, &exec_cache_);
    raw->sel = std::move(out);

    // mark that a filter was applied so VecProjectNode treats empty
    // sel.indices as "zero rows passed" rather than "all rows valid"
    raw->filter_applied = true;

    stats.rows_in  += raw->num_rows;
    stats.rows_out += static_cast<int>(raw->sel.indices.size());
    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();

    return raw;
}

const Schema& VecFilterNode::outputSchema() const {
    return child_->outputSchema();
}

std::string VecFilterNode::explain() const{
    return "VecFilter [" + exprToString(predicate_.get()) + "]";
}

std::vector<VecPlanNode*> VecFilterNode::children() const {
    return {child_.get()};
}