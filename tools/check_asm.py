#!/usr/bin/env python3
"""Detect the SDCC gbz80 "cached accumulator" miscompile in lcc -S output.

Background: see README, "A compiler note". SDCC's gbz80 backend compiles a
comparison of a memory field as `sub a,(...)`, which leaves A holding the
*difference* while the backend can still believe A holds the field. If the
generated code then reuses that A -- storing it into a local, or comparing it
again against something else -- the C source is silently compiled wrong, and
nothing in the C shows it. This project hit exactly that: the win/lose splash
and the aim preview stopped drawing.

Two signatures are flagged, per the README's How-to-check invariant that every
read of a field must be backed by a fresh load:

1.  stored-difference: A is stored to memory while the most recent value
    transition was a comparison (sub/dec), with no reload in between. The
    store of the difference IS the bug.

2.  multi-cmp: a single load of A is compared twice (`sub`/`dec`) with no
    reload in between -- the classic shape of
    `if (X.f != y) { ... if (X.f == c) ... }`. Ordinary arithmetic does not
    flag: add/inc/sbc chains mark A as holding a fresh arithmetic value
    rather than comparison residue, and the next reload resets tracking.

The scan is linear through the listing. State is dropped at function
boundaries (`_label:` / "; Function" headers) and opaque flow (call/jp/ret),
but NOT at the ";src/.." statement comments, which SDCC interleaves
mid-expression.

Run automatically by `make check-asm`; `--self-test` checks the detector
against crafted bad and good listings.
"""
import re
import sys

SRC     = re.compile(r'^;\S+:\d+:')                        # statement comment: opaque, no reset
FUNC    = re.compile(r'^(?:;\s*Function\b|_\S+:)')         # function entry: fresh A
LOAD_A  = re.compile(r'^\tld\s+a,')                       # A gets a real value
ZERO_A  = re.compile(r'^\t(?:xor|ld)\s+a,\s*(?:a|#0x00)\b')  # A = 0
TEST_A  = re.compile(r'^\t(?:and|or)\s+a,\s*a\b')         # flags only, A unchanged
CMP_A   = re.compile(r'^\t(?:sub|dec)\s+a\b')             # SDCC comparison idioms
VAL_A   = re.compile(r'^\t(?:add|adc|sbc|inc|xor)\s+a\b|^\tcpl\b')
FLUSH   = re.compile(r'^\t(?:call|jp|ret)\b|^\tjr\t[0-9]')  # opaque flow: reset
JCC     = re.compile(r'^\tj[pr]\s+[A-Z]{1,2},')           # conditional branch
ST_A    = re.compile(r'^\tldh?\s+\(.+\),\s*a$')           # store of A

SELFTEST_BAD = """\
;main.c:10: if (sim.state != last_state) {
\tld\ta, (#(_sim + 1115) + 0)
\tld\thl, #_last_state
\tsub\ta, (hl)
\tjr\tZ, 00165$
;main.c:775: uint8_t st = sim.state;              /* NO reload: difference stored */
\tldhl\tsp,#2
\tld\t(hl), a
\tsub\ta, #0x03
\tjr\tZ, 00160$
\tsub\ta, #0x04
\tjr\tNZ, 00161$
"""
SELFTEST_GOOD = """\
;main.c:775: volatile uint8_t st = sim.state;     /* reloaded: correct */
\tld\ta, (#(_sim + 1115) + 0)
\tldhl\tsp,#2
\tld\t(hl), a
\tld\ta, (hl)
\tld\thl, #_last_state
\tsub\ta, (hl)
\tjr\tZ, 00165$
\tldhl\tsp,#2
\tld\ta, (hl)
\tsub\ta, #0x03
\tjr\tNZ, 00161$
"""


def scan(text):
    """Yield (line_no, line, signature) for each flagged instruction."""
    fresh = False    # A holds a plain loaded value (tests on it are suspect)
    cmps = 0         # comparisons on that load
    armed = False    # latest transition is a branch-consumed comparison
    pending = False  # a comparison awaits its consuming branch
    hits = []
    for i, ln in enumerate(text.split('\n'), 1):
        if SRC.match(ln):
            continue
        if LOAD_A.match(ln) or ZERO_A.match(ln):
            fresh, cmps, armed, pending = True, 0, False, False
        elif FLUSH.match(ln) or FUNC.match(ln):
            fresh, cmps, armed, pending = False, 0, False, False
        elif TEST_A.match(ln):
            pass
        elif VAL_A.match(ln):
            # arithmetic: A now holds a fresh, intentional value
            fresh, cmps, armed, pending = False, 0, False, False
        elif CMP_A.match(ln) and fresh:
            cmps += 1
            pending = True
        elif JCC.match(ln) and pending:
            # the comparison was actually a comparison: consumed by a branch
            armed, pending = True, False
            if cmps >= 2:
                hits.append((i, ln, 'multi-cmp: one load, second compare, no reload'))
        if ST_A.match(ln) and armed:
            hits.append((i, ln, 'stored-difference: A last set by a compared field, no reload'))
    return hits


def selftest():
    flagged = scan(SELFTEST_BAD)
    if not any(sig.startswith('stored-difference') for _, _, sig in flagged):
        sys.exit('self-test FAILED: stored-difference not fired on crafted bad listing')
    if not any(sig.startswith('multi-cmp') for _, _, sig in flagged):
        sys.exit('self-test FAILED: multi-cmp not fired on crafted bad listing')
    if scan(SELFTEST_GOOD):
        sys.exit('self-test FAILED: detection fired on correct listing')
    print('self-test ok: bad listing flagged (%d hits), correct listing clean'
          % len(flagged))


def main(argv):
    if '--self-test' in argv:
        selftest()
        return 0
    hits = []
    for path in argv or []:
        try:
            hits += [(path,) + h for h in scan(open(path).read())]
        except OSError as e:
            sys.exit('check_asm: %s' % e)
    for path, line_no, line, sig in hits:
        print('%s:%d: %s: %s' % (path, line_no, sig, line))
    if hits:
        sys.exit('check_asm: SDCC reused a stale accumulator over a compared '
                 'field -- see README "A compiler note"')
    print('ok    no SDCC cached-accumulator patterns in assembly')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))