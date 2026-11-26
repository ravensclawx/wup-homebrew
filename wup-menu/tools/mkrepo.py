#!/usr/bin/env python3
"""The catalogue the App Store and Software Update read, in the format of the Homebrew App Store's libget
(github.com/fortheusers/libget): repo.json, zips/<name>.zip (the files to put on the pad, at their paths there),
packages/<name>/icon.png. Publish the folder as it is (a web server over HTTPS, e.g. GitHub Pages).

    python3 tools/mkrepo.py [OUT]          -> build/repo

The packages: the apps below, each zip holding apps/<id>/<id>.wup (the Homebrew Menu's layout), and the Homebrew Menu
itself as "homebrew_menu" (category system), its zip holding kernel.bin first, the kernel's slot as stored (a 4 KB
header sector {"GPST", size, crc32}, then the kernel), and homebrew_menu.bin, the menu's slot as stored (a 4 KB header
sector {"WMNU", stored size, crc32, inflated size, stamp}, then the deflated menu): Software Update writes both."""
import datetime, hashlib, io, json, os, re, shutil, subprocess, sys, zipfile
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import paths
ROOT = os.path.dirname(HERE)
PROJECTS = os.path.dirname(ROOT)

HOME = 'https://github.com/ravensclawx/wup-homebrew'      # (an app's own: url in its app.cfg)

APPS = [   # (folder, category, licence[, more files])
    (os.path.join(ROOT, 'apps', 'app_store'), 'system', 'GPL-3.0-or-later'),          # (system: Software Update's,
    (os.path.join(ROOT, 'apps', 'software_update'), 'system', 'GPL-3.0-or-later'),    #  not listed in the App Store)
    (os.path.join(PROJECTS, 'wup-stream', 'pad'), 'tool', 'GPL-3.0-or-later'),                              # (its own repository, next to this one)
    (os.path.join(PROJECTS, 'wup-boy'), 'emu', 'GPL-3.0-or-later', [('player', 'player.wup')]),          # (its own repository, next to this one)
]


def cfg(d):
    c = {}
    for line in open(os.path.join(d, 'app.cfg')):
        if '=' in line and not line.lstrip().startswith('#'): k, v = line.split('=', 1); c[k.strip()] = re.split(r'\s+#', v)[0].strip()
    return c


def pkg(out, name, title, author, version, category, description, licence, files, icon, url=''):
    z = io.BytesIO()
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf:
        for path, data, how in files: zf.writestr(zipfile.ZipInfo(path, (2026, 1, 1, 0, 0, 0)), data, compress_type=how)
    data = z.getvalue()
    open(os.path.join(out, 'zips', name + '.zip'), 'wb').write(data)
    if icon:
        os.makedirs(os.path.join(out, 'packages', name), exist_ok=True)
        shutil.copy(icon, os.path.join(out, 'packages', name, 'icon.png'))
    return {'name': name, 'title': title, 'author': author, 'version': version, 'category': category, 'license': licence,
            'description': description, 'details': description, 'changelog': '', 'url': url, 'screens': 0, 'app_dls': 0,
            'md5': hashlib.md5(data).hexdigest(), 'filesize': (len(data) + 1023) // 1024,
            'extracted': (sum(len(f[1]) for f in files) + 1023) // 1024,
            'binary': files[0][0] if files[0][0].startswith('apps/') else '', 'updated': datetime.date.today().strftime('%d/%m/%Y')}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(paths.local_out(), 'repo')
    shutil.rmtree(out, ignore_errors=True); os.makedirs(os.path.join(out, 'zips'))
    packages = []
    def built(d):
        r = subprocess.run([sys.executable, os.path.join(paths.LIBWUP, 'tools', 'wupbuild.py'), d], capture_output=True, text=True, cwd=ROOT)
        m = re.search(r'(\S+\.wup) \(', r.stdout)
        if r.returncode or not m: sys.exit('%s did not build:\n%s' % (d, r.stdout[-2000:] + r.stderr[-2000:]))
        return open(m.group(1), 'rb').read()
    def made(script):                                  # (a file a script writes to standard output)
        return subprocess.run([sys.executable, script], capture_output=True, check=True).stdout
    for d, category, licence, *more in APPS:
        c = cfg(d)
        wid = c['name'].replace(' ', '_').lower()
        wup = built(d)
        icon = os.path.join(d, c['icon']) if c.get('icon') else None
        packages.append(pkg(out, wid, c['name'], c.get('author', ''), c.get('version', '1.0'), category, c.get('description', ''),
                            licence, [('apps/%s/%s.wup' % (wid, wid), wup, zipfile.ZIP_STORED)]
                            + [('apps/%s/%s' % (wid, name), made(os.path.join(d, sub)) if sub.endswith('.py') else built(os.path.join(d, sub)), zipfile.ZIP_STORED) for sub, name in (more[0] if more else [])], icon, c.get('url', HOME)))
        print('  %-18s %s' % (wid, c.get('version')))
    from mkflash import kernel_slot, menu_slot         # the system: the kernel and the Homebrew Menu, in one package
    version = re.search(r'MENU_VERSION="([^"]+)"', open(os.path.join(ROOT, 'menu', 'app.cfg')).read()).group(1)
    for w in os.environ.get('WUP_DEFINES', '').split():
        if w.startswith('MENU_VERSION='): version = w.split('=', 1)[1].strip('"')
    kdir = os.path.join(PROJECTS, 'kernel')
    subprocess.run(['make', '-C', kdir], check=True, capture_output=True)
    files = [('kernel.bin', kernel_slot(open(os.path.join(kdir, 'build', 'kernel.bin'), 'rb').read()), zipfile.ZIP_STORED),
             ('homebrew_menu.bin', menu_slot(built(os.path.join(ROOT, 'menu'))), zipfile.ZIP_STORED)]
    packages.append(pkg(out, 'homebrew_menu', 'Homebrew Menu', 'wup-homebrew', version, 'system', 'The Homebrew Menu',
                        'GPL-3.0-or-later', files, None, HOME))
    print('  %-18s %s' % ('homebrew_menu', version))
    json.dump({'packages': packages}, open(os.path.join(out, 'repo.json'), 'w'), indent=1)
    print('%s: %d packages' % (out, len(packages)))


if __name__ == '__main__':
    main()
