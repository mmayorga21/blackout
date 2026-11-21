"""Etapa 8: aislamiento, límite compartido, recuperación y cierre comprobado."""
import os
import socket
import subprocess
import time
import unittest
import zlib

from test_multi_store import StoreFixture, receive, routes
from test_network import BINARY, PREFIX, wire


class RecoveryTests(StoreFixture, unittest.TestCase):
    def command(self, name, command, expected):
        store = self.current[name]
        previous = len(store.output)
        store.send(command + "\n")
        self.until(lambda: expected.encode() in store.output[previous:])

    def closed(self, names):
        return all(b"Cierre de cadena: SINCRONIZADO_CONECTADAS" in self.status(name) for name in names)

    def test_disconnect_local_sales_and_bidirectional_reconnect(self):
        for name in "ABC": self.start(name)
        self.settle()
        self.command("C", "desconectar", "Red desactivada")
        for name, amount in (("A", 10), ("B", 20), ("C", 30)):
            self.current[name].send(f"venta {amount}\n")
        self.until(lambda: self.equal("AB", 2) and len(self.rows("C")) == 1)
        self.assertEqual(sum(float(row["valor_venta"]) for row in self.rows("C")), .30)
        self.assertIn(b"DESCONECTADA MANUALMENTE", self.status("C"))
        self.command("C", "conectar", "Red activada")
        self.settle()
        self.until(lambda: self.equal("ABC", 3))
        self.assertTrue(self.status("A").strip().endswith(b", C"))
        self.command("C", "conectar", "ya está activada")

    def test_shared_limit_control_immediate_sales_wait_five_seconds(self):
        for name in "ABC":
            self.configure(name, limit=100, delay=5000)
            self.start(name)
        self.settle()
        started = time.monotonic()
        self.current["A"].send("venta 100\n")
        self.until(lambda: b"Venta aceptada:" in self.current["A"].output)
        self.until(lambda: b"SOLO_CONSULTA" in self.status("B").splitlines()[0])
        self.assertLess(time.monotonic() - started, 3)
        self.assertEqual(self.rows("B"), [])
        self.assertIn(b"SINCRONIZANDO", self.status("B"))
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            self.assertEqual(self.rows("B"), [])
            self.assertNotIn(b"SINCRONIZADO_CONECTADAS", self.status("B"))
        self.until(lambda: self.equal("ABC", 1))
        self.assertGreaterEqual(time.monotonic() - started, 4.9)
        self.until(lambda: self.closed("ABC"))
        for name in "ABC":
            self.assertTrue((self.folder / name / "close.snapshot").exists())
            previous = len(self.current[name].errors)
            self.current[name].send("venta 1\nhistorial\n")
            self.until(lambda: b"Venta rechazada" in self.current[name].errors[previous:])
        self.assertTrue(self.equal("ABC", 1))

    def test_group_closes_without_absent_store_and_late_sales_are_kept(self):
        for name in "ABC":
            self.configure(name, limit=100)
            self.start(name)
        self.settle()
        self.command("C", "desconectar", "Red desactivada")
        self.current["C"].send("venta 40\n")
        self.until(lambda: len(self.rows("C")) == 1)
        self.current["A"].send("venta 100\n")
        self.until(lambda: self.equal("AB", 1))
        self.until(lambda: self.closed("AB"))
        self.assertIn("Recuperación pendiente con: C".encode(), self.status("A"))
        self.assertIn(b"ABIERTA", self.status("C").splitlines()[0])
        self.current["C"].send("venta 1\n")
        self.until(lambda: len(self.rows("C")) == 2)
        self.command("C", "conectar", "Red activada")
        self.settle(); self.until(lambda: self.equal("ABC", 3))
        self.until(lambda: self.closed("ABC"))
        for name in "ABC":
            self.assertIn(b"Ingresos conocidos de cadena: 141", self.status(name))

    def test_offline_state_and_pending_sales_survive_exit_and_restart(self):
        self.start("A"); self.start("B"); self.settle("AB")
        self.command("B", "desconectar", "Red desactivada")
        self.current["B"].send("venta 25\n")
        self.until(lambda: len(self.rows("B")) == 1)
        self.assertEqual(self.current["B"].stop(), 0)
        self.current["A"].send("venta 50\n")
        self.until(lambda: len(self.rows("A")) == 1)
        self.assertEqual(self.current["A"].stop(), 0)
        self.start("B")
        self.assertIn(b"DESCONECTADA MANUALMENTE", self.status("B"))
        self.command("B", "conectar", "Red activada")
        self.assertEqual(len(self.rows("B")), 1)  # A sigue apagada.
        self.start("A"); self.settle("AB")
        self.until(lambda: self.equal("AB", 2))

    def test_equal_totals_with_different_sales_require_exact_reconciliation(self):
        self.start("A"); self.start("B"); self.settle("AB")
        for name in "AB": self.command(name, "desconectar", "Red desactivada")
        for name in "AB": self.current[name].send("venta 25\n")
        self.until(lambda: all(len(self.rows(name)) == 1 for name in "AB"))
        self.assertNotEqual(self.rows("A")[0]["identificador"], self.rows("B")[0]["identificador"])
        self.command("A", "cerrar", "Cierre de cadena solicitado")
        for name in "AB": self.command(name, "conectar", "Red activada")
        self.settle("AB"); self.until(lambda: self.equal("AB", 2))
        self.until(lambda: self.closed("AB"))
        self.current["A"].stop(); self.start("A"); self.settle("AB")
        self.assertIn(b"SOLO_CONSULTA", self.status("A").splitlines()[0])

    @unittest.skipIf(os.environ.get("BLACKOUT_NETWORK_TEST_TSAN"), "El inyector de E/S se verifica con el binario normal")
    def test_write_ahead_route_recovers_interrupted_csv_without_new_id(self):
        self.start("B")
        a = self.start("A", fault="partial_exit"); self.settle("AB")
        a.send("venta 123\n")
        self.until(lambda: a.process.poll() is not None)
        self.assertEqual(a.stop(), 77)
        original = routes(self.folder / "A/routes.log")[0]
        self.assertEqual(len(self.rows("B")), 0)
        self.start("A"); self.settle("AB")
        self.until(lambda: self.equal("AB", 1))
        self.assertEqual(self.rows("A")[0]["identificador"], original["id"])
        self.assertEqual(routes(self.folder / "A/routes.log")[0], original)
        self.current["A"].send("venta 2\n")
        self.until(lambda: self.equal("AB", 2))
        self.assertTrue(self.rows("A")[-1]["identificador"].endswith(":2"))

    def test_orphaned_route_does_not_block_connected_group_closure(self):
        for name in "BC":
            self.configure(name, limit=100)
            self.start(name)
        self.settle("BC")
        identity = "A:" + "2" * 32 + ":1"
        csv = f"{identity},2026-11-21T10:00:00-0500,0.50"
        csv += f",{zlib.crc32(csv.encode()):08x}"
        prefix = f"R7|1|2|B,1,P;C,1,P|{csv}"
        proof = f"{prefix}|{zlib.crc32(prefix.encode()):08x}\n".encode()
        with socket.create_connection(("127.0.0.1", self.ports["B"]), timeout=3) as sock:
            sock.sendall(wire(1, f"X A 100 127.0.0.1 {self.ports['A']} 1 1 0".encode()))
            sock.sendall(wire(2, proof))
            while True:
                kind, body = receive(sock)
                if kind == 4: sock.sendall(wire(5))
                if kind == 3:
                    self.assertEqual(body, identity.encode()); break
        self.until(lambda: len(self.rows("B")) == 1)
        self.assertEqual(self.rows("C"), [])
        self.current["C"].send("venta 100\n")
        self.until(lambda: self.equal("BC", 2))
        self.until(lambda: self.closed("BC"))
        self.assertIn("Recuperación pendiente con: A".encode(), self.status("B"))

    def test_manual_close_and_state_corruption_never_reopen_sales(self):
        self.configure("A", seeds="A")
        self.start("A")
        self.command("A", "cerrar", "Cierre de cadena solicitado")
        self.until(lambda: self.closed("A"))
        self.current["A"].stop()
        state = self.folder / "A/chain.state"
        state.write_bytes(state.read_bytes() + b"interrumpido")
        self.start("A")
        self.assertIn(b"SOLO_CONSULTA", self.status("A").splitlines()[0])
        self.current["A"].stop()
        original = state.read_bytes(); damaged = bytes([original[0] ^ 1]) + original[1:]
        state.write_bytes(damaged)
        result = subprocess.run(PREFIX + [str(BINARY), "--config", str(self.folder / "A.conf")], input=b"", capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(state.read_bytes(), damaged)

    def test_paged_cut_with_many_sales_and_rejoining_member(self):
        for name in "ABC": self.start(name)
        self.settle()
        self.current["A"].send("venta 1\n" * 17)
        self.current["B"].send("venta 2\n" * 13)
        self.until(lambda: self.equal("ABC", 30))
        self.command("A", "cerrar", "Cierre de cadena solicitado")
        self.until(lambda: self.closed("ABC"))
        before = {row["identificador"] for row in self.rows("A")}
        self.current["B"].stop(kill=True)
        self.until(lambda: self.closed("AC"))
        self.start("B"); self.settle()
        self.until(lambda: self.closed("ABC"))
        self.assertEqual(before, {row["identificador"] for row in self.rows("B")})
        self.assertIn(b"SOLO_CONSULTA", self.status("B").splitlines()[0])

    @unittest.skipIf(os.environ.get("BLACKOUT_NETWORK_TEST_TSAN"), "Inyector de E/S del binario normal")
    def test_route_write_failures_never_confirm_and_replay_only_complete_intents(self):
        modes = (("route_partial_exit", 0), ("route_full_exit", 1), ("route_enospc", 0), ("route_sync_error", 1))
        for name, (mode, recovered) in zip("ABCD", modes):
            with self.subTest(mode=mode):
                self.configure(name, seeds=name)
                store = self.start(name, fault=mode)
                store.send("venta 7\n")
                self.until(lambda: store.process.poll() is not None)
                self.assertNotEqual(store.stop(), 0)
                self.assertNotIn(b"Venta aceptada:", store.output)
                self.start(name)
                self.assertEqual(len(self.rows(name)), recovered)
                self.current[name].send("venta 3\n")
                self.until(lambda: len(self.rows(name)) == recovered + 1)
                ids = [row["identificador"] for row in self.rows(name)]
                self.assertEqual(len(ids), len(set(ids)))

    @unittest.skipIf(os.environ.get("BLACKOUT_NETWORK_TEST_TSAN"), "Inyector de E/S del binario normal")
    def test_certificate_sync_failure_never_reports_success(self):
        self.configure("A", seeds="A", limit=10)
        store = self.start("A", fault="certificate_sync_error")
        store.send("venta 10\n")
        self.until(lambda: store.process.poll() is not None)
        self.assertNotEqual(store.stop(), 0)
        self.assertNotIn(b"Cierre sincronizado", store.output + store.errors)
        self.assertEqual(len(self.rows("A")), 1)
        self.assertFalse((self.folder / "A/close.snapshot").exists())
        self.start("A")
        self.until(lambda: self.closed("A"))
        self.assertEqual(len(self.rows("A")), 1)

    def test_complete_inventory_avoids_resending_known_sales(self):
        self.start("A")
        self.current["A"].send("venta 10\n")
        self.until(lambda: len(self.rows("A")) == 1)
        csv_line = (self.folder / "A/sales.csv").read_bytes().splitlines(keepends=True)[1]
        with socket.create_connection(("127.0.0.1", self.ports["A"]), timeout=3) as sock:
            hello = f"X B 100000 127.0.0.1 {self.ports['B']} 1 10 0".encode()
            inventory = b"1 1 0 0 A:1;B:1\n" + csv_line
            sock.sendall(wire(1, hello) + wire(13, inventory))
            sock.settimeout(.1)
            deadline = time.monotonic() + .6
            seen = []
            while time.monotonic() < deadline:
                try:
                    kind, body = receive(sock)
                except socket.timeout:
                    continue
                seen.append(kind)
                if kind == 4: sock.sendall(wire(5))
            self.assertNotIn(2, seen)
            self.assertNotIn(9, seen)
            self.assertIn(1, seen)

    def test_inventory_does_not_expose_new_sale_during_share_wait(self):
        self.configure("A", delay=5000)
        self.start("A")
        with socket.create_connection(("127.0.0.1", self.ports["A"]), timeout=3) as sock:
            hello = f"X B 100000 127.0.0.1 {self.ports['B']} 1 10 0".encode()
            sock.sendall(wire(1, hello))
            self.until(lambda: b"Red: CONECTADA" in self.status("A"))
            self.current["A"].send("venta 10\n")
            self.until(lambda: len(self.rows("A")) == 1)
            identity = self.rows("A")[0]["identificador"].encode()
            sock.settimeout(.1)
            deadline = time.monotonic() + 1
            while time.monotonic() < deadline:
                try:
                    kind, body = receive(sock)
                except socket.timeout:
                    continue
                self.assertNotIn(identity, body)
                self.assertNotIn(kind, (2, 9, 14))
                if kind == 4: sock.sendall(wire(5))

    def test_passive_copy_can_close_without_waiting_for_any_online_source(self):
        self.start("C")
        identity = "A:" + "3" * 32 + ":1"
        csv = f"{identity},2026-11-21T10:00:00-0500,0.50"
        csv += f",{zlib.crc32(csv.encode()):08x}"
        prefix = f"R7|1|2|B,1,P;C,1,P|{csv}"
        proof = f"{prefix}|{zlib.crc32(prefix.encode()):08x}\n".encode()
        with socket.create_connection(("127.0.0.1", self.ports["C"]), timeout=3) as sock:
            sock.sendall(wire(1, f"X B 100000 127.0.0.1 {self.ports['B']} 1 2 0".encode()) + wire(9, proof))
            while True:
                kind, body = receive(sock)
                if kind == 4: sock.sendall(wire(5))
                if kind == 10:
                    self.assertEqual(body, identity.encode()); break
        self.assertEqual(self.rows("C"), [])
        self.command("C", "cerrar", "Cierre de cadena solicitado")
        self.until(lambda: len(self.rows("C")) == 1)
        self.until(lambda: self.closed("C"))
        self.assertEqual(self.rows("C")[0]["identificador"], identity)

    def test_previous_wire_version_is_rejected_without_affecting_sales(self):
        self.start("A")
        with socket.create_connection(("127.0.0.1", self.ports["A"]), timeout=3) as sock:
            packet = bytearray(wire(1, b"X B 100000"))
            packet[4] = 1
            sock.sendall(packet)
            self.until(lambda: b"Mensaje de red rechazado" in self.current["A"].errors)
        self.assertEqual(self.rows("A"), [])
        self.current["A"].send("venta 1\n")
        self.until(lambda: len(self.rows("A")) == 1)
        self.assertIsNone(self.current["A"].process.poll())


if __name__ == "__main__": unittest.main()
