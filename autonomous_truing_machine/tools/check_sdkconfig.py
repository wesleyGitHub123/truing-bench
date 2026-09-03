"""Refuse to build against a generated sdkconfig that has drifted from sdkconfig.defaults.

PlatformIO notices when sdkconfig.defaults is newer than the CMake cache and reconfigures,
so it is easy to assume a changed default has taken effect. It has not. ESP-IDF's kconfig
treats an existing sdkconfig.<env> as authoritative for every symbol already in it and
applies the defaults file only to symbols it does not yet carry, so *editing* a value in
sdkconfig.defaults changes nothing in an environment that has been built before. Verified on
this toolchain: with CONFIG_ESP_TASK_WDT_TIMEOUT_S=17 in the defaults and a full reconfigure,
the generated file still read 20.

The generated files are gitignored, so the two boards' copies are whatever each developer's
machine last happened to produce. That is how the Nano came to build with a 5 s task watchdog
while the DevKit had 20 s, from what looked like the same tree, during the portability run.

This runs before every ESP-IDF build and compares the two, so the divergence is a build error
naming the symbol rather than a difference in behaviour discovered on the bench later.

Wired in from platformio.ini as `extra_scripts = pre:tools/check_sdkconfig.py`.
"""
import os
import sys


def parse_defaults(path):
    """CONFIG_x -> value, as written in sdkconfig.defaults."""
    out = {}
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            out[key.strip()] = value.strip()
    return out


def parse_generated(path):
    """CONFIG_x -> value, treating '# CONFIG_x is not set' as the value n."""
    out = {}
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if line.startswith("#") and line.endswith(" is not set"):
                out[line[1:-len(" is not set")].strip()] = "n"
            elif line and not line.startswith("#") and "=" in line:
                key, _, value = line.partition("=")
                out[key.strip()] = value.strip()
    return out


def divergences(defaults, generated):
    """(symbol, wanted, found) for every default the generated file contradicts.

    A symbol the generated file does not mention at all is not reported: kconfig legitimately
    drops symbols whose dependencies are unmet, and failing on those would make the guard
    something developers learn to bypass.
    """
    bad = []
    for key, wanted in defaults.items():
        if key not in generated:
            continue
        found = generated[key]
        if wanted == "n" and found == "n":
            continue
        if found != wanted:
            bad.append((key, wanted, found))
    return bad


def check(project_dir, env_name):
    defaults_path = os.path.join(project_dir, "sdkconfig.defaults")
    generated_path = os.path.join(project_dir, "sdkconfig.%s" % env_name)
    if not os.path.isfile(defaults_path) or not os.path.isfile(generated_path):
        return []            # nothing to compare: a first build generates from the defaults
    return divergences(parse_defaults(defaults_path), parse_generated(generated_path))


def report(bad, env_name):
    print("")
    print("sdkconfig.%s has drifted from sdkconfig.defaults:" % env_name)
    for key, wanted, found in bad:
        print("    %-48s defaults %-12s generated %s" % (key, wanted, found))
    print("")
    print("  The generated file wins, so this build would NOT use the values above.")
    print("  Delete it and build again:  rm sdkconfig.%s" % env_name)
    print("")


try:
    Import("env")                                            # noqa: F821  (PlatformIO injects this)
except NameError:
    if __name__ == "__main__":                               # allow a plain command-line run
        proj = sys.argv[1] if len(sys.argv) > 1 else "."
        name = sys.argv[2] if len(sys.argv) > 2 else "s3_devkit"
        found = check(proj, name)
        if found:
            report(found, name)
            sys.exit(1)
        print("sdkconfig.%s matches sdkconfig.defaults." % name)
else:
    _bad = check(env.subst("$PROJECT_DIR"), env.subst("$PIOENV"))   # noqa: F821
    if _bad:
        report(_bad, env.subst("$PIOENV"))                          # noqa: F821
        env.Exit(1)                                                 # noqa: F821
