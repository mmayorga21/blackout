"""Pruebas de comportamiento en procesos separados y directorios temporales."""
import os
import csv
from datetime import datetime
import io
from pathlib import Path
import selectors
import subprocess
import tempfile
import struct
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "bin/blackout"


class IndependentStore(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="blackout-test-")
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.config = self.folder / "store.conf"
        self.data = self.folder / "data/nested/D"
        self.journal = self.data / "sales.csv"
        self.configure()

    def configure(self, limit=100000, store="D", chain="none"):
        text = (ROOT / "config/tienda-d.conf").read_text()
        text = text.replace("data_dir=data/D", f"data_dir={self.data}")
        text = text.replace("limit_cent=100000", f"limit_cent={limit}")
        text = text.replace("store_id=D", f"store_id={store}")
        text = text.replace("chain_id=none", f"chain_id={chain}")
        self.config.write_text(text)

    def run_store(self, commands="estado\nsalir\n", fault=None):
        env = os.environ.copy()
        if fault:
            # El cargador separa LD_PRELOAD por espacios antes de expandir ORIGIN.
            # Así funciona también cuando la ruta del repositorio contiene espacios.
            env["LD_PRELOAD"] = "$ORIGIN/../build/fault_io.so"
            env["BLACKOUT_TEST_FAULT"] = fault
        return subprocess.run([str(BINARY), "--config", str(self.config)],
                              input=commands, text=True, capture_output=True,
                              timeout=5, env=env)

    def assert_ok(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_sales_restart_sequence_and_manual_exit(self):
        first = self.run_store("venta 1000\nventa 2500\nsalir\n")
        self.assert_ok(first)
        self.assertIn("Ventas: 2 | Ingresos: 3500", first.stdout)
        second = self.run_store("estado\nventa 300\nsalir\n")
        self.assert_ok(second)
        self.assertIn("Venta aceptada: D:3", second.stdout)
        self.assertIn("Ventas: 3 | Ingresos: 3800", second.stdout)

    def test_invalid_commands_allow_next_sale(self):
        result = self.run_store("venta 0\nventa -1\nventa 1.5\nventa 9223372036854775808\n"
                                "venta 1 2\nventa\nnoexiste\n" + "a" * 400 + "\nventa 500\n")
        self.assert_ok(result)
        self.assertEqual(result.stdout.count("Venta aceptada:"), 1)
        self.assertIn("Ventas: 1 | Ingresos: 500", result.stdout)

    def test_limit_and_restart_closed(self):
        self.configure(limit=3000)
        result = self.run_store("venta 2500\nventa 1000\nventa 50\n")
        self.assert_ok(result)
        self.assertIn("Estado: SOLO_CONSULTA", result.stdout)
        self.assertIn("Ventas: 2 | Ingresos: 3500", result.stdout)
        restart = self.run_store("venta 100\nestado\nhistorial\nayuda\nsalir\n")
        self.assert_ok(restart)
        self.assertNotIn("Venta aceptada:", restart.stdout)
        self.assertIn("Ventas: 2 | Ingresos: 3500", restart.stdout)
        self.assertIn("D:1 |", restart.stdout)
        self.assertIn("D:2 |", restart.stdout)
        self.assertIn("Salida ordenada", restart.stdout)

    def test_exact_limit(self):
        self.configure(limit=1000)
        result = self.run_store("venta 1000\nventa 1\n")
        self.assert_ok(result)
        self.assertIn("Estado: SOLO_CONSULTA", result.stdout)
        self.assertEqual(result.stdout.count("Venta aceptada:"), 1)

    def test_accumulation_overflow(self):
        self.configure(limit=9223372036854775807)
        result = self.run_store("venta 9223372036854775806\nventa 2\nventa 1\n")
        self.assert_ok(result)
        self.assertIn("Ventas: 2 | Ingresos: 9223372036854775807", result.stdout)
        self.assertIn("Venta rechazada", result.stderr)

    def test_exclusive_process_lock_and_release_after_kill(self):
        first = subprocess.Popen([str(BINARY), "--config", str(self.config)],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, bufsize=0)
        self.addCleanup(lambda: first.poll() is None and first.kill())
        selector = selectors.DefaultSelector()
        try:
            selector.register(first.stdout, selectors.EVENT_READ)
            output = b""
            while b"Estado: ABIERTA" not in output:
                self.assertTrue(selector.select(5), "La primera instancia no inició")
                chunk = os.read(first.stdout.fileno(), 4096)
                self.assertTrue(chunk)
                output += chunk
            second = self.run_store()
            self.assertNotEqual(second.returncode, 0)
            self.assertIn("otra instancia", second.stderr)
            first.stdin.write(b"venta 1200\n")
            while b"Venta aceptada: D:1" not in output:
                self.assertTrue(selector.select(5), "La venta no se confirmó")
                output += os.read(first.stdout.fileno(), 4096)
            first.kill()
            first.communicate(timeout=5)
        finally:
            selector.close()
            if first.poll() is None:
                first.kill()
                first.communicate(timeout=5)
        restart = self.run_store("venta 100\n")
        self.assert_ok(restart)
        self.assertIn("Venta aceptada: D:2", restart.stdout)
        self.assertIn("Ingresos: 1300", restart.stdout)

    def test_partial_record_after_crash(self):
        self.assert_ok(self.run_store("venta 1000\nsalir\n"))
        crashed = self.run_store("venta 2500\n", fault="partial_exit")
        self.assertEqual(crashed.returncode, 77)
        self.assertNotIn("Venta aceptada:", crashed.stdout)
        restart = self.run_store("venta 500\n")
        self.assert_ok(restart)
        self.assertIn("registro incompleto", restart.stderr)
        self.assertIn("Venta aceptada: D:2", restart.stdout)
        self.assertIn("Ingresos: 1500", restart.stdout)

    def test_complete_record_without_confirmation(self):
        self.assert_ok(self.run_store())
        crashed = self.run_store("venta 800\n", fault="full_exit")
        self.assertEqual(crashed.returncode, 78)
        self.assertNotIn("Venta aceptada:", crashed.stdout)
        restart = self.run_store("venta 100\n")
        self.assert_ok(restart)
        self.assertIn("Venta aceptada: D:2", restart.stdout)
        self.assertIn("Ingresos: 900", restart.stdout)

    def test_complete_corruption_preserves_file(self):
        self.assert_ok(self.run_store("venta 500\nventa 600\nsalir\n"))
        content = bytearray(self.journal.read_bytes())
        content[76] ^= 1
        self.journal.write_bytes(content)
        restart = self.run_store()
        self.assertNotEqual(restart.returncode, 0)
        self.assertIn("corrupto", restart.stderr)
        self.assertEqual(self.journal.read_bytes(), content)

    def test_incompatible_identity_or_limit_preserves_file(self):
        self.assert_ok(self.run_store("venta 500\n"))
        content = self.journal.read_bytes()
        for kwargs in ({"store": "E"}, {"limit": 200000}):
            self.configure(**kwargs)
            result = self.run_store()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("incompatible", result.stderr)
            self.assertEqual(content, self.journal.read_bytes())

    def test_truncated_header_preserves_file(self):
        self.assert_ok(self.run_store())
        self.journal.write_bytes(b"BLKOUT1\n")
        result = self.run_store()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cabecera CSV incompleta", result.stderr)
        self.assertEqual(self.journal.read_bytes(), b"BLKOUT1\n")

    def test_write_and_sync_failures_never_confirm(self):
        self.assert_ok(self.run_store())
        for fault in ("enospc", "zero_write", "sync_error"):
            with self.subTest(fault=fault):
                before = self.journal.read_bytes()
                result = self.run_store("venta 100\nventa 200\n", fault=fault)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("Venta aceptada:", result.stdout)
                self.assertIn("Se detiene la instancia", result.stderr)
                # Restablecer fixture: el resultado de fsync fallido es incierto.
                self.journal.write_bytes(before)

    def test_short_writes_and_interrupted_system_calls(self):
        for fault in ("short_write", "eintr"):
            with self.subTest(fault=fault):
                result = self.run_store("venta 350\nsalir\n", fault=fault)
                self.assert_ok(result)
        restart = self.run_store()
        self.assert_ok(restart)
        self.assertIn("Ventas: 2 | Ingresos: 700", restart.stdout)

    def test_chain_configuration_not_silently_treated_as_independent(self):
        self.configure(chain="X")
        result = self.run_store()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("solo admite tiendas independientes", result.stderr)
        self.assertFalse(self.journal.exists())

    def test_invalid_storage_location(self):
        self.data.parent.mkdir(parents=True)
        self.data.write_text("No es un directorio")
        result = self.run_store()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("Venta aceptada:", result.stdout)

    def test_symlink_and_hardlink_journals_rejected(self):
        self.assert_ok(self.run_store())
        original = self.journal.with_name("original")
        self.journal.rename(original)
        self.journal.symlink_to(original)
        self.assertNotEqual(self.run_store().returncode, 0)
        self.journal.unlink()
        os.link(original, self.journal)
        self.assertNotEqual(self.run_store().returncode, 0)

    def test_csv_fields_timestamp_amount_and_history(self):
        result = self.run_store("historial\nventa 1250\nhistorial\nventa 1\nhistorial\nsalir\n")
        self.assert_ok(result)
        self.assertIn("Sin ventas registradas", result.stdout)
        self.assertIn("D:1 |", result.stdout)
        self.assertIn("| 12.50", result.stdout)
        self.assertIn("D:2 |", result.stdout)
        self.assertIn("| 0.01", result.stdout)
        rows = list(csv.DictReader(io.StringIO(self.journal.read_text())))
        self.assertEqual(list(rows[0]), ["identificador", "fecha_hora", "valor_venta", "crc32"])
        self.assertEqual([r["identificador"] for r in rows], ["D:1", "D:2"])
        self.assertEqual([r["valor_venta"] for r in rows], ["12.50", "0.01"])
        stamp = datetime.strptime(rows[0]["fecha_hora"], "%Y-%m-%dT%H:%M:%S%z")
        self.assertIsNotNone(stamp.utcoffset())
        self.assertLess(abs((datetime.now().astimezone() - stamp).total_seconds()), 10)
        self.assert_ok(self.run_store("venta 50\nhistorial\n"))
        self.assertIn("Ingresos: 1301", self.run_store().stdout)

    def test_history_option_for_closed_store(self):
        self.configure(limit=1000)
        self.assert_ok(self.run_store("venta 1000\n"))
        original = self.journal.read_bytes()
        result = subprocess.run([str(BINARY), "--config", str(self.config), "--historial"],
                                text=True, capture_output=True, timeout=5)
        self.assert_ok(result)
        self.assertIn("D:1 |", result.stdout)
        self.assertIn("| 10.00", result.stdout)
        self.assertEqual(original, self.journal.read_bytes())

    def create_legacy(self, amounts=(1000, 2500)):
        self.data.mkdir(parents=True)
        header = bytearray(64)
        header[:8] = b"BLKOUT1\n"
        header[8:9] = b"D"
        struct.pack_into("<Q", header, 40, 100000)
        struct.pack_into("<I", header, 60, zlib.crc32(header[:60]))
        content = header
        for sequence, amount in enumerate(amounts, 1):
            record = bytearray(32)
            record[:4] = b"SALE"
            struct.pack_into("<QQ", record, 4, sequence, amount)
            struct.pack_into("<I", record, 28, zlib.crc32(record[:28]))
            content.extend(record)
        legacy = self.data / "sales.journal"
        legacy.write_bytes(content)
        return legacy

    def test_migration_keeps_original_and_unknown_dates(self):
        legacy = self.create_legacy()
        original = legacy.read_bytes()
        result = self.run_store("historial\nventa 50\nsalir\n")
        self.assert_ok(result)
        self.assertIn("convertido a sales.csv", result.stderr)
        self.assertIn("D:1 | desconocida | 10.00", result.stdout)
        self.assertIn("D:2 | desconocida | 25.00", result.stdout)
        self.assertIn("Venta aceptada: D:3", result.stdout)
        self.assertEqual(original, legacy.read_bytes())
        restart = self.run_store()
        self.assert_ok(restart)
        self.assertIn("Ventas: 3 | Ingresos: 3550", restart.stdout)
        self.assertNotIn("convertido", restart.stderr)

    def test_corrupted_legacy_does_not_publish_csv(self):
        legacy = self.create_legacy()
        content = bytearray(legacy.read_bytes())
        content[80] ^= 1
        legacy.write_bytes(content)
        result = self.run_store()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.journal.exists())
        self.assertEqual(content, legacy.read_bytes())

    def test_partial_legacy_tail_keeps_backup(self):
        legacy = self.create_legacy()
        original = legacy.read_bytes() + b"SALEpartial"
        legacy.write_bytes(original)
        result = self.run_store("historial\n")
        self.assert_ok(result)
        self.assertIn("registro incompleto final omitido", result.stderr)
        self.assertIn("Ingresos: 3500", result.stdout)
        self.assertEqual(original, legacy.read_bytes())

    def test_missing_csv_does_not_restore_stale_backup(self):
        legacy = self.create_legacy()
        original = legacy.read_bytes()
        self.assert_ok(self.run_store("venta 50\n"))
        self.journal.unlink()
        result = self.run_store()
        self.assert_ok(result)
        self.assertIn("se creó un historial vacío", result.stderr)
        self.assertIn("Ventas: 0 | Ingresos: 0", result.stdout)
        self.assertEqual(original, legacy.read_bytes())
        with self.journal.open() as file:
            self.assertEqual(list(csv.DictReader(file)), [])
        restart = self.run_store("venta 50\n")
        self.assert_ok(restart)
        self.assertIn("Venta aceptada: D:1", restart.stdout)
        self.assertNotIn("convertido", restart.stderr)

    def test_missing_csv_without_backup_starts_empty_and_persists_new_sales(self):
        self.assert_ok(self.run_store("venta 1000\n"))
        self.journal.unlink()
        result = self.run_store("historial\nventa 250\nsalir\n")
        self.assert_ok(result)
        self.assertIn("Sin ventas registradas", result.stdout)
        self.assertIn("Venta aceptada: D:1", result.stdout)
        restart = self.run_store("venta 50\n")
        self.assert_ok(restart)
        self.assertIn("Venta aceptada: D:2", restart.stdout)
        self.assertIn("Ventas: 2 | Ingresos: 300", restart.stdout)

    def test_deleted_csv_resets_previously_closed_store(self):
        self.configure(limit=100)
        self.assert_ok(self.run_store("venta 100\n"))
        self.journal.unlink()
        result = self.run_store("venta 25\n")
        self.assert_ok(result)
        self.assertIn("Estado: ABIERTA", result.stdout)
        self.assertIn("Ventas: 1 | Ingresos: 25", result.stdout)


if __name__ == "__main__":
    unittest.main()
