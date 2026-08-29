# SPDX-License-Identifier: Apache-2.0
from pathlib import Path

Import("env")


def normalized_path(value):
    try:
        return Path(str(value)).resolve().as_posix().rstrip("/")
    except (OSError, RuntimeError, ValueError):
        return ""


roots = {
    normalized_path(Path.home()),
    normalized_path(env.subst("$PROJECT_DIR")),
    normalized_path(env.subst("$PROJECT_PACKAGES_DIR")),
}

prefix_flags = []
for root in sorted(item for item in roots if item):
    prefix_flags.extend((
        f"-ffile-prefix-map={root}=.",
        f"-fmacro-prefix-map={root}=.",
    ))

env.Append(CCFLAGS=prefix_flags)
