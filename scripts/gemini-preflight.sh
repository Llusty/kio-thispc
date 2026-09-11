#!/usr/bin/env bash
set -euo pipefail
printf 'Project: %s\n' "$(pwd)"
printf 'Branch:  %s\n' "$(git branch --show-current 2>/dev/null || echo no-git)"
printf 'Node:    '; node --version || true
printf 'Gemini:  '; gemini --version || true
printf '\nRecommended next command:\n  gemini\n'
