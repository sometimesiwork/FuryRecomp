#! /usr/bin/env python3
"""Widen jumps that NASM reports as out of range in the recompiled source.

SR emits the encodings of the original program, so most branches are `short`.
The recompiled code is longer than the original (a far call grows from five
bytes to a push plus a call, some immediates grow), so a branch that fit in 8
bits may no longer fit.  Given NASM's error output on stdin or in a file:

    nasm -f elf32 ... fury-asm.asm 2> nasm.err
    ./repair_short_jumps.py nasm.err
    nasm -f elf32 ... fury-asm.asm 2> nasm.err      # repeat until quiet

every reported `jcc short`/`jmp short` becomes `jcc near`/`jmp` (NASM picks the
short form again when it fits), and an out-of-range `loop` — which has no near
form — gets a relay within reach: the `loop` jumps to a one-instruction stub that
does the near jump, so its exact semantics (decrement CX, test it, leave the flags
alone) are kept.  The same relay serves `jcxz`/`jecxz`/`jrcxz`.
"""
import sys

PREFIX = ' error: short jump is out of range'

_relay_seq = 0


def relay_names(kind, number):
    """Unique labels: the same line can be widened again in a later round."""
    global _relay_seq
    _relay_seq += 1
    return (f'loc_{kind}_relay_{number}_{_relay_seq}',
            f'loc_{kind}_after_{number}_{_relay_seq}')


def widen(line, number):
    """The replacement for one out-of-range branch, or None."""
    stripped = line.lstrip()
    indent = line[:len(line) - len(stripped)]

    mnemonic, _, rest = stripped.partition(' ')
    if rest.startswith('short '):
        rest = rest[len('short '):]
    target = rest.strip()

    if mnemonic == 'jmp':
        return indent + 'jmp ' + target + '\n'

    if mnemonic == 'loop':
        # loop has no near form.  Keep it as it is and give it a relay within
        # reach, so its exact semantics (decrement CX, test it, leave the flags
        # alone) stay in the code; the relay does the near jump.
        relay, after = relay_names('loop', number)
        return (f'{indent}loop {relay}\n'
                f'{indent}jmp short {after}\n'
                f'{relay}:\n'
                f'{indent}jmp near {target}\n'
                f'{after}:\n')

    if mnemonic in ('loopz', 'loope', 'loopnz', 'loopne'):
        relay, after = relay_names('loop', number)
        return (f'{indent}{mnemonic} {relay}\n'
                f'{indent}jmp short {after}\n'
                f'{relay}:\n'
                f'{indent}jmp near {target}\n'
                f'{after}:\n')

    if mnemonic in ('jcxz', 'jecxz', 'jrcxz'):
        relay, after = relay_names('jcxz', number)
        return (f'{indent}{mnemonic} {relay}\n'
                f'{indent}jmp short {after}\n'
                f'{relay}:\n'
                f'{indent}jmp near {target}\n'
                f'{after}:\n')

    if mnemonic.startswith('j'):
        return indent + mnemonic + ' near ' + target + '\n'

    return None


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'nasm.err'

    errors = {}
    with open(path, 'rt', errors='replace') as f:
        for error in f:
            error = error.rstrip('\r\n')
            name, _, rest = error.partition(':')
            number, _, message = rest.partition(':')
            if not message.startswith(PREFIX) or not number.isdigit():
                continue
            errors.setdefault(name, set()).add(int(number))

    total = 0
    for name, numbers in errors.items():
        with open(name, 'rt', newline='') as f:
            lines = f.readlines()

        # later lines first, so earlier line numbers stay valid while the
        # replacements change the length of the file
        for number in sorted(numbers, reverse=True):
            replacement = widen(lines[number - 1], number)
            if replacement is None:
                sys.exit(f'{name}:{number}: not a branch SR should have emitted: '
                         f'{lines[number - 1].strip()!r}')
            lines[number - 1] = replacement
            total += 1

        with open(name, 'wt', newline='') as f:
            f.writelines(lines)

        print(f'{name}: {len(numbers)} jumps widened')

    if total == 0:
        print(f'{path}: no out-of-range jumps')
        return 1

    return 0


if __name__ == '__main__':
    sys.exit(main())
