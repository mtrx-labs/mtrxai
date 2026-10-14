#!/usr/bin/env bash
# Build the mtrxAI RPM. Usage: ./packaging/build-rpm.sh
set -euo pipefail

cd "$(dirname "$0")/.."
VERSION="$(python3 -c 'import tomllib;print(tomllib.load(open("pyproject.toml","rb"))["project"]["version"])')"
# Name must match Source0 in the spec: %{url}/archive/refs/tags/v%{version}.tar.gz
# (GitHub tag archives extract to mtrxai-<version>/ — same as %autosetup -n).
TARBALL="v${VERSION}.tar.gz"

echo "==> staging ${TARBALL}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir "${STAGE}/mtrxai-${VERSION}"

# Source tarball: package + tests + metadata + the default agent roster.
# NOT .venv/logs/user workspace/.env — those stay local.
# NOTE: excludes are anchored (./name) so top-level workspace/ (sandbox)
# is dropped but mtrxai/agents/ (code) and agents/ (default YAMLs) are kept.
tar -cf - \
    --exclude='./.venv' --exclude='__pycache__' --exclude='*.egg-info' \
    --exclude='./.pytest_cache' --exclude='./logs' --exclude='./.zwork' \
    --exclude='./.env' --exclude='./workspace' \
    --exclude='./mtrxAI.onnx' --exclude='./assets' \
    mtrxai tests agents pyproject.toml README.md LICENSE settings.toml .env.example packaging \
    | tar -xf - -C "${STAGE}/mtrxai-${VERSION}"

tar -czf "${STAGE}/${TARBALL}" -C "$STAGE" "mtrxai-${VERSION}"

mkdir -p ~/rpmbuild/SOURCES ~/rpmbuild/SPECS
cp "${STAGE}/${TARBALL}" ~/rpmbuild/SOURCES/
cp packaging/mtrxai.spec ~/rpmbuild/SPECS/

echo "==> rpmbuild -ba"
rpmbuild -ba ~/rpmbuild/SPECS/mtrxai.spec 2>&1 | tail -30
echo

echo
echo "==> result:"
ls -lh ~/rpmbuild/RPMS/noarch/mtrxai-*.rpm
