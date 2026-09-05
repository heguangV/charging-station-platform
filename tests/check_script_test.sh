#!/usr/bin/env bash
set -euo pipefail

check_script="$(realpath "$1")"
format_config="$(realpath "$2")"
command -v clang-format >/dev/null 2>&1 || {
    echo "clang-format is required for the check-script test" >&2
    exit 2
}

test_root="$(mktemp -d /tmp/ncs-check-script-test.XXXXXX)"
cleanup() {
    rm -rf -- "${test_root}"
}
trap cleanup EXIT

expect_failure() {
    local label="$1"
    shift
    if "$@" >"${test_root}/${label}.out" 2>&1; then
        echo "expected failure: ${label}" >&2
        exit 1
    fi
}

mkdir -p "${test_root}/scripts" "${test_root}/tests"
cp "${check_script}" "${test_root}/scripts/check.sh"
cp "${format_config}" "${test_root}/.clang-format"
chmod +x "${test_root}/scripts/check.sh"
cd "${test_root}"

git init --quiet
git config user.name "NCS Check Test"
git config user.email "check-test@example.invalid"
printf 'int main()\n{\n    return 0;\n}\n' >tests/example.cpp
git add .clang-format scripts/check.sh tests/example.cpp
git commit --quiet -m "baseline"
unset NCS_CHECK_BASE_REF

# A clean repository and non-C++ changes must not be blocked by historical
# formatting checks or accidentally passed to clang-format.
./scripts/check.sh >/dev/null
printf 'this is deliberately not valid Python\n' >tests/not_cpp.py
./scripts/check.sh >/dev/null

# New C++ files are checked even before they are added to Git.
printf 'int added(){return 1;}\n' >tests/added.cpp
expect_failure untracked-cpp ./scripts/check.sh
clang-format -i tests/added.cpp
./scripts/check.sh >/dev/null
git add tests/added.cpp tests/not_cpp.py
git commit --quiet -m "add files"

# CI mode must inspect committed changes relative to its explicit base.
base_commit="$(git rev-parse HEAD)"
printf 'int committed(){return 2;}\n' >tests/committed.cpp
git add tests/committed.cpp
git commit --quiet -m "unformatted change"
expect_failure committed-cpp env NCS_CHECK_BASE_REF="${base_commit}" ./scripts/check.sh
clang-format -i tests/committed.cpp
git add tests/committed.cpp
git commit --quiet -m "format change"
NCS_CHECK_BASE_REF="${base_commit}" ./scripts/check.sh >/dev/null

# The same file may occur in both the committed PR range and local changes;
# de-duplication must not trip `set -e`.
sed -i 's/return 2/return 3/' tests/committed.cpp
NCS_CHECK_BASE_REF="${base_commit}" ./scripts/check.sh >/dev/null

# Bad CI configuration and newly introduced oversized files fail closed.
expect_failure invalid-base env NCS_CHECK_BASE_REF=missing-check-base ./scripts/check.sh
grep -q "NCS_CHECK_BASE_REF" "${test_root}/invalid-base.out"
for line_number in $(seq 1 701); do
    printf '// line %s\n' "${line_number}"
done >tests/too_long.cpp
expect_failure oversized ./scripts/check.sh

echo "check.sh regression tests passed"
