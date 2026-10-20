#!/usr/bin/env python3
"""Fetch the parts of an iPhone X iOS 16 IPSW this project needs.

  ipsw-fetch.py OUTDIR [--build 20H392] [--device iPhone10,3] [--list]

Looks the firmware up on the ipsw.me API (download URL on Apple's CDN,
size, signing state), then pulls only the members that are used, with HTTP
range requests through tools/remotezip.py (resumable, CRC-checked):

  BuildManifest.plist                     to find the right disk images
  the OS disk image                       root filesystem
  the Cryptex1,SystemOS and ,AppOS images shared cache and cryptex contents
  Firmware/all_flash/DeviceTree.d22ap.im4p   /product for MobileGestalt

and writes OUTDIR/manifest.json with the local paths, plus the raw D22AP
device tree at research/ipsw-x/d22ap.adt (where tools/mkadt.py reads it).
About 6 GB in total. iOS is Apple's software: this downloads it from Apple
for your own use and nothing here redistributes it.
"""
import argparse, json, os, plistlib, subprocess, sys, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import adt  # noqa: E402
import remotezip  # noqa: E402

API = 'https://api.ipsw.me/v4/device/{}?type=ipsw'


def firmwares(device):
    req = urllib.request.Request(API.format(device), headers={'Accept': 'application/json',
                                                              'User-Agent': 'ios-sweet-fajita'})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)['firmwares']


def pick(fws, build):
    if build:
        for f in fws:
            if f['buildid'] == build:
                return f
        raise SystemExit(f'build {build} not found; --list shows what exists')
    ios16 = [f for f in fws if f['version'].startswith('16.')]
    if not ios16:
        raise SystemExit('no iOS 16 firmware listed for this device')
    return ios16[0]  # the API lists newest first


def members(url):
    return {e[0]: e for e in remotezip.central_dir(url)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('outdir')
    ap.add_argument('--device', default='iPhone10,3')
    ap.add_argument('--build', default='20H392', help='iOS build (default 20H392 = 16.7.16, the one the project uses); "" = newest iOS 16')
    ap.add_argument('--list', action='store_true', help='list available iOS 16 builds and exit')
    a = ap.parse_args()

    fws = firmwares(a.device)
    if a.list:
        for f in fws:
            if f['version'].startswith('16.'):
                print(f"{f['version']:10} {f['buildid']:8} {f['filesize'] / 2**30:5.2f} GiB signed={f['signed']}")
        return 0
    fw = pick(fws, a.build or None)
    url = fw['url']
    print(f"{a.device} iOS {fw['version']} ({fw['buildid']}): {url}")
    os.makedirs(a.outdir, exist_ok=True)

    zipdir = members(url)
    remotezip.extract(url, zipdir['BuildManifest.plist'], a.outdir)
    bm = plistlib.load(open(os.path.join(a.outdir, 'BuildManifest.plist'), 'rb'))
    ident = next(i for i in bm['BuildIdentities']
                 if i['Info'].get('DeviceClass', '').lower() == 'd22ap')
    m = ident['Manifest']

    def path_of(key):
        if key not in m:
            raise SystemExit(f'{key} not in the BuildManifest (not an iOS 16 IPSW?)')
        return m[key]['Info']['Path']

    want = {'os': path_of('OS'), 'systemos': path_of('Cryptex1,SystemOS'),
            'appos': path_of('Cryptex1,AppOS'),
            'devicetree': 'Firmware/all_flash/DeviceTree.d22ap.im4p'}
    out = {'device': a.device, 'version': fw['version'], 'build': fw['buildid'], 'url': url}
    for key, name in want.items():
        if name not in zipdir:
            raise SystemExit(f'{name} is not in the IPSW')
        remotezip.extract(url, zipdir[name], a.outdir)
        out[key] = os.path.abspath(os.path.join(a.outdir, name))

    # Raw D22AP ADT for tools/mkadt.py (im4p payload, LZFSE-decoded by adt.py)
    raw = adt.load(out['devicetree'])
    adt.parse(raw)  # sanity check: it is an ADT
    dst = os.path.join(HERE, '..', 'research', 'ipsw-x', 'd22ap.adt')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(raw)
    out['d22ap_adt'] = os.path.abspath(dst)

    json.dump(out, open(os.path.join(a.outdir, 'manifest.json'), 'w'), indent=2)
    print(json.dumps(out, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
