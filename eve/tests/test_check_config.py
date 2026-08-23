import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import importlib.util
_spec = importlib.util.spec_from_file_location(
    "check_config", os.path.join(os.path.dirname(__file__), "..", "check-config.py")
)
check_config = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check_config)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(text)


class TestParseConfig(unittest.TestCase):
    def test_parses_values_and_not_set(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, ".config")
            write(p, "\n".join([
                "# comment line",
                "CONFIG_SOUND=y",
                "CONFIG_SND_HDA=m",
                "# CONFIG_STAGING is not set",
                'CONFIG_MODULE_SIG_HASH="sha256"',
                "CONFIG_HZ=250",
                "",
            ]))
            cfg = check_config.parse_config(p)
        self.assertEqual(cfg["SOUND"], "y")
        self.assertEqual(cfg["SND_HDA"], "m")
        self.assertEqual(cfg["STAGING"], "n")
        self.assertEqual(cfg["MODULE_SIG_HASH"], '"sha256"')
        self.assertEqual(cfg["HZ"], "250")
        self.assertNotIn("comment", cfg)

    def test_fragment_uses_same_parser_shape(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "deny.fragment")
            write(p, "# CONFIG_SOUND is not set\nCONFIG_MEGARAID_SAS=y\n")
            frag = check_config.parse_fragment(p)
        self.assertEqual(frag, {"SOUND": "n", "MEGARAID_SAS": "y"})


class TestSymbolMap(unittest.TestCase):
    def test_maps_symbol_to_declaring_directory(self):
        with tempfile.TemporaryDirectory() as d:
            write(os.path.join(d, "sound", "Kconfig"),
                  "menuconfig SOUND\n\tbool \"Sound card support\"\n")
            write(os.path.join(d, "drivers", "scsi", "Kconfig"),
                  "config MEGARAID_SAS\n\ttristate \"LSI MegaRAID SAS\"\n"
                  "config SCSI_MOD\n\ttristate\n")
            m = check_config.build_symbol_map(d)
        self.assertEqual(m["SOUND"], "sound")
        self.assertEqual(m["MEGARAID_SAS"], "drivers/scsi")
        self.assertEqual(m["SCSI_MOD"], "drivers/scsi")

    def test_first_declaration_wins(self):
        with tempfile.TemporaryDirectory() as d:
            write(os.path.join(d, "aaa", "Kconfig"), "config DUP\n\tbool\n")
            write(os.path.join(d, "zzz", "Kconfig"), "config DUP\n\tbool\n")
            m = check_config.build_symbol_map(d)
        self.assertEqual(m["DUP"], "aaa")

    def test_symbol_map_deterministic_traversal_order(self):
        """Verify traversal order is deterministic via sorting, not filesystem.

        Patches os.walk to yield reversed directory order (zzz before aaa) and
        verifies that sorting still causes aaa to win. Fails without sorting.
        """
        with tempfile.TemporaryDirectory() as d:
            # Write both files with the same symbol
            write(os.path.join(d, "aaa", "Kconfig"), "config DETERM\n\tbool\n")
            write(os.path.join(d, "zzz", "Kconfig"), "config DETERM\n\tbool\n")

            # Patch os.walk in the check_config module to reverse dirnames order
            original_walk = os.walk
            def mock_walk(root):
                for dirpath, dirnames, filenames in original_walk(root):
                    # Reverse the dirnames to test that sorting matters
                    dirnames.reverse()
                    yield (dirpath, dirnames, filenames)

            with mock.patch.object(check_config.os, "walk", side_effect=mock_walk):
                m = check_config.build_symbol_map(d)

            # Should win lexicographically (aaa) despite reversed traversal order
            self.assertEqual(m["DETERM"], "aaa")

    def test_declaration_with_trailing_comment(self):
        """Verify that config declarations with trailing # comments are captured."""
        with tempfile.TemporaryDirectory() as d:
            write(os.path.join(d, "clk", "Kconfig"),
                  "config HAVE_LEGACY_CLK # TODO: Remove once all legacy users are migrated\n"
                  "\tbool\n")
            m = check_config.build_symbol_map(d)
        self.assertEqual(m["HAVE_LEGACY_CLK"], "clk")


class TestPolicyList(unittest.TestCase):
    def test_reads_symbols_and_ignores_reasons(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "select-exceptions")
            write(p, "\n".join([
                "# a comment",
                "",
                "AD5592R_BASE      select'd by AD5593R",
                "HTS221_I2C",
                "",
            ]))
            got = check_config.parse_policy_list(p)
        self.assertEqual(got, {"AD5592R_BASE", "HTS221_I2C"})

    def test_missing_file_is_empty_set(self):
        self.assertEqual(check_config.parse_policy_list("/nonexistent/path"), set())


class TestFragmentDisjoint(unittest.TestCase):
    def test_disjoint_fragments_pass(self):
        frags = [("a.fragment", {"SOUND": "n"}), ("b.fragment", {"MEGARAID_SAS": "y"})]
        self.assertEqual(check_config.check_fragment_disjoint(frags), [])

    def test_overlapping_fragments_are_reported(self):
        frags = [("a.fragment", {"SOUND": "n"}), ("b.fragment", {"SOUND": "y"})]
        problems = check_config.check_fragment_disjoint(frags)
        self.assertEqual(len(problems), 1)
        self.assertIn("SOUND", problems[0])
        self.assertIn("a.fragment", problems[0])
        self.assertIn("b.fragment", problems[0])

    def test_overlap_reported_even_when_values_agree(self):
        frags = [("a.fragment", {"SOUND": "n"}), ("b.fragment", {"SOUND": "n"})]
        self.assertEqual(len(check_config.check_fragment_disjoint(frags)), 1)

    def test_later_fragment_reported_as_winner(self):
        # rt.fragment disabling XEN after common.fragment enables its
        # dependent is the motivating case: the later path must be named
        # as the winner, not just "appears in both".
        frags = [("common.fragment", {"XEN": "y"}), ("rt.fragment", {"XEN": "n"})]
        problems = check_config.check_fragment_disjoint(frags)
        self.assertEqual(len(problems), 1)
        self.assertIn("common.fragment", problems[0])
        self.assertIn("rt.fragment wins", problems[0])


class TestFragmentValues(unittest.TestCase):
    def test_all_requested_values_present(self):
        cfg = {"SOUND": "n", "MEGARAID_SAS": "y"}
        frags = [("a.fragment", {"SOUND": "n", "MEGARAID_SAS": "y"})]
        self.assertEqual(check_config.check_fragment_values(cfg, frags, set()), [])

    def test_unheld_value_is_reported(self):
        cfg = {"SOUND": "y"}
        frags = [("a.fragment", {"SOUND": "n"})]
        problems = check_config.check_fragment_values(cfg, frags, set())
        self.assertEqual(len(problems), 1)
        self.assertIn("SOUND", problems[0])
        self.assertIn("requested n", problems[0])
        self.assertIn("got y", problems[0])

    def test_symbol_absent_from_config_counts_as_n(self):
        frags = [("a.fragment", {"SOUND": "n"})]
        self.assertEqual(check_config.check_fragment_values({}, frags, set()), [])

    def test_exception_suppresses_the_failure(self):
        cfg = {"AD5592R_BASE": "m"}
        frags = [("deny.fragment", {"AD5592R_BASE": "n"})]
        self.assertEqual(
            check_config.check_fragment_values(cfg, frags, {"AD5592R_BASE"}), []
        )

    def test_later_fragment_override_is_not_a_failure(self):
        # common.fragment sets XEN_PVCALLS_BACKEND=y; rt.fragment disables
        # its dependency XEN, which drops it. The effective (last) request
        # for XEN is "n" and the config satisfies it - not a failure, even
        # though common.fragment's original request for XEN was "y".
        cfg = {"XEN": "n"}
        frags = [
            ("common.fragment", {"XEN": "y"}),
            ("rt.fragment", {"XEN": "n"}),
        ]
        self.assertEqual(check_config.check_fragment_values(cfg, frags, set()), [])

    def test_effective_value_checked_is_the_last_one(self):
        # Same layering, but this time the actual config holds neither
        # value cleanly overridden nor correctly - it must be judged
        # against the LAST (effective) request, not the first.
        cfg = {"XEN": "y"}  # rt.fragment wanted "n" and didn't get it
        frags = [
            ("common.fragment", {"XEN": "y"}),
            ("rt.fragment", {"XEN": "n"}),
        ]
        problems = check_config.check_fragment_values(cfg, frags, set())
        self.assertEqual(len(problems), 1)
        self.assertIn("rt.fragment", problems[0])
        self.assertIn("requested n", problems[0])
        self.assertIn("got y", problems[0])

    def test_effective_request_that_genuinely_fails_is_reported(self):
        # A later fragment's request is the effective one and it does not
        # hold - a real failure that must survive layering support, not be
        # swallowed by it. (Mirrors rt.fragment's CONFIG_PM case.)
        cfg = {"PM": "y"}
        frags = [
            ("common.fragment", {"PM": "y"}),
            ("rt.fragment", {"PM": "n"}),
        ]
        problems = check_config.check_fragment_values(cfg, frags, set())
        self.assertEqual(len(problems), 1)
        self.assertIn("PM", problems[0])
        self.assertIn("rt.fragment", problems[0])


class TestDenylist(unittest.TestCase):
    SYMMAP = {
        "SND_HDA": "sound/pci/hda",
        "VIDEO_OV5640": "drivers/media/i2c",
        "COMEDI_TEST": "drivers/comedi/drivers",
        "IIO_ST_PRESS": "drivers/iio/pressure",
        "MEGARAID_SAS": "drivers/scsi/megaraid",
        "SOUNDWIRE": "drivers/soundwire",
    }

    def test_clean_config_passes(self):
        cfg = {"MEGARAID_SAS": "y", "SND_HDA": "n"}
        self.assertEqual(
            check_config.check_denylist(
                cfg, self.SYMMAP, check_config.DENIED_PREFIXES, set()
            ),
            [],
        )

    def test_enabled_denied_symbol_is_reported(self):
        cfg = {"SND_HDA": "m", "VIDEO_OV5640": "y", "MEGARAID_SAS": "y"}
        problems = check_config.check_denylist(
            cfg, self.SYMMAP, check_config.DENIED_PREFIXES, set()
        )
        self.assertEqual(len(problems), 2)
        self.assertTrue(any("SND_HDA" in p for p in problems))
        self.assertTrue(any("VIDEO_OV5640" in p for p in problems))

    def test_soundwire_is_not_denied(self):
        # drivers/soundwire must not be caught by the 'sound/' prefix.
        cfg = {"SOUNDWIRE": "m"}
        self.assertEqual(
            check_config.check_denylist(
                cfg, self.SYMMAP, check_config.DENIED_PREFIXES, set()
            ),
            [],
        )

    def test_unknown_symbol_is_ignored(self):
        cfg = {"SYMBOL_NOT_IN_MAP": "y"}
        self.assertEqual(
            check_config.check_denylist(
                cfg, self.SYMMAP, check_config.DENIED_PREFIXES, set()
            ),
            [],
        )

    def test_exception_suppresses_the_failure(self):
        # e.g. CEC_CORE, select'd by a retained display driver despite
        # MEDIA_SUPPORT being denied.
        cfg = {"VIDEO_OV5640": "m"}
        self.assertEqual(
            check_config.check_denylist(
                cfg, self.SYMMAP, check_config.DENIED_PREFIXES, {"VIDEO_OV5640"}
            ),
            [],
        )


class TestBootPath(unittest.TestCase):
    def test_all_builtin_passes(self):
        cfg = {"MEGARAID_SAS": "y", "SCSI_MPI3MR": "y"}
        self.assertEqual(
            check_config.check_boot_path(cfg, {"MEGARAID_SAS", "SCSI_MPI3MR"}), []
        )

    def test_module_in_boot_path_is_reported(self):
        cfg = {"MEGARAID_SAS": "m"}
        problems = check_config.check_boot_path(cfg, {"MEGARAID_SAS"})
        self.assertEqual(len(problems), 1)
        self.assertIn("MEGARAID_SAS", problems[0])
        self.assertIn("initramfs", problems[0])

    def test_absent_boot_symbol_is_reported(self):
        problems = check_config.check_boot_path({}, {"MEGARAID_SAS"})
        self.assertEqual(len(problems), 1)
        self.assertIn("MEGARAID_SAS", problems[0])


class TestBehavioural(unittest.TestCase):
    SYMMAP = {
        "PANIC_ON_OOPS": "kernel",
        "BRIDGE": "net/bridge",
        "EXT4_FS": "fs/ext4",
        "MEGARAID_SAS": "drivers/scsi/megaraid",
        "SND_HDA": "sound/pci/hda",
    }

    def test_classification(self):
        self.assertTrue(check_config.is_behavioural("PANIC_ON_OOPS", self.SYMMAP))
        self.assertTrue(check_config.is_behavioural("BRIDGE", self.SYMMAP))
        self.assertTrue(check_config.is_behavioural("EXT4_FS", self.SYMMAP))
        self.assertFalse(check_config.is_behavioural("MEGARAID_SAS", self.SYMMAP))
        self.assertFalse(check_config.is_behavioural("SND_HDA", self.SYMMAP))

    def test_unknown_symbol_is_not_behavioural(self):
        # Unmapped symbols are skipped rather than assumed behavioural, to
        # avoid noise from generated or arch-internal symbols.
        self.assertFalse(check_config.is_behavioural("NOT_IN_MAP", self.SYMMAP))

    def test_matching_values_pass(self):
        core = {"PANIC_ON_OOPS": "y", "BRIDGE": "y"}
        cfg = {"PANIC_ON_OOPS": "y", "BRIDGE": "y"}
        self.assertEqual(
            check_config.check_behavioural_parity(cfg, core, self.SYMMAP, set()), []
        )

    def test_divergent_value_is_reported(self):
        core = {"BRIDGE": "y"}
        cfg = {"BRIDGE": "m"}
        problems = check_config.check_behavioural_parity(cfg, core, self.SYMMAP, set())
        self.assertEqual(len(problems), 1)
        self.assertIn("BRIDGE", problems[0])
        self.assertIn("core has y", problems[0])

    def test_symbol_absent_from_target_is_reported(self):
        core = {"PANIC_ON_OOPS": "y"}
        problems = check_config.check_behavioural_parity({}, core, self.SYMMAP, set())
        self.assertEqual(len(problems), 1)
        self.assertIn("PANIC_ON_OOPS", problems[0])

    def test_recorded_divergence_is_accepted(self):
        core = {"BRIDGE": "y"}
        cfg = {"BRIDGE": "m"}
        self.assertEqual(
            check_config.check_behavioural_parity(cfg, core, self.SYMMAP, {"BRIDGE"}), []
        )

    def test_driver_divergence_is_ignored(self):
        core = {"MEGARAID_SAS": "y"}
        cfg = {"MEGARAID_SAS": "m"}
        self.assertEqual(
            check_config.check_behavioural_parity(cfg, core, self.SYMMAP, set()), []
        )


class TestRunChecks(unittest.TestCase):
    SYMMAP = {
        "SOUND": "sound",
        "MEGARAID_SAS": "drivers/scsi/megaraid",
        "PANIC_ON_OOPS": "kernel",
    }

    def test_clean_run_returns_zero(self):
        rc, out = check_config.run_checks(
            config={"SOUND": "n", "MEGARAID_SAS": "y", "PANIC_ON_OOPS": "y"},
            symbol_map=self.SYMMAP,
            fragments=[("boot.fragment", {"MEGARAID_SAS": "y"})],
            exceptions=set(),
            boot_symbols={"MEGARAID_SAS"},
            core_config={"PANIC_ON_OOPS": "y"},
            divergences=set(),
            deny=True,
            advisory_parity=False,
        )
        self.assertEqual(rc, 0)
        self.assertIn("PASS", out)

    def test_fatal_problem_returns_one(self):
        rc, out = check_config.run_checks(
            config={"MEGARAID_SAS": "m"},
            symbol_map=self.SYMMAP,
            fragments=[("boot.fragment", {"MEGARAID_SAS": "y"})],
            exceptions=set(),
            boot_symbols={"MEGARAID_SAS"},
            core_config={},
            divergences=set(),
            deny=True,
            advisory_parity=False,
        )
        self.assertEqual(rc, 1)
        self.assertIn("MEGARAID_SAS", out)

    def test_advisory_parity_does_not_fail_the_build(self):
        rc, out = check_config.run_checks(
            config={"PANIC_ON_OOPS": "n"},
            symbol_map=self.SYMMAP,
            fragments=[],
            exceptions=set(),
            boot_symbols=set(),
            core_config={"PANIC_ON_OOPS": "y"},
            divergences=set(),
            deny=False,
            advisory_parity=True,
        )
        self.assertEqual(rc, 0)
        self.assertIn("ADVISORY", out)
        self.assertIn("PANIC_ON_OOPS", out)

    def test_parity_is_fatal_when_not_advisory(self):
        rc, _ = check_config.run_checks(
            config={"PANIC_ON_OOPS": "n"},
            symbol_map=self.SYMMAP,
            fragments=[],
            exceptions=set(),
            boot_symbols=set(),
            core_config={"PANIC_ON_OOPS": "y"},
            divergences=set(),
            deny=False,
            advisory_parity=False,
        )
        self.assertEqual(rc, 1)

    def test_deny_disabled_for_core_flavour(self):
        rc, _ = check_config.run_checks(
            config={"SOUND": "y"},
            symbol_map=self.SYMMAP,
            fragments=[],
            exceptions=set(),
            boot_symbols=set(),
            core_config={},
            divergences=set(),
            deny=False,
            advisory_parity=False,
        )
        self.assertEqual(rc, 0)

    def test_fragment_overlap_is_advisory_not_fatal(self):
        # Overlapping fragments (layering) must not affect the exit code,
        # even though the values genuinely differ across layers - only
        # the effective (last) value is subject to the fatal check, and
        # here it holds.
        rc, out = check_config.run_checks(
            config={"SOUND": "n"},
            symbol_map=self.SYMMAP,
            fragments=[
                ("common.fragment", {"SOUND": "y"}),
                ("rt.fragment", {"SOUND": "n"}),
            ],
            exceptions=set(),
            boot_symbols=set(),
            core_config={},
            divergences=set(),
            deny=False,
            advisory_parity=False,
        )
        self.assertEqual(rc, 0)
        self.assertIn("ADVISORY", out)
        self.assertIn("SOUND", out)


class TestDonorDrift(unittest.TestCase):
    SYMMAP = {
        "E1000E": "drivers/net/ethernet/intel/e1000e",
        "IWLWIFI": "drivers/net/wireless/intel/iwlwifi",
        "PANIC_ON_OOPS": "kernel",
        "SND_HDA": "sound/pci/hda",
    }

    def test_reports_drivers_ubuntu_has_that_we_lack(self):
        donor = {"E1000E": "m", "IWLWIFI": "m"}
        cfg = {"E1000E": "m"}
        report = check_config.report_donor_drift(cfg, donor, self.SYMMAP)
        self.assertEqual(len(report), 1)
        self.assertIn("IWLWIFI", report[0])

    def test_ignores_behavioural_symbols(self):
        donor = {"PANIC_ON_OOPS": "y"}
        self.assertEqual(check_config.report_donor_drift({}, donor, self.SYMMAP), [])

    def test_ignores_denied_subsystems(self):
        donor = {"SND_HDA": "m"}
        self.assertEqual(check_config.report_donor_drift({}, donor, self.SYMMAP), [])


if __name__ == "__main__":
    unittest.main()
