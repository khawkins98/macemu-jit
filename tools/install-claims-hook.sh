#!/bin/bash
# install-claims-hook.sh — install the claims-guard pre-commit hook. Idempotent.
#
# Writes .git/hooks/pre-commit invoking tools/check-claims.sh. If a pre-commit hook
# already exists (and isn't ours), it is preserved at pre-commit.pre-claims and
# chained: the claims guard runs first, then the preserved hook.

set -eu

REPO_ROOT="$(git rev-parse --show-toplevel)"
HOOKS_DIR="$(git -C "$REPO_ROOT" rev-parse --git-path hooks)"
case "$HOOKS_DIR" in
    /*) : ;;
    *) HOOKS_DIR="$REPO_ROOT/$HOOKS_DIR" ;;
esac
HOOK="$HOOKS_DIR/pre-commit"
MARKER="# claims-guard-hook v1"

if [ -f "$HOOK" ] && grep -qF "$MARKER" "$HOOK"; then
    echo "claims-guard pre-commit hook already installed: $HOOK"
    exit 0
fi

CHAIN_LINE=""
if [ -f "$HOOK" ]; then
    mv "$HOOK" "$HOOK.pre-claims"
    echo "Preserved existing pre-commit hook at $HOOK.pre-claims (chained after the guard)."
fi
# Chain unconditionally if a preserved hook exists (covers re-installs too).
if [ -f "$HOOK.pre-claims" ]; then
    CHAIN_LINE='[ -x "$(dirname "$0")/pre-commit.pre-claims" ] && exec "$(dirname "$0")/pre-commit.pre-claims" "$@"'
fi

cat > "$HOOK" <<EOF
#!/bin/bash
$MARKER
# Installed by tools/install-claims-hook.sh — do not edit by hand; re-run the installer.
REPO_ROOT="\$(git rev-parse --show-toplevel)"
"\$REPO_ROOT/tools/check-claims.sh" || exit 1
$CHAIN_LINE
exit 0
EOF
chmod +x "$HOOK"
echo "Installed claims-guard pre-commit hook: $HOOK"
