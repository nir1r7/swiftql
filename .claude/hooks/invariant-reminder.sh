#!/usr/bin/env bash
# Before editing src/ files, remind Claude to apply invariant-extraction.
file=$(jq -r '.tool_input.file_path // ""' 2>/dev/null)
if echo "$file" | grep -qE '^.*\/src\/'; then
  printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","additionalContext":"Editing src/ — apply /invariant-extraction before changing operator logic or schema-affecting code."}}\n'
fi
