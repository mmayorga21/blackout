"""Consultas de solo lectura, coherencia y caché durable del gobierno."""
import csv
import os
from pathlib import Path
import socket
import struct
import subprocess
import threading
import time
import unittest
import zlib

from test_multi_store import StoreFixture
from test_network import BINARY, PREFIX, ROOT


def frame(body):
    return struct.pack("!4sI", b"BQ09", len(body)) + body


def receive(sock):
    def exact(length):
        result = b""
        while len(result) < length:
            part = sock.recv(length - len(result))
            if not part: raise EOFError
            result += part
        return result
    magic, length = struct.unpack("!4sI", exact(8))
    if magic != b"BQ09" or not 0 < length < 4096: raise ValueError
    body = exact(length)
    prefix, checksum = body.rstrip(b"\n").rsplit(b"|", 1)
    if f"{zlib.crc32(prefix):08x}".encode() != checksum: raise ValueError
    return prefix.decode().split("|")


class ControlledServer:
    def __init__(self, callback, port=0):
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", port)); self.port = self.listener.getsockname()[1]
        self.listener.listen(); self.listener.settimeout(.1)
        self.callback = callback; self.stopping = threading.Event(); self.clients = []
        self.thread = threading.Thread(target=self.loop, daemon=True); self.thread.start()

    def loop(self):
        while not self.stopping.is_set():
            try: sock, _ = self.listener.accept()
            except socket.timeout: continue
            except OSError: return
            self.clients.append(sock)
            try: self.callback(sock, self.stopping)
            except (OSError, EOFError): pass
            finally: sock.close()

    def close(self):
        self.stopping.set(); self.listener.close()
        for client in self.clients:
            try: client.shutdown(socket.SHUT_RDWR)
            except OSError: pass
        self.thread.join(timeout=3)


class GovernmentTests(StoreFixture, unittest.TestCase):
    def setUp(self):
        super().setUp()
        sockets = [socket.socket() for _ in "ABCD"]
        for sock in sockets: sock.bind(("127.0.0.1", 0))
        self.query_ports = dict(zip("ABCD", [sock.getsockname()[1] for sock in sockets]))
        for sock in sockets: sock.close()
        for name in "ABC": self.enable_query(name)
        text = (ROOT / "config/tienda-d.conf").read_text()
        text = text.replace("data_dir=data/D", f"data_dir={self.folder}/D").replace("query_port=6104", f"query_port={self.query_ports['D']}")
        (self.folder / "D.conf").write_text(text)
        self.government_config("ABCD")

    def enable_query(self, name):
        path = self.folder / f"{name}.conf"
        path.write_text(path.read_text() + f"query_port={self.query_ports[name]}\n")

    def government_config(self, names="ABCD", timeout=1000):
        self.catalog = self.folder / "government.stores"
        self.catalog.write_text("".join(f"{name} {'none' if name == 'D' else 'X'} 127.0.0.1 {self.query_ports[name]}\n" for name in names))
        self.govconfig = self.folder / "government.conf"
        self.govconfig.write_text(f"stores_file={self.catalog}\ndata_dir={self.folder}/government\ntimeout_ms={timeout}\n")

    def government(self, commands, fault=None):
        env = os.environ.copy()
        if fault:
            env["LD_PRELOAD"] = "$ORIGIN/../build/fault_io.so"
            env["BLACKOUT_TEST_FAULT"] = fault
        args = PREFIX + [str(BINARY), "--gobierno", str(self.govconfig)]
        process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        pending = commands.encode(); deadline = time.monotonic() + 20
        try:
            while True:
                try:
                    out, err = process.communicate(input=pending, timeout=.03)
                    return subprocess.CompletedProcess(args, process.returncode, out, err)
                except subprocess.TimeoutExpired:
                    pending = None
                    for store in self.running:
                        if not store.closed: store.pump(.001)
                    if time.monotonic() > deadline: self.fail("El consultor no terminó")
        finally:
            if process.poll() is None:
                process.kill(); process.communicate(timeout=5)

    def direct(self, name, pieces=False):
        with socket.create_connection(("127.0.0.1", self.query_ports[name]), timeout=3) as sock:
            wire = frame(b"CONSULTAR\n")
            if pieces:
                for part in (wire[:3], wire[3:9], wire[9:]): sock.sendall(part)
            else: sock.sendall(wire)
            return receive(sock)

    def command(self, name, command, marker):
        store = self.current[name]; previous = len(store.output)
        store.send(command + "\n")
        self.until(lambda: marker.encode() in store.output[previous:])

    def test_independent_live_query_and_history_after_shutdown(self):
        config = self.folder / "D.conf"
        config.write_text(config.read_text().replace("limit_cent=100000", "limit_cent=1000"))
        self.start("D"); self.current["D"].send("venta 1250\n")
        self.until(lambda: len(self.rows("D")) == 1)
        before = (self.folder / "D/sales.csv").read_bytes()
        result = self.government("consultar D\nsalir\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"RESPUESTA ACTUAL", result.stdout)
        self.assertIn(b"Ingresos propios: 1250", result.stdout)
        self.assertNotIn(b"Ingresos conocidos de cadena:", result.stdout)
        self.assertIn(b"Estado comercial informado: SOLO_CONSULTA", result.stdout)
        self.assertEqual((self.folder / "D/sales.csv").read_bytes(), before)
        self.current["D"].stop()
        result = self.government("consultar D\nsalir\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"SIN RESPUESTA", result.stdout)
        self.assertIn(b"INFORMACION HISTORICA", result.stdout)
        self.assertIn("Antigüedad aproximada:".encode(), result.stdout)
        self.assertNotIn(b"RESPUESTA ACTUAL", result.stdout)
        self.assertIn(b"Ingresos propios: 1250", result.stdout)

    def test_chain_queries_do_not_join_membership_or_change_sales(self):
        for name in "ABC": self.start(name)
        self.settle()
        self.current["A"].send("venta 100\n"); self.current["B"].send("venta 200\n")
        self.until(lambda: self.equal("ABC", 2))
        before = {name: (self.folder / name / "sales.csv").read_bytes() for name in "ABC"}
        members = {name: (self.folder / name / "membership.log").read_bytes() for name in "ABC"}
        result = self.government("consultar todas\nsalir\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count(b"RESPUESTA ACTUAL"), 3)
        self.assertEqual(result.stdout.count(b"Ingresos conocidos de cadena: 300"), 3)
        self.assertIn(b"Tienda D: SIN RESPUESTA", result.stdout)
        for name in "ABC":
            self.assertEqual((self.folder / name / "sales.csv").read_bytes(), before[name])
            self.assertEqual((self.folder / name / "membership.log").read_bytes(), members[name])

    def test_isolated_chain_store_remains_queryable_and_reports_partial_view(self):
        for name in "AB": self.start(name)
        self.settle("AB")
        self.command("B", "desconectar", "Red desactivada")
        self.current["A"].send("venta 100\n"); self.current["B"].send("venta 20\n")
        self.until(lambda: len(self.rows("A")) == len(self.rows("B")) == 1)
        result = self.government("consultar B\nsalir\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"RESPUESTA ACTUAL", result.stdout)
        self.assertIn(b"Red de cadena: AISLADA", result.stdout)
        self.assertIn(b"Ingresos conocidos de cadena: 20", result.stdout)
        self.assertIn("Recuperación pendiente con: A".encode(), result.stdout)
        self.command("B", "conectar", "Red activada")
        self.settle("AB"); self.until(lambda: self.equal("AB", 2))
        result = self.government("consultar B\nsalir\n")
        self.assertIn(b"Ingresos conocidos de cadena: 120", result.stdout)

    def test_readonly_and_confirmed_group_closure_are_queryable(self):
        for name in "AB":
            self.configure(name, seeds="AB", limit=100)
            self.enable_query(name); self.start(name)
        self.settle("AB")
        self.current["A"].send("venta 100\n")
        self.until(lambda: self.equal("AB", 1))
        self.until(lambda: all(b"SINCRONIZADO_CONECTADAS" in self.status(name) for name in "AB"))
        result = self.government("consultar A\nconsultar B\nsalir\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count(b"Estado comercial informado: SOLO_CONSULTA"), 2)
        self.assertEqual(result.stdout.count(b"Cierre informado: SINCRONIZADO_CONECTADAS"), 2)
        self.assertEqual(result.stdout.count("Límite compartido de cadena: 100".encode()), 2)

    def test_queries_observe_consistent_counts_during_sales(self):
        path = self.folder / "D.conf"
        path.write_text(path.read_text().replace("timeout_ms=2000", "timeout_ms=5000"))
        self.government_config(timeout=5000)
        self.start("D")
        def producer():
            for _ in range(40):
                self.current["D"].send("venta 3\n"); time.sleep(.02)
        thread = threading.Thread(target=producer); thread.start()
        try:
            result = self.government("consultar D\n" * 15 + "salir\n")
        finally: thread.join(timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        path = self.folder / "government/consultas.log"
        records = [line.split("|Q9|", 1)[1].split("|") for line in path.read_text().splitlines()]
        self.assertEqual(len(records), 15, result.stdout + result.stderr)
        counts = []
        for record in records:
            count, own, known = map(int, record[3:6])
            self.assertEqual(own, count * 3); self.assertEqual(known, own); counts.append(count)
        self.assertEqual(counts, sorted(counts))
        self.until(lambda: len(self.rows("D")) == 40)

    def test_fragmented_request_and_invalid_or_mutating_requests(self):
        self.start("D")
        self.assertEqual(self.direct("D", pieces=True)[1:3], ["D", "none"])
        before = (self.folder / "D/sales.csv").read_bytes()
        for packet in (frame(b"venta 100\n"), b"XXXX" + struct.pack("!I", 10) + b"CONSULTAR\n",
                       b"BQ09" + struct.pack("!I", 999999), frame(b"CONSULTAR\0")):
            with socket.create_connection(("127.0.0.1", self.query_ports["D"]), timeout=3) as sock:
                sock.sendall(packet)
                try: self.assertEqual(sock.recv(128), b"")
                except ConnectionResetError: pass
        self.assertEqual(self.direct("D")[4], "0")
        self.assertEqual((self.folder / "D/sales.csv").read_bytes(), before)

    def test_cache_partial_tail_recovery_and_complete_corruption_rejected(self):
        self.start("D")
        self.assertEqual(self.government("consultar D\nsalir\n").returncode, 0)
        self.current["D"].stop()
        path = self.folder / "government/consultas.log"; original = path.read_bytes()
        path.write_bytes(original + b"G9|interrumpido")
        result = self.government("consultar D\nsalir\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"INFORMACION HISTORICA", result.stdout)
        self.assertEqual(path.read_bytes(), original)
        damaged = bytes([original[0] ^ 1]) + original[1:]; path.write_bytes(damaged)
        result = self.government("consultar D\nsalir\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(path.read_bytes(), damaged)

    def test_old_endpoint_cache_is_not_misattributed_to_new_endpoint(self):
        self.start("D")
        self.assertEqual(self.government("consultar D\nsalir\n").returncode, 0)
        self.current["D"].stop()
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0)); newport = sock.getsockname()[1]
        self.catalog.write_text(f"D none 127.0.0.1 {newport}\n")
        result = self.government("consultar D\nsalir\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(b"INFORMACION HISTORICA", result.stdout)

    def test_wrong_identity_or_checksum_never_overwrites_cache(self):
        self.start("D")
        self.assertEqual(self.government("consultar D\nsalir\n").returncode, 0)
        self.current["D"].stop()
        path = self.folder / "government/consultas.log"; original = path.read_bytes()
        calls = []
        def respond(sock, stop):
            sock.recv(128); calls.append(True)
            prefix = b"Q9|X|none|0|0|0|0|100000|0|0|.|." if len(calls) == 1 else b"Q9|D|none|0|0|0|0|100000|0|0|.|."
            checksum = zlib.crc32(prefix) if len(calls) == 1 else zlib.crc32(prefix) ^ 1
            sock.sendall(frame(prefix + f"|{checksum:08x}\n".encode()))
        server = ControlledServer(respond, self.query_ports["D"]); self.addCleanup(server.close)
        result = self.government("consultar D\nconsultar D\nsalir\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count(b"RESPUESTA INVALIDA"), 2)
        self.assertIn(b"INFORMACION HISTORICA", result.stdout)
        self.assertNotIn(b"RESPUESTA ACTUAL", result.stdout)
        self.assertEqual(path.read_bytes(), original)

    def test_parallel_queries_do_not_multiply_timeout_by_store_count(self):
        arrivals = []
        def stall(sock, stop):
            sock.recv(128); arrivals.append(time.monotonic()); stop.wait(2)
        servers = [ControlledServer(stall) for _ in range(3)]
        for server in servers: self.addCleanup(server.close)
        self.catalog.write_text("".join(f"{name} X 127.0.0.1 {server.port}\n" for name, server in zip("ABC", servers)))
        self.govconfig.write_text(self.govconfig.read_text().replace("timeout_ms=1000", "timeout_ms=300"))
        started = time.monotonic(); result = self.government("consultar todas\nsalir\n")
        elapsed = time.monotonic() - started
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count(b"SIN RESPUESTA"), 3)
        self.assertEqual(len(arrivals), 3)
        self.assertLess(max(arrivals) - min(arrivals), .25)
        self.assertLess(elapsed, 3)

    def test_cache_lock_and_invalid_configuration(self):
        process = subprocess.Popen(PREFIX + [str(BINARY), "--gobierno", str(self.govconfig)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            import selectors
            selector = selectors.DefaultSelector(); selector.register(process.stdout, selectors.EVENT_READ)
            ready = b""; deadline = time.monotonic() + 5
            try:
                while b"Tiendas configuradas:" not in ready:
                    self.assertLess(time.monotonic(), deadline)
                    if selector.select(.1):
                        chunk = os.read(process.stdout.fileno(), 65536)
                        self.assertTrue(chunk, "El primer consultor terminó antes de iniciar")
                        ready += chunk
            finally: selector.close()
            result = self.government("salir\n")
            self.assertNotEqual(result.returncode, 0)
        finally:
            process.communicate(b"salir\n", timeout=5)
        original = self.govconfig.read_text()
        for extra in ("timeout_ms=3\n", "unknown=1\n"):
            self.govconfig.write_text(original + extra)
            self.assertNotEqual(self.government("salir\n").returncode, 0)

    def test_slow_clients_do_not_block_queries_or_sales(self):
        self.start("D")
        clients = []
        try:
            for _ in range(8):
                sock = socket.create_connection(("127.0.0.1", self.query_ports["D"]), timeout=3)
                clients.append(sock); sock.sendall(b"BQ")
            self.current["D"].send("venta 5\n")
            self.until(lambda: len(self.rows("D")) == 1)
            result = self.government("consultar D\nsalir\n")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(b"RESPUESTA ACTUAL", result.stdout)
            self.assertIn(b"Ingresos propios: 5", result.stdout)
        finally:
            for sock in clients: sock.close()
        self.assertEqual(self.direct("D")[5], "5")

    @unittest.skipIf(os.environ.get("BLACKOUT_NETWORK_TEST_TSAN"), "Inyector de E/S del binario normal")
    def test_abandoned_query_reply_cannot_reach_reused_connection(self):
        config = self.folder / "D.conf"
        config.write_text(config.read_text().replace("timeout_ms=2000", "timeout_ms=100"))
        self.start("D", fault="slow_sync")
        self.current["D"].send("venta 1\n" * 20)
        sock = socket.create_connection(("127.0.0.1", self.query_ports["D"]), timeout=3)
        sock.sendall(frame(b"CONSULTAR\n"))
        deadline = time.monotonic() + .15
        self.until(lambda: time.monotonic() > deadline)
        sock.close()
        self.current["D"].send("venta 9\n")
        self.until(lambda: len(self.rows("D")) == 21)
        reply = self.direct("D")
        self.assertEqual(reply[4:7], ["21", "29", "29"])

    def test_query_port_validation_and_busy_listener(self):
        config = self.folder / "D.conf"; original = config.read_text()
        for value in ("-1", "65536", "1.5", "none", "5104"):
            config.write_text(original.replace(f"query_port={self.query_ports['D']}", f"query_port={value}"))
            result = subprocess.run(PREFIX + [str(BINARY), "--config", str(config)], input=b"q\n", capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((self.folder / "D/sales.csv").exists())
        config.write_text(original)
        with socket.socket() as busy:
            busy.bind(("127.0.0.1", self.query_ports["D"])); busy.listen()
            result = subprocess.run(PREFIX + [str(BINARY), "--config", str(config)], input=b"salir\n", capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"Iniciar consultas de gobierno", result.stderr)

    @unittest.skipIf(os.environ.get("BLACKOUT_NETWORK_TEST_TSAN"), "Inyector de E/S del binario normal")
    def test_cache_write_failures_do_not_affect_store(self):
        self.start("D"); self.current["D"].send("venta 8\n")
        self.until(lambda: len(self.rows("D")) == 1)
        original = (self.folder / "D/sales.csv").read_bytes()
        for mode in ("cache_enospc", "cache_sync_error"):
            with self.subTest(mode=mode):
                result = self.government("consultar D\nsalir\n", fault=mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b"Guardar historial de consultas", result.stderr)
                self.assertEqual((self.folder / "D/sales.csv").read_bytes(), original)
                self.assertEqual(self.direct("D")[5], "8")
        self.assertIsNone(self.current["D"].process.poll())

    def test_fragmented_reply_and_oversized_reply_preserve_last_valid_observation(self):
        calls = []
        def respond(sock, stop):
            sock.recv(128); calls.append(True)
            if len(calls) == 2:
                sock.sendall(b"BQ09" + struct.pack("!I", 999999)); return
            prefix = b"Q9|D|none|0|1|12|12|100000|0|0|.|."
            packet = frame(prefix + f"|{zlib.crc32(prefix):08x}\n".encode())
            for piece in (packet[:3], packet[3:11], packet[11:20], packet[20:]):
                sock.sendall(piece); time.sleep(.002)
        server = ControlledServer(respond); self.addCleanup(server.close)
        self.catalog.write_text(f"D none 127.0.0.1 {server.port}\n")
        result = self.government("consultar D\nconsultar D\nsalir\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count(b"RESPUESTA ACTUAL"), 1)
        self.assertEqual(result.stdout.count(b"RESPUESTA INVALIDA"), 1)
        self.assertEqual(result.stdout.count(b"Ingresos propios: 12"), 2)
        self.assertIn(b"INFORMACION HISTORICA", result.stdout)
        self.assertEqual(len((self.folder / "government/consultas.log").read_bytes().splitlines()), 1)


if __name__ == "__main__": unittest.main()
