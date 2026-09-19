"""Stamp the firmware with the git version it was built from.

A version anyone has to remember to bump is a version that is wrong, so this
takes it from git: the tag if the commit has one, otherwise the short hash,
with -dirty when the tree has uncommitted changes. That last part matters more
than it sounds -- "1.2.0-dirty" on an About page is the difference between
"this is the release" and "this is whatever was on someone's laptop".

Exposed as FW_VERSION and FW_BUILT. No git, or not a repo: "unknown", which
is honest and still builds.
"""
import subprocess

Import("env")  # noqa: F821


def git(*args: str) -> str:
    try:
        return subprocess.check_output(["git", *args], stderr=subprocess.DEVNULL, text=True).strip()
    except Exception:
        return ""


version = git("describe", "--tags", "--always", "--dirty") or "unknown"
built = git("show", "-s", "--format=%cs", "HEAD") or "unknown"

env.Append(CPPDEFINES=[  # noqa: F821
    ("FW_VERSION", env.StringifyMacro(version)),  # noqa: F821
    ("FW_BUILT", env.StringifyMacro(built)),  # noqa: F821
])
print(f"version: {version} (committed {built})")
