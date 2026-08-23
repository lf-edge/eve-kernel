#!/usr/bin/env python3
"""EVE kernel config policy checker.

Runs inside the kernel build container (see Dockerfile.gcc) before the compile.
Python 3 standard library only - the container has python3 but no pip packages.

Spec: docs/superpowers/specs/2026-08-23-eve-kernel-6.18-config-flavors-design.md
"""

import os
import re

_SET_RE = re.compile(r"^CONFIG_([A-Za-z0-9_]+)=(.*)$")
_UNSET_RE = re.compile(r"^# CONFIG_([A-Za-z0-9_]+) is not set$")
_DECL_RE = re.compile(r"^[ \t]*(?:menu)?config[ \t]+([A-Za-z0-9_]+)[ \t]*$")


def _strip_comment(line):
    """Strip trailing # comment from a line. Returns the line up to the first #."""
    parts = line.split('#', 1)
    return parts[0]


def parse_config(path):
    """Parse a .config or defconfig into {SYMBOL: value}.

    'y'/'m' and literal values are returned verbatim; '# CONFIG_X is not set'
    becomes 'n'. The CONFIG_ prefix is stripped from keys.
    """
    values = {}
    with open(path) as fh:
        for line in fh:
            line = line.rstrip("\n")
            match = _SET_RE.match(line)
            if match:
                values[match.group(1)] = match.group(2)
                continue
            match = _UNSET_RE.match(line)
            if match:
                values[match.group(1)] = "n"
    return values


def parse_fragment(path):
    """Parse a kconfig fragment. Identical syntax to a .config."""
    return parse_config(path)


def build_symbol_map(tree_root):
    """Map {SYMBOL: directory} by walking every Kconfig in the tree.

    Derived from the tree rather than maintained by hand, so subsystem rules
    keep meaning the right thing across kernel version bumps. First declaration
    wins, matching kbuild's own behaviour for our purposes. Traversal is
    deterministic to ensure consistent results across different filesystems.
    """
    symbols = {}
    for dirpath, dirnames, filenames in os.walk(tree_root):
        if ".git" in dirnames:
            dirnames.remove(".git")
        # Sort for deterministic traversal order
        dirnames.sort()
        filenames.sort()
        for name in filenames:
            if not name.startswith("Kconfig"):
                continue
            rel = os.path.relpath(dirpath, tree_root)
            rel = "" if rel == "." else rel
            try:
                with open(os.path.join(dirpath, name), errors="replace") as fh:
                    for line in fh:
                        line_stripped = _strip_comment(line.rstrip("\n"))
                        match = _DECL_RE.match(line_stripped)
                        if match:
                            symbols.setdefault(match.group(1), rel)
            except OSError:
                continue
    return symbols


def parse_policy_list(path):
    """Read a policy file of bare symbol names into a set.

    Blank lines and '#' comments are ignored. Anything after the first
    whitespace on a line is treated as an inline reason and discarded, so
    entries can document themselves. A missing file is an empty set.
    """
    symbols = set()
    if not os.path.exists(path):
        return symbols
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            symbols.add(line.split()[0])
    return symbols


def check_fragment_disjoint(fragments):
    """Report symbols set by more than one fragment layer applied to a flavour.

    This is advisory, not an error: fragments layer, applied in the order the
    flavour lists them, and a later fragment may deliberately override an
    earlier one (e.g. rt.fragment disables XEN_PVCALLS_BACKEND's dependency
    after common.fragment enables it - that is correct, not a conflict).
    merge_config.sh is last-wins, so this just surfaces which layer wins
    rather than letting an override happen silently.

    `fragments` is an ordered list of (path, {SYMBOL: value}) pairs, in the
    order the flavour applies them - the same order passed to
    check_fragment_values.
    """
    seen = {}
    problems = []
    for path, values in fragments:
        for symbol in sorted(values):
            if symbol in seen:
                problems.append(
                    "CONFIG_%s set by both %s and %s; %s wins (applied later)"
                    % (symbol, seen[symbol], path, path)
                )
            seen[symbol] = path
    return problems


def check_fragment_values(config, fragments, exceptions):
    """Every symbol's EFFECTIVE request must hold that value after olddefconfig.

    Fragments layer: a symbol's effective request is its last mention across
    the flavour's fragment list, in list order (matching merge_config.sh's
    last-wins semantics). A value overridden by a later fragment is not
    checked and not a failure - only the effective request is.

    This is the check merge_config.sh implements and EVE disables by passing
    -m (which sets RUNMAKE=false and skips it). Symbols in `exceptions` are
    permitted to differ - they are select'd by something deliberately retained.

    `fragments` is an ordered list of (path, {SYMBOL: value}) pairs, in the
    order the flavour applies them. A plain dict can't express that order
    reliably, and which fragment is "effective" depends entirely on it.
    """
    effective = {}
    for path, values in fragments:
        for symbol, value in values.items():
            effective[symbol] = (path, value)

    problems = []
    for symbol in sorted(effective):
        if symbol in exceptions:
            continue
        path, requested = effective[symbol]
        actual = config.get(symbol, "n")
        if actual != requested:
            problems.append(
                "CONFIG_%s: %s requested %s, got %s"
                % (symbol, path, requested, actual)
            )
    return problems


# Subsystems denied wholesale in hwe/ai. Trailing slashes matter: 'sound/'
# must not match 'drivers/soundwire'. Enforced here as a second net behind
# deny.fragment, which denies the top-level menu symbols directly.
DENIED_PREFIXES = [
    "sound/",
    "drivers/media/",
    "drivers/comedi/",
    "drivers/staging/",
    "drivers/iio/",
]


def _under(directory, prefix):
    return directory == prefix.rstrip("/") or directory.startswith(prefix)


def check_denylist(config, symbol_map, denied_prefixes, exceptions):
    """No symbol under a denied subsystem may be enabled.

    Deliberately redundant with deny.fragment: this catches a symbol enabled
    directly in the base defconfig whose menu dependency changed upstream, so
    that the top-level switch silently stopped covering it. Symbols in
    `exceptions` are permitted to survive - select-exceptions documents them
    generically as "permitted to survive a denial", not solely for the
    fragment-value check, so this net honours it too.
    """
    problems = []
    for symbol in sorted(config):
        if symbol in exceptions:
            continue
        if config[symbol] not in ("y", "m"):
            continue
        directory = symbol_map.get(symbol)
        if directory is None:
            continue
        for prefix in denied_prefixes:
            if _under(directory, prefix):
                problems.append(
                    "CONFIG_%s=%s is under denied subsystem %s"
                    % (symbol, config[symbol], prefix)
                )
                break
    return problems


def check_boot_path(config, boot_symbols):
    """Every boot-path symbol must be built in.

    EVE has no initramfs - /boot holds only kernel, cmdline, ucode.img and
    xen.gz - so a driver needed to reach the root filesystem cannot be a
    module.
    """
    problems = []
    for symbol in sorted(boot_symbols):
        actual = config.get(symbol, "n")
        if actual != "y":
            problems.append(
                "CONFIG_%s=%s must be y: boot-path driver, and EVE has no initramfs"
                % (symbol, actual)
            )
    return problems


# Symbols under these prefixes are hardware drivers. Everything else that
# eve-core_defconfig sets is a behavioural choice EVE has been shipping for
# years, and hwe must not silently diverge from it.
_DRIVER_PREFIXES = ("drivers/", "sound/")


def is_behavioural(symbol, symbol_map):
    """True when a symbol expresses EVE behaviour rather than hardware support.

    Unmapped symbols return False: they are usually generated or arch-internal
    and would add noise without signal.
    """
    directory = symbol_map.get(symbol)
    if directory is None:
        return False
    return not directory.startswith(_DRIVER_PREFIXES)


def check_behavioural_parity(config, core_config, symbol_map, divergences):
    """Non-driver symbols set by eve-core must hold the same value here.

    eve-core_defconfig is the authoritative statement of EVE's behavioural
    choices. Commit bf841829cf73 restored PANIC_ON_OOPS, RT_GROUP_SCHED,
    IKCONFIG and others that the Ubuntu derivation had silently dropped;
    they were found by hand and nothing guaranteed the list was complete.
    This makes that class of loss a build failure instead of archaeology.
    """
    problems = []
    for symbol in sorted(core_config):
        if symbol in divergences:
            continue
        if not is_behavioural(symbol, symbol_map):
            continue
        expected = core_config[symbol]
        actual = config.get(symbol, "n")
        if actual != expected:
            problems.append(
                "CONFIG_%s=%s but core has %s; fix it or record the divergence "
                "with a reason in eve/policy/behavioural-divergence"
                % (symbol, actual, expected)
            )
    return problems


def report_donor_drift(config, donor, symbol_map):
    """Advisory: drivers Ubuntu enables that this config does not.

    Never fatal, and never applied. The donor is a reference for widening
    coverage deliberately; re-deriving a defconfig from it is how commit
    bf841829cf73's losses happened. Behavioural symbols and denied
    subsystems are excluded - parity and the denylist already cover those.
    """
    report = []
    for symbol in sorted(donor):
        if donor[symbol] not in ("y", "m"):
            continue
        if config.get(symbol, "n") in ("y", "m"):
            continue
        directory = symbol_map.get(symbol)
        if directory is None or is_behavioural(symbol, symbol_map):
            continue
        if any(_under(directory, prefix) for prefix in DENIED_PREFIXES):
            continue
        report.append("CONFIG_%s=%s in Ubuntu, absent here (%s)"
                      % (symbol, donor[symbol], directory))
    return report


# Flavours that take deny.fragment and therefore must satisfy the denylist.
DENY_FLAVOURS = ("hwe", "ai")


def run_checks(config, symbol_map, fragments, exceptions, boot_symbols,
               core_config, divergences, deny, advisory_parity):
    """Run every check. Returns (exit_code, report_text).

    `fragments` is an ordered list of (path, {SYMBOL: value}) pairs, in the
    order the flavour applies them (as merge_config.sh would apply them).
    """
    lines = []
    fatal = 0

    def section(title, problems, is_fatal):
        nonlocal fatal
        if not problems:
            lines.append("PASS  %s" % title)
            return
        label = "FAIL " if is_fatal else "ADVISORY"
        lines.append("%s %s (%d)" % (label, title, len(problems)))
        for problem in problems:
            lines.append("        %s" % problem)
        if is_fatal:
            fatal += len(problems)

    section("fragment layering (which layer wins an overlap)",
            check_fragment_disjoint(fragments), False)
    section("fragment effective values held after olddefconfig",
            check_fragment_values(config, fragments, exceptions), True)
    if deny:
        section("denied subsystems absent",
                check_denylist(config, symbol_map, DENIED_PREFIXES, exceptions), True)
    section("boot-path drivers built in",
            check_boot_path(config, boot_symbols), True)
    section("behavioural parity with eve-core",
            check_behavioural_parity(config, core_config, symbol_map, divergences),
            not advisory_parity)

    return (1 if fatal else 0), "\n".join(lines)


def main(argv=None):
    import argparse

    parser = argparse.ArgumentParser(description="EVE kernel config policy checker")
    parser.add_argument("--tree-root", default=".")
    parser.add_argument("--config", required=True)
    parser.add_argument("--flavour", required=True,
                        choices=["core", "rt", "hwe", "ai"])
    parser.add_argument("--fragment", action="append", default=[])
    parser.add_argument("--core-defconfig", required=True)
    parser.add_argument("--advisory-parity", action="store_true")
    args = parser.parse_args(argv)

    policy_dir = os.path.join(args.tree_root, "eve", "policy")
    symbol_map = build_symbol_map(args.tree_root)
    config = parse_config(args.config)
    # Ordered list of (path, {SYMBOL: value}) pairs, in the order the
    # flavour applies them - order matters because fragments layer.
    fragments = [(path, parse_fragment(path)) for path in args.fragment]
    core_config = parse_config(args.core_defconfig)

    deny = args.flavour in DENY_FLAVOURS
    if deny:
        boot_symbols = set()
        for path, values in fragments:
            if path.endswith("boot.fragment"):
                boot_symbols |= {s for s, v in values.items() if v == "y"}
    else:
        boot_symbols = parse_policy_list(os.path.join(policy_dir, "core-boot-path"))

    code, report = run_checks(
        config=config,
        symbol_map=symbol_map,
        fragments=fragments,
        exceptions=parse_policy_list(os.path.join(policy_dir, "select-exceptions")),
        boot_symbols=boot_symbols,
        core_config=core_config,
        divergences=parse_policy_list(
            os.path.join(policy_dir, "behavioural-divergence")),
        deny=deny,
        advisory_parity=args.advisory_parity,
    )
    print("=== eve config policy: flavour=%s ===" % args.flavour)
    print(report)

    donor_path = os.path.join(args.tree_root, "eve", "ubuntu",
                              "resolute-7.0.0-30-generic.config")
    if args.flavour in DENY_FLAVOURS and os.path.exists(donor_path):
        drift = report_donor_drift(config, parse_config(donor_path), symbol_map)
        print("ADVISORY ubuntu donor drift (%d drivers Ubuntu has, we do not)"
              % len(drift))
        for line in drift[:50]:
            print("        %s" % line)
        if len(drift) > 50:
            print("        ... and %d more" % (len(drift) - 50))

    return code


if __name__ == "__main__":
    raise SystemExit(main())
