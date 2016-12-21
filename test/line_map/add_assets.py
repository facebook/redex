#! /usr/bin/env python
# @lint-avoid-python-3-compatibility-imports

import argparse
import os
import subprocess
import sys
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('apk', help='Input APK file')
parser.add_argument('assets', help='Files to add to assets/', nargs='+')
parser.add_argument('--keystore')
parser.add_argument('--keypass')
parser.add_argument('--keyalias')
args = parser.parse_args()

with zipfile.ZipFile(args.apk, 'a') as zf:
    for asset in args.assets:
        zf.write(asset, os.path.join('assets', os.path.basename(asset)))

subprocess.check_call([
    'jarsigner',
    '-sigalg', 'SHA1withRSA',
    '-digestalg',
    'SHA1',
    '-keystore',
    args.keystore,
    '-storepass',
    args.keypass,
    args.apk,
    args.keyalias],
    stdout=sys.stderr)
