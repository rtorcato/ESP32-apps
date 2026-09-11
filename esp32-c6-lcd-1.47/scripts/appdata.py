"""Point PlatformIO's data dir at the current app's own data/ directory.

`data_dir` is a [platformio] option, so it cannot be set per-environment -- but
this repo has one env per app, each with its own data/config.json. Without this
hook every uploadfs needs PLATFORMIO_DATA_DIR spelled out on the command line,
which is long enough to get wrong and silently upload the wrong app's config.

With it, `pio run -e ticker -t uploadfs` uses apps/ticker/data. An app with no
data/ directory is left alone, so envs that ship no config still build.
"""
import os
Import("env")  # noqa: F821  (injected by SCons)

app = env["PIOENV"]

# An explicit PLATFORMIO_DATA_DIR wins. This hook used to override it
# unconditionally, which silently ignored the caller: a deliberate
# `PLATFORMIO_DATA_DIR=/tmp/empty pio run -t uploadfs` uploaded the app's real
# config instead, and reported success either way. Overriding a default is
# helpful; overriding an explicit instruction is a trap.
explicit = os.environ.get("PLATFORMIO_DATA_DIR")
if explicit:
    print(f"appdata: PLATFORMIO_DATA_DIR set explicitly -> {explicit} (not overriding)")
else:
    candidate = os.path.join(env["PROJECT_DIR"], "apps", app, "data")
    if os.path.isdir(candidate):
        env["PROJECT_DATA_DIR"] = candidate
        print(f"appdata: {app} data dir -> apps/{app}/data")
    else:
        print(f"appdata: {app} has no data/ directory; nothing to upload")
