# ext.py - a PlatformIO pre-script (platformio.ini, extra_scripts): builds in
# the extension modules (src/ext.h) that are here. A module is a directory
# with a module.json:
#   { "name": "example",
#     "include": ["src"],                        include directories
#     "defines": ["EXAMPLE_OPTION"],             defines for the whole build
#     "src_filter": ["+<src/*.cpp>"],            its sources (PlatformIO's
#                                                filter syntax, from its directory)
#     "lib_deps": ["owner/Library@1.2.3"] }      libraries it needs (PlatformIO's
#                                                lib_deps), added to the board's own
# "defines" and "lib_deps" can also be given per chip, as an object keyed by
# the board's MCU ("esp32s3", "esp32"; "*" for every board), so a module can
# use a library only where it fits.
# It is found through a link (or a directory) in ext/ next to platformio.ini
# (not part of the repository), or a path in $AM_EXT_DIRS (separated by ':').
# AM_EXT=0 leaves every module out. With none, the build is the plain one.
# The host tests take the same modules from their own Makefiles
# (hosttest/sources.mk). MIT licence (see LICENSE at the top of the repository).
import json
import os

from platformio.builder.tools.piobuild import BuildSources as pio_build_sources

Import("env")  # noqa: F821 (PlatformIO's SCons environment)


def module_dirs(proj):
    if os.environ.get("AM_EXT", "1") == "0":
        return []
    found = []
    here = os.path.join(proj, "ext")
    if os.path.isdir(here):
        for n in sorted(os.listdir(here)):
            found.append(os.path.join(here, n))
    for d in os.environ.get("AM_EXT_DIRS", "").split(os.pathsep):
        if d:
            found.append(d)
    dirs = []
    for d in found:
        d = os.path.realpath(d)
        if os.path.isfile(os.path.join(d, "module.json")) and d not in dirs:
            dirs.append(d)
    return dirs


def for_board(value, mcu):
    # a list for every board, or {"<mcu>": [...], "*": [...]}
    if isinstance(value, dict):
        return list(value.get("*", [])) + list(value.get(mcu, []))
    return list(value or [])


# A module with libraries is compiled with the project's own environment,
# which has their include directories (the one this script sees does not):
# when the project's sources are, through a wrapper of BuildSources (the
# environment the project builds with is a clone of this one, and clones
# keep added methods)
later = []


def build_sources(e, variant_dir, src_dir, src_filter=None):
    if later and e.subst(src_dir) == e.subst("$PROJECT_SRC_DIR"):
        for args in later:
            pio_build_sources(e, *args)
        del later[:]
    return pio_build_sources(e, variant_dir, src_dir, src_filter)


proj = env.subst("$PROJECT_DIR")  # noqa: F821
mcu = env.BoardConfig().get("build.mcu", "")  # noqa: F821
for d in module_dirs(proj):
    with open(os.path.join(d, "module.json")) as f:
        m = json.load(f)
    name = m.get("name") or os.path.basename(d)
    env.Append(CPPPATH=[env.subst("$PROJECT_SRC_DIR")] + [os.path.join(d, p) for p in m.get("include", [])])  # noqa: F821
    env.Append(CPPDEFINES=for_board(m.get("defines", []), mcu))  # noqa: F821
    deps = for_board(m.get("lib_deps", []), mcu)
    sources = (os.path.join("$BUILD_DIR", "ext", name), d, m.get("src_filter", ["+<*.cpp>", "+<*.c>"]))
    if deps:
        # (the board's lib_deps, read when the libraries are installed and found)
        config = env.GetProjectConfig()  # noqa: F821
        section = "env:" + env["PIOENV"]  # noqa: F821
        have = config.get(section, "lib_deps", [])
        config.set(section, "lib_deps", have + [x for x in deps if x not in have])
        if not later:
            env.AddMethod(build_sources, "BuildSources")  # noqa: F821
        later.append(sources)
    else:
        env.BuildSources(*sources)  # noqa: F821
    print("Extension module %s: %s%s" % (name, d, " (with %s)" % ", ".join(deps) if deps else ""))
