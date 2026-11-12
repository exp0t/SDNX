import re, glob, sys, collections
files = glob.glob(sys.argv[1] + '/sdretail_recomp.*.cpp')
func_re = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)')
hits = collections.OrderedDict()
total = 0
for fn in files:
    lines = open(fn, encoding='utf-8', errors='replace').read().split('\n')
    func = None
    i = 0
    while i < len(lines):
        m = func_re.match(lines[i])
        if m: func = m.group(1)
        if 'reserved.u32 = *(uint32_t*)' in lines[i] or 'reserved.u64 = *(uint64_t*)' in lines[i]:
            total += 1
            # destination register of the lwarx (next line: X.u64 = __builtin_bswapNN(reserved...))
            m2 = re.match(r'\s*((?:ctx\.)?r\d+)\.u64 = __builtin_bswap', lines[i + 1]) if i + 1 < len(lines) else None
            dst = m2.group(1) if m2 else None
            j = i + 2
            dep_load = False
            while j < len(lines) and j < i + 60 and 'compare_and_swap' not in lines[j]:
                if dst and re.search(r'REX_LOAD\w*\(\s*' + re.escape(dst) + r'\.u32', lines[j]):
                    dep_load = True
                j += 1
            if dep_load:
                hits.setdefault(func, 0)
                hits[func] += 1
        i += 1
print('lwarx total', total, 'with a load through the reserved value before stwcx:', sum(hits.values()), 'in', len(hits), 'functions')
for f, n in list(hits.items())[:40]: print(f, n)
