#!/usr/bin/env bash
#
# Apply the version-controlled GitHub rulesets in .github/rulesets/ to the fork.
# Idempotent: creates a ruleset if none with that name exists, otherwise updates
# the existing one in place (so re-running after an edit just syncs the change).
#
# Requires: gh (authenticated with a token that has admin on the repo) and jq.
# Usage:    scripts/apply-rulesets.sh [owner/repo]   (default: motrixapp/aria2)

set -euo pipefail

REPO="${1:-motrixapp/aria2}"
DIR="$(cd "$(dirname "$0")/../.github/rulesets" && pwd)"

apply() {
  local file="$1" name id
  name="$(jq -r .name "$file")"
  id="$(gh api "repos/$REPO/rulesets" --jq ".[] | select(.name==\"$name\") | .id" 2>/dev/null || true)"
  if [ -n "$id" ]; then
    echo "→ updating ruleset '$name' (id=$id) on $REPO"
    gh api --method PUT "repos/$REPO/rulesets/$id" --input "$file" >/dev/null
  else
    echo "→ creating ruleset '$name' on $REPO"
    gh api --method POST "repos/$REPO/rulesets" --input "$file" >/dev/null
  fi
}

apply "$DIR/main-branch.json"
apply "$DIR/release-tags.json"
echo "✓ done — review at https://github.com/$REPO/settings/rules"
