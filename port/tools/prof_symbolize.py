# Top samples per thread of the last N blocks of a Switch rex_profile.log, symbolized with addr2line on the ELF of the
# same build (nx/elf/). addr2line runs in the devkitA64 Docker image through WSL (SUDO_PW: the WSL sudo password, or be in the docker group).
#   python prof_symbolize.py <rex_profile.log> <nx/elf/sdretail_XXXX.elf> [blocks=4] [top=14] [min_cpu=5]
import collections
import os
import re
import subprocess
import sys

log, elf = sys.argv[1], sys.argv[2]
nblocks = int(sys.argv[3]) if len(sys.argv) > 3 else 4
top = int(sys.argv[4]) if len(sys.argv) > 4 else 14
min_cpu = float(sys.argv[5]) if len(sys.argv) > 5 else 5.0
root = 'D:/CODE/SDNX/'

parts = open(log, encoding='utf-8', errors='replace').read().split('\n==== ')
blocks = parts[-nblocks - 1:-1] if len(parts) > nblocks + 1 else parts[1:]
threads = collections.OrderedDict()
for b in blocks:
    cur = None
    for line in b.split('\n'):
        m = re.match(r'-- thread \d+ "([^"]*)": CPU ([\d.]+)%', line)
        if m:
            cur = m.group(1)
            t = threads.setdefault(cur, {'cpu': 0.0, 'pc': collections.Counter(), 'lr': collections.Counter()})
            t['cpu'] += float(m.group(2)) / len(blocks)
            continue
        m = re.match(r'\s+([\d.]+)%\s+(pc|lr) image\+(0x[0-9a-f]+)', line)
        if m and cur:
            threads[cur][m.group(2)][m.group(3)] += float(m.group(1))

sel = [(k, v) for k, v in threads.items() if v['cpu'] >= min_cpu]
addrs = sorted({a for _, v in sel for kind in ('pc', 'lr') for a, _ in v[kind].most_common(top)})
open(root + 'port/pcs_prof.txt', 'w').write(' '.join(addrs) + '\n')
elf_rel = elf.replace(chr(92), '/').split('SDNX/')[-1]
script = ('/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line -f -C -e /w/' + elf_rel +
          ' $(cat /w/port/pcs_prof.txt)')
open(root + 'port/pcs_prof.sh', 'w', newline='\n').write(script + '\n')
cmd = ('echo "' + os.environ.get('SUDO_PW', '') + '" | sudo -S -p "" docker run --rm -v /mnt/d/CODE/SDNX:/w devkitpro/devkita64:latest '
       'bash /w/port/pcs_prof.sh')
r = subprocess.run(['wsl', '-e', 'bash', '-lc', cmd], capture_output=True, text=True)
out = r.stdout.split('\n')
if len(out) < 2 * len(addrs):
    print('addr2line failed:', r.stderr[:400])
names = {a: (out[2 * i].strip()[:80] if 2 * i < len(out) else '?') for i, a in enumerate(addrs)}
for k, v in sel:
    print('== %s  CPU %.1f%%' % (k, v['cpu']))
    for kind in ('pc', 'lr'):
        for a, p in v[kind].most_common(top):
            print('   %s %5.1f%%  %s' % (kind, p / len(blocks), names.get(a, '?')))
