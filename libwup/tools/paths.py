"""Where things are in libwup. Every host script imports this instead of building paths itself.
Other projects (the Homebrew Menu, apps) use these tools from here: python3 ../libwup/tools/wupbuild.py <app folder>."""
import os, shutil
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, 'tools')
LIBWUP = ROOT                                 # the runtime every app (and the menu) is built on
INCLUDE = os.path.join(ROOT, 'include')       # wup.h, handoff.h, wupfmt.h, the POSIX headers
SRC = os.path.join(ROOT, 'src')
CONFIG = os.path.join(ROOT, 'config')         # lv_conf.h, lwipopts.h, lwIP's arch/
FONTS = os.path.join(ROOT, 'fonts')
THIRD = os.path.join(ROOT, 'third_party')     # fetched open-source code (tools/deps.py; git-ignored)
BUILD = os.path.join(ROOT, 'build')           # libwup's own builds and caches (git-ignored)
SDK = os.path.join(BUILD, 'sdk')              # the C library and compiler runtime for the pad (tools/sdk.py)
GEN = os.path.join(BUILD, 'gen')              # generated headers: font.h, font_aa.h


def _llvm():
    """the LLVM tools (clang, ld.lld, llvm-nm, llvm-objcopy): $LLVM_BIN, Homebrew's, the Windows installer's, or PATH"""
    for d in (os.environ.get('LLVM_BIN'), '/opt/homebrew/opt/llvm/bin', '/usr/local/opt/llvm/bin',
              os.path.join(os.environ.get('ProgramFiles', r'C:\Program Files'), 'LLVM', 'bin')):
        if d and os.path.isdir(d): return os.path.join(d, '')
    nm = shutil.which('llvm-nm')
    return os.path.join(os.path.dirname(nm), '') if nm else ''


LLVM = _llvm()
if os.name == 'nt' and LLVM:          # Windows: clang and ld.lld from the same LLVM, even if it is not on PATH
    os.environ['PATH'] = LLVM.rstrip('\\/') + os.pathsep + os.environ.get('PATH', '')



def tool(name):
    """an LLVM tool by its full path (Windows does not add .exe to a name with a dot in it, like ld.lld)"""
    return shutil.which(name, path=LLVM.rstrip('\\/') or None) or shutil.which(name) or name


LD, NM, OBJCOPY = tool('ld.lld'), tool('llvm-nm'), tool('llvm-objcopy')


def host_cc():
    """a C compiler for tools that run on this computer (tools/fsimage.c): $CC, cc, gcc or clang"""
    for c in (os.environ.get('CC'), 'cc', 'gcc', 'clang'):
        if c and shutil.which(c): return c
    raise SystemExit('no C compiler for host tools: install one (Windows: MSYS2 gcc), or set CC')


KERNEL = os.environ.get('WUP_KERNEL') == '1'   # builds for the wup-homebrew kernel (wup-homebrew/kernel, include/wupk.h)


def out(*parts):
    """a directory under libwup's build/, created if needed (kernel builds: their own, build/kernel/...)"""
    d = os.path.join(BUILD, *(('kernel',) if KERNEL and parts and parts[0] == 'apps' else ()), *parts)
    os.makedirs(d, exist_ok=True)
    return d


def apps_out():
    """where a finished app goes: build/apps in the folder the tool runs from (like make)"""
    d = os.path.join(os.getcwd(), 'build', 'apps-kernel' if KERNEL else 'apps')
    os.makedirs(d, exist_ok=True)
    return d
