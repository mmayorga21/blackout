"""Concurrencia local: orden, contrapresión y cierre sin más entrada del usuario."""
import csv
import os
from pathlib import Path
import selectors
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "bin/blackout"


class ConcurrentStore(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="blackout-concurrency-")
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.data = self.folder / "D"
        self.config = self.folder / "store.conf"
        self.configure()

    def configure(self, capacity=1, limit=100000):
        text = (ROOT / "config/tienda-d.conf").read_text()
        text = text.replace("data_dir=data/D", f"data_dir={self.data}")
        text = text.replace("queue_capacity=256", f"queue_capacity={capacity}")
        text = text.replace("limit_cent=100000", f"limit_cent={limit}")
        self.config.write_text(text)

    def run_store(self, commands):
        return subprocess.run([str(BINARY), "--config", str(self.config)], input=commands,
                              text=True, capture_output=True, timeout=15)

    def assert_ok(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def rows(self):
        with (self.data / "sales.csv").open() as file:
            return list(csv.DictReader(file))

    def start(self, fault=None, expected_state=b"Estado: ABIERTA"):
        env = os.environ.copy()
        if fault:
            env["LD_PRELOAD"] = "$ORIGIN/../build/fault_io.so"
            env["BLACKOUT_TEST_FAULT"] = fault
        process = subprocess.Popen([str(BINARY), "--config", str(self.config)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, bufsize=0, env=env)
        self.addCleanup(self.close_process, process)
        selector = selectors.DefaultSelector()
        try:
            selector.register(process.stdout, selectors.EVENT_READ)
            output = b""
            while expected_state not in output:
                self.assertTrue(selector.select(5), "No se abrió la tienda")
                chunk = os.read(process.stdout.fileno(), 4096)
                self.assertTrue(chunk)
                output += chunk
        finally:
            selector.close()
        return process

    @staticmethod
    def close_process(process):
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=5)

    def test_burst_with_capacity_one_and_fifo_queries(self):
        result = self.run_store("venta 1\n" * 100 + "estado\nhistorial\nventa 2\nsalir\nventa 999\n")
        self.assert_ok(result)
        rows = self.rows()
        self.assertEqual(len(rows), 101)
        self.assertEqual([r["identificador"] for r in rows], [f"D:{i}" for i in range(1, 102)])
        self.assertEqual(rows[-1]["valor_venta"], "0.02")
        before, after = result.stdout.split("Identificador | Fecha y hora", 1)
        self.assertIn("Ventas: 100 | Ingresos: 100", before)
        self.assertIn("D:100 |", after)
        self.assertLess(after.index("D:100 |"), after.index("Venta aceptada: D:101"))
        self.assertIn("Ventas: 101 | Ingresos: 102", after)

    def test_eof_drains_queue_and_handles_last_line_without_newline(self):
        result = self.run_store("venta 3\n" * 50 + "venta 7")
        self.assert_ok(result)
        self.assertEqual(len(self.rows()), 51)
        self.assertIn("Ventas: 51 | Ingresos: 157", result.stdout)
        self.assertIn("Fin de entrada", result.stdout)

    def read_until(self, process, marker):
        selector = selectors.DefaultSelector()
        output = b""
        deadline = time.monotonic() + 5
        try:
            selector.register(process.stdout, selectors.EVENT_READ)
            while marker not in output:
                remaining = deadline - time.monotonic()
                self.assertGreater(remaining, 0, output)
                self.assertTrue(selector.select(remaining), output)
                chunk = os.read(process.stdout.fileno(), 4096)
                self.assertTrue(chunk, "La tienda terminó antes de responder")
                output += chunk
        finally:
            selector.close()
        return output

    def test_limit_keeps_console_open_for_future_queries_until_manual_exit(self):
        self.configure(limit=100)
        process = self.start()
        process.stdin.write(b"venta 100\n")
        self.read_until(process, b"Estado: SOLO_CONSULTA")
        original = (self.data / "sales.csv").read_bytes()
        self.assertIsNone(process.poll())
        # Consultas enviadas después de confirmar el límite, sin cerrar stdin.
        process.stdin.write(b"historial\nestado\nventa 1\nhistorial\nayuda\n")
        out = self.read_until(process, b"venta CENTAVOS | estado | historial | ayuda | salir\n")
        self.assertEqual(out.count(b"D:1 |"), 2)
        self.assertIn(b"Estado: SOLO_CONSULTA", out)
        self.assertIsNone(process.poll())
        process.stdin.write(b"salir\n")
        process.wait(timeout=5)
        final, err = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, final + err)
        self.assertIn(b"Venta rechazada", err)
        self.assertIn(b"Salida ordenada", final)
        self.assertEqual((self.data / "sales.csv").read_bytes(), original)
        self.assertEqual(len(self.rows()), 1)

    def test_restart_at_limit_keeps_console_available(self):
        self.configure(limit=100)
        self.assert_ok(self.run_store("venta 100\nsalir\n"))
        process = self.start(expected_state=b"Estado: SOLO_CONSULTA")
        process.stdin.write(b"historial\nestado\nventa 1\nsalir\n")
        process.wait(timeout=5)
        out, err = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, out + err)
        self.assertIn(b"D:1 |", out)
        self.assertIn(b"Estado: SOLO_CONSULTA", out)
        self.assertIn(b"Venta rechazada", err)
        self.assertNotIn(b"Venta aceptada:", out)
        self.assertEqual(len(self.rows()), 1)

    def test_storage_failure_exits_with_input_still_open(self):
        self.assert_ok(self.run_store("salir\n"))
        process = self.start(fault="sync_error")
        process.stdin.write(b"venta 100\n")
        process.wait(timeout=5)
        out, err = process.communicate(timeout=5)
        self.assertNotEqual(process.returncode, 0)
        self.assertNotIn(b"Venta aceptada:", out)
        self.assertIn(b"Se detiene la instancia", err)

    def test_limit_with_many_pending_sales(self):
        self.configure(capacity=256, limit=10)
        result = self.run_store("venta 1\n" * 200)
        self.assert_ok(result)
        self.assertEqual(len(self.rows()), 10)
        self.assertEqual(result.stdout.count("Venta aceptada:"), 10)
        self.assertIn("Venta rechazada", result.stderr)

    def test_slow_disk_and_queue_backpressure_preserve_sales(self):
        self.assert_ok(self.run_store("salir\n"))
        env = os.environ.copy()
        env["LD_PRELOAD"] = "$ORIGIN/../build/fault_io.so"
        env["BLACKOUT_TEST_FAULT"] = "slow_sync"
        result = subprocess.run([str(BINARY), "--config", str(self.config)],
                                input="venta 1\n" * 30 + "estado\nsalir\n", text=True,
                                capture_output=True, env=env, timeout=10)
        self.assert_ok(result)
        self.assertEqual(len(self.rows()), 30)
        self.assertIn("Ventas: 30 | Ingresos: 30", result.stdout)

    def test_idle_two_threads_without_busy_waiting(self):
        process = self.start()
        # La línea inicial se imprime antes de pthread_create; esperar el segundo hilo.
        deadline = time.monotonic() + 3
        while len(list(Path(f"/proc/{process.pid}/task").iterdir())) != 2:
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.01)
        def cpu_ticks():
            stat = Path(f"/proc/{process.pid}/stat").read_text().split(")", 1)[1].split()
            return int(stat[11]) + int(stat[12])
        before = cpu_ticks()
        time.sleep(0.3)
        self.assertLessEqual(cpu_ticks() - before, 5)
        process.stdin.write(b"salir\n")
        process.wait(timeout=5)
        self.assertEqual(process.returncode, 0)

    def test_embedded_nul_does_not_turn_invalid_line_into_sale(self):
        result = self.run_store("venta 100\x00oculto\nventa 50\n")
        self.assert_ok(result)
        self.assertEqual(len(self.rows()), 1)
        self.assertEqual(self.rows()[0]["valor_venta"], "0.50")

    def test_queue_multiple_producers(self):
        result = subprocess.run([str(ROOT / "build/test_work_queue")],
                                text=True, capture_output=True, timeout=20)
        self.assert_ok(result)
        self.assertIn("16000 solicitudes", result.stdout)


if __name__ == "__main__":
    unittest.main()
