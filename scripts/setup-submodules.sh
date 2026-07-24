#!/usr/bin/env bash
# Fetches every submodule (including nested ones - bell/taojson/mbedtls each
# vendor their own) over HTTPS instead of SSH. Needed because every
# .gitmodules in this dependency tree defaults to git@github.com URLs, which
# fail without an SSH key/agent configured. This rewrites the URL scheme for
# the whole clone via git's global insteadOf mechanism rather than editing
# any vendored repo's own .gitmodules (those aren't ours to modify - a commit
# inside a third-party submodule's checkout wouldn't be reachable by anyone
# else cloning it).
set -euo pipefail

git config --global url."https://github.com/".insteadOf "git@github.com:"
git submodule update --init --recursive
