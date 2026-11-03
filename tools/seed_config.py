#!/usr/bin/env python3
"""Seed a box config into an NVS partition image, for the first flash of a board.

    source tools/idf-env.sh
    python tools/seed_config.py IN.json OUT.bin

IN.json is a JSON object holding either {"ssid": ..., "pass": ...} (the Wi-Fi network the box
joins) or box config keys as the firmware names them (camelCase: wifiSsid, wifiPass, boxName,
companionHost, companionPort, ethDhcp, surfaceMode ("satellite" or "dock"), ...; see
components/boxconfig/config_json.c). "ssid" and "pass" are written as wifiSsid and wifiPass. Only
the keys given are written: on its first start the firmware lays them over its defaults (and stores
the completed config, with a hotspot password of its own).

OUT.bin is an NVS image of the "nvs" partition (0x9000, 0x6000 bytes, partitions.csv) holding one
string, namespace "boxcfg", key "cfg". Flash it after the firmware (an erase-flash wipes it):

    python -m esptool --chip esp32p4 -p PORT write_flash 0x9000 OUT.bin

This tool never prints or logs a value (Wi-Fi names and passwords stay off the screen and out of
logs): only which keys were written and where the image went. The temporary CSV the NVS generator
needs is written with mode 0600 in a private temporary directory and deleted afterwards; OUT.bin is
written with mode 0600 too.
"""
import csv
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile

NVS_SIZE = 0x6000  # partitions.csv: nvs, data, nvs, 0x9000, 0x6000
NAMESPACE = 'boxcfg'
KEY = 'cfg'

# Box config keys (config_json.c boxcfg_from_json): kind and, for strings, byte length bounds.
STRING_KEYS = {
    'boxName': (1, 32),
    'companionHost': (0, 63),
    'companionPinId': (0, 63),
    'ethIp': (0, 15),
    'ethMask': (0, 15),
    'ethGw': (0, 15),
    'ethDns': (0, 15),
    'wifiSsid': (0, 32),
    'wifiPass': (0, 63),
    'apPass': (8, 63),
    'adminPass': (0, 64),
}
BOOL_KEYS = {'companionAuto', 'ethDhcp', 'apEnabled', 'apSecured', 'mdnsEnabled'}
INT_KEYS = {'companionPort': (1, 65535)}
ENUM_KEYS = {'surfaceMode': ('satellite', 'dock')}
ALIASES = {'ssid': 'wifiSsid', 'pass': 'wifiPass'}


class SeedError(Exception):
    """A problem with the input. The message names keys only, never values."""


def valid_quad(s):
    parts = s.split('.')
    return len(parts) == 4 and all(p.isdigit() and len(p) <= 3 and int(p) <= 255 for p in parts)


def build_config(raw):
    if not isinstance(raw, dict):
        raise SeedError('the input must be a JSON object')
    cfg, given_as = {}, {}
    for key, value in raw.items():
        name = ALIASES.get(key, key)
        if name in cfg:
            raise SeedError('%s and %s both set %s: give one' % (given_as[name], key, name))
        given_as[name] = key
        if name in STRING_KEYS:
            lo, hi = STRING_KEYS[name]
            if not isinstance(value, str):
                raise SeedError('%s: must be a string' % key)
            if '\0' in value:
                raise SeedError('%s: must not contain NUL' % key)
            n = len(value.encode('utf-8'))
            if n < lo or n > hi:
                raise SeedError('%s: must be %d-%d bytes' % (key, lo, hi))
        elif name in BOOL_KEYS:
            if not isinstance(value, bool):
                raise SeedError('%s: must be true or false' % key)
        elif name in INT_KEYS:
            lo, hi = INT_KEYS[name]
            if isinstance(value, bool) or not isinstance(value, int) or not lo <= value <= hi:
                raise SeedError('%s: must be an integer %d-%d' % (key, lo, hi))
        elif name in ENUM_KEYS:
            if value not in ENUM_KEYS[name]:
                raise SeedError('%s: must be one of %s' % (key, ', '.join(ENUM_KEYS[name])))
        else:
            raise SeedError('%s: not a box config key' % key)
        cfg[name] = value
    if not cfg:
        raise SeedError('no keys given')
    # The firmware rejects the whole stored config on any of these (it then starts from defaults).
    if 0 < len(cfg.get('wifiPass', '').encode('utf-8')) < 8:
        raise SeedError('wifiPass: must be empty or 8-63 bytes')
    if cfg.get('ethDhcp') is False:
        for k in ('ethIp', 'ethMask', 'ethGw'):
            if k not in cfg or not valid_quad(cfg[k]):
                raise SeedError('%s: a valid IPv4 address is needed when ethDhcp is false' % k)
        if cfg.get('ethDns') and not valid_quad(cfg['ethDns']):
            raise SeedError('ethDns: not a valid IPv4 address')
    return cfg


def generator_cmd():
    if importlib.util.find_spec('esp_idf_nvs_partition_gen'):
        return [sys.executable, '-m', 'esp_idf_nvs_partition_gen']
    idf = os.environ.get('IDF_PATH')
    script = idf and os.path.join(idf, 'components', 'nvs_flash', 'nvs_partition_generator',
                                  'nvs_partition_gen.py')
    if script and os.path.isfile(script):
        return [sys.executable, script]
    raise SeedError('the ESP-IDF NVS partition generator was not found: run "source tools/idf-env.sh" first')


def write_private(path, write):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w', encoding='utf-8', newline='') as f:
        write(f)


def make_image(cfg, out_path):
    value = json.dumps(cfg, ensure_ascii=False, separators=(',', ':'))
    tmp = tempfile.mkdtemp(prefix='seed_config_')  # mode 0700
    try:
        csv_path = os.path.join(tmp, 'nvs.csv')

        def write_csv(f):
            w = csv.writer(f, lineterminator='\n')
            w.writerow(['key', 'type', 'encoding', 'value'])
            w.writerow([NAMESPACE, 'namespace', '', ''])
            w.writerow([KEY, 'data', 'string', value])

        write_private(csv_path, write_csv)
        bin_path = os.path.join(tmp, 'nvs.bin')
        # The generator's own output is not shown: it is not needed, and nothing it prints may ever
        # carry a value.
        r = subprocess.run(generator_cmd() + ['generate', csv_path, bin_path, hex(NVS_SIZE)], cwd=tmp,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode != 0 or not os.path.isfile(bin_path):
            raise SeedError('the NVS partition generator failed (exit %d)' % r.returncode)
        if os.path.getsize(bin_path) != NVS_SIZE:
            raise SeedError('the NVS image is not 0x%x bytes' % NVS_SIZE)
        os.chmod(bin_path, 0o600)
        shutil.move(bin_path, out_path)
        os.chmod(out_path, 0o600)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv):
    if len(argv) != 3:
        print('usage: seed_config.py IN.json OUT.bin', file=sys.stderr)
        return 2
    in_path, out_path = argv[1], argv[2]
    try:
        try:
            with open(in_path, encoding='utf-8') as f:
                raw = json.load(f)
        except (OSError, ValueError) as e:
            # The parser's message can quote the input: report only the kind of problem.
            raise SeedError('%s: cannot read it as JSON (%s)' % (in_path, type(e).__name__))
        cfg = build_config(raw)
        try:
            make_image(cfg, out_path)
        except OSError as e:
            raise SeedError('%s: cannot write it (%s)' % (out_path, e.strerror or type(e).__name__))
    except SeedError as e:
        print('seed_config: %s' % e, file=sys.stderr)
        return 1
    print('wrote keys %s to %s (NVS namespace "%s", key "%s", 0x%x bytes)'
          % (', '.join(sorted(cfg)), out_path, NAMESPACE, KEY, NVS_SIZE))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
