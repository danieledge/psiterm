# ext.py - a PlatformIO pre-script (platformio.ini, extra_scripts): builds in
# the extension modules (src/ext.h) that are here. A module is a directory
# with a module.json:
#   { "name": "example",
#     "include": ["src"],                        include directories
#     "defines": ["EXAMPLE_OPTION"],             defines for the whole build
#     "src_filter": ["+<src/*.cpp>"] }           its sources (PlatformIO's
#                                                filter syntax, from its directory)
# It is found through a link (or a directory) in ext/ next to platformio.ini
# (not part of the repository), or a path in $AM_EXT_DIRS (separated by ':').
# AM_EXT=0 leaves every module out. With none, the build is the plain one.
# The host tests take the same modules from their own Makefiles
# (hosttest/sources.mk). MIT licence (see LICENSE at the top of the repository).
import json
import os

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


proj = env.subst("$PROJECT_DIR")  # noqa: F821
for d in module_dirs(proj):
    with open(os.path.join(d, "module.json")) as f:
        m = json.load(f)
    name = m.get("name") or os.path.basename(d)
    env.Append(CPPPATH=[env.subst("$PROJECT_SRC_DIR")] + [os.path.join(d, p) for p in m.get("include", [])])  # noqa: F821
    env.Append(CPPDEFINES=m.get("defines", []))  # noqa: F821
    env.BuildSources(os.path.join("$BUILD_DIR", "ext", name), d, m.get("src_filter", ["+<*.cpp>", "+<*.c>"]))  # noqa: F821
    print("Extension module %s: %s" % (name, d))
