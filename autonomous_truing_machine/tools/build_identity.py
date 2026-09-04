"""Stamp the build with an identity the running board can show you.

Two boards on this bench serve the operator UI, both of them on http://192.168.4.1/,
and a laptop that loses one access point will silently rejoin the other. That is how an
afternoon went into "the old UI came back after a replug" when the real answer was "that
is the other board". A page that cannot tell you which board and which build it came from
is a page you cannot trust during a demo.

So the commit and a hash of the UI source are computed here and handed to the compiler:

    TRUING_BUILD_REV   short git hash, with -dirty when the tree has uncommitted changes
    TRUING_UI_HASH     first 8 hex of sha256(src/web_ui.h)

The UI hash is what makes the check end to end. The page prints it, the boot log prints
it, and this script prints it at build time, so "is the board serving the file I just
built" stops being a matter of counting bytes by eye.

Wired in from platformio.ini as `extra_scripts = pre:tools/build_identity.py`.
"""
import hashlib
import os
import subprocess

Import("env")  # noqa: F821  (PlatformIO injects this)

PROJECT_DIR = env.subst("$PROJECT_DIR")  # noqa: F821


def git(*args):
    try:
        out = subprocess.check_output(
            ["git"] + list(args), cwd=PROJECT_DIR, stderr=subprocess.DEVNULL
        )
        return out.decode("utf-8", "replace").strip()
    except Exception:
        return ""


def build_rev():
    """Short hash, marked dirty when the tree does not match it.

    The dirty marker matters more than the hash: a clean hash that does not describe the
    bytes on the board is the failure this whole script exists to prevent.
    """
    rev = git("rev-parse", "--short", "HEAD")
    if not rev:
        return "nogit"
    if git("status", "--porcelain", "--untracked-files=no"):
        rev += "-dirty"
    return rev


def ui_hash():
    path = os.path.join(PROJECT_DIR, "src", "web_ui.h")
    try:
        with open(path, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()[:8]
    except OSError:
        return "nofile"


REV = build_rev()
UIH = ui_hash()
print("build identity: rev %s | ui %s | env %s" % (REV, UIH, env.subst("$PIOENV")))  # noqa: F821

env.Append(CPPDEFINES=[  # noqa: F821
    ("TRUING_BUILD_REV", env.StringifyMacro(REV)),      # noqa: F821
    ("TRUING_UI_HASH", env.StringifyMacro(UIH)),        # noqa: F821
])
