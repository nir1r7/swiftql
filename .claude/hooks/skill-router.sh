#!/usr/bin/env bash
# Reads user prompt from stdin (JSON) and injects skill guidance based on intent keywords.
prompt=$(jq -r '.prompt // ""' 2>/dev/null)
lower=$(echo "$prompt" | tr '[:upper:]' '[:lower:]')

skills=()

# Bug / fix / wrong output
if echo "$lower" | grep -qE 'bug|fix|broken|wrong|incorrect|fail|error|crash|assert|mismatch|regress|not work'; then
  skills+=("/project:minimal-fix-strategy  /project:invariant-extraction  /project:execution-state-simulation")
fi

# Query plan / operators
if echo "$lower" | grep -qE '\bplan\b|plan tree|scan|filter node|project node|join node|aggregate|seq.?scan|hash.?join|explain'; then
  skills+=("/project:query-plan-tracing  /project:ast-to-execution-pipeline-tracing")
fi

# Full pipeline trace (SQL → execution)
if echo "$lower" | grep -qE 'pipeline|ast|parser|lexer|token|planner.*executor|sql.*plan|trace'; then
  skills+=("/project:ast-to-execution-pipeline-tracing")
fi

# Operator review / implementation
if echo "$lower" | grep -qE 'implement|review|operator|iterator|open\(\)|next\(\)|close\(\)|volcano|lifecycle'; then
  skills+=("/project:operator-correctness-verification  /project:volcano-iterator-lifecycle-auditing")
fi

# Correctness / semantics / SQLite comparison
if echo "$lower" | grep -qE 'correct|semantic|sqlite|drift|cardinality|null handling|three.valued|wrong result|expected'; then
  skills+=("/project:semantic-drift-detection")
fi

# Feature completeness
if echo "$lower" | grep -qE 'complete|completeness|implement.*feature|feature.*implement|missing|group by|order by|distinct|having|limit|is null'; then
  skills+=("/project:sql-feature-completeness-auditor")
fi

# Planner / executor boundary
if echo "$lower" | grep -qE 'planner|executor|boundary|contract|schema.*mismatch|plan node.*field|output.schema'; then
  skills+=("/project:planner-executor-boundary-validation")
fi

# Columnar storage / zone-map / encodings
if echo "$lower" | grep -qE 'columnar|chunk|zone.?map|encoding|rle|dictionary|pruning|storage layer'; then
  skills+=("/project:columnar-storage-verification")
fi

# Vectorized execution / SelectionVector / late materialization
if echo "$lower" | grep -qE 'vectorized|selection.?vector|data.?chunk|late.?materialization|vec.*node'; then
  skills+=("/project:vectorized-execution-auditing")
fi

# Cost-based optimizer / statistics / cardinality
if echo "$lower" | grep -qE 'optimizer|cost.?model|cardinality|statistics|predicate.?pushdown|join.*order|plan.*rewrite'; then
  skills+=("/project:optimizer-verification")
fi

# EXPLAIN / EXPLAIN ANALYZE / timing / performance diagnosis
if echo "$lower" | grep -qE '\bexplain\b|analyze|rows_in|rows_out|self.?time|\btiming\b|\bslow\b|bottleneck|performance'; then
  skills+=("/project:explain-analyze-interpretation")
fi

if [ ${#skills[@]} -gt 0 ]; then
  # Deduplicate and format
  unique=$(printf '%s\n' "${skills[@]}" | sort -u | tr '\n' '  ')
  printf '{"hookSpecificOutput":{"hookEventName":"UserPromptSubmit","additionalContext":"Relevant skills for this task: %s"}}\n' "$unique"
fi
