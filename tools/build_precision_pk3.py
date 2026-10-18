"""Build dist/zz_codvr_precision.pk3: the carried single-player firearms with
their bullet SPREAD and VIEW KICK fields zeroed, so every shot leaves along
the physical barrel the proxy already aims (request of 2026-09-06: "bullets
ALWAYS go where the barrel is pointing").

Reads the installed pak0.pk3 (never edits it); writes an override archive
that the engine loads after the stock paks (pk3s load in name order, later
wins). Everything else in every weapon file is byte-preserved, and the
script re-parses its own output to prove it. No executable is touched.
"""
import argparse, hashlib, json, os, re, sys, zipfile

BS = chr(92)
PACKAGE = 'zz_codvr_precision.pk3'
SPREAD = ['hipSpreadStandMin', 'hipSpreadDuckedMin', 'hipSpreadProneMin', 'hipSpreadMax',
          'hipSpreadFireAdd', 'hipSpreadTurnAdd', 'hipSpreadMoveAdd', 'adsSpread']
KICK = [p + 'ViewKick' + a + b for p in ('ads', 'hip') for a in ('Pitch', 'Yaw') for b in ('Min', 'Max')]
ZERO = set(SPREAD + KICK)

def fields(raw):
    parts = raw.decode('latin1').split(BS)
    return {parts[i]: parts[i + 1] for i in range(1, len(parts) - 1, 2)}

def patch(raw):
    parts = raw.decode('latin1').split(BS)
    changed = {}
    for i in range(1, len(parts) - 1, 2):
        if parts[i] in ZERO and parts[i + 1] != '0':
            changed[parts[i]] = parts[i + 1]
            parts[i + 1] = '0'
    return BS.join(parts).encode('latin1'), changed

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--main', default=r'H:\SteamLibrary\steamapps\common\Call of Duty\main')
    ap.add_argument('--out', default=os.path.join(os.path.dirname(__file__), '..', 'dist', PACKAGE))
    ap.add_argument('--report', default=os.path.join(os.path.dirname(__file__), '..', 'docs', 'precision_weapons.json'))
    a = ap.parse_args()
    # stock index: later pak wins, our own package excluded
    index = {}
    for pak in sorted(f for f in os.listdir(a.main) if f.lower().endswith('.pk3')):
        if pak.lower() == PACKAGE: continue
        with zipfile.ZipFile(os.path.join(a.main, pak)) as z:
            for n in z.namelist():
                if n.startswith('weapons/sp/') and not n.endswith('/'):
                    index[n] = (pak, z.read(n))
    entries = []
    with zipfile.ZipFile(a.out, 'w', compression=zipfile.ZIP_DEFLATED) as out:
        for path in sorted(index):
            pak, raw = index[path]
            f = fields(raw)
            if f.get('weaponType') != 'bullet' or f.get('weaponSlot') not in ('primary', 'pistol'):
                continue
            patched, changed = patch(raw)
            # oracle: every zeroed key reads 0, every other key is byte-identical, key order kept
            g = fields(patched)
            assert list(g) == list(f), path
            for k in f:
                if k in ZERO: assert g[k] == '0', (path, k)
                else: assert g[k] == f[k], (path, k)
            assert len(patched) <= len(raw)
            info = zipfile.ZipInfo(path, date_time=(2026, 9, 6, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            out.writestr(info, patched)
            entries.append(dict(path=path, source=pak, sha256_original=hashlib.sha256(raw).hexdigest(),
                                sha256_patched=hashlib.sha256(patched).hexdigest(), zeroed=changed))
    # second oracle: re-open the archive and re-check against the stock files
    with zipfile.ZipFile(a.out) as z:
        names = z.namelist()
        assert len(names) == len(entries)
        for e in entries:
            g = fields(z.read(e['path'])); f = fields(index[e['path']][1])
            assert all(g[k] == ('0' if k in ZERO else f[k]) for k in f), e['path']
    with open(a.report, 'w') as fh:
        json.dump(dict(package=PACKAGE, zeroedKeys=sorted(ZERO), weapons=entries), fh, indent=1)
    print(f'{PACKAGE}: {len(entries)} carried firearms, spread + view kick zeroed, all other fields byte-identical')
    for e in entries:
        print(f"  {e['path']:32s} {len(e['zeroed'])} fields zeroed")

if __name__ == '__main__':
    main()
