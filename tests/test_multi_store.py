"""Recorridos, incorporaciones y recuperación con tres o más procesos."""
import csv
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
import zlib
from pathlib import Path

from test_network import BINARY, PREFIX, RunningStore, ROOT, wire


def receive(sock):
    def exact(n):
        data = b""
        while len(data) < n:
            part = sock.recv(n - len(data))
            if not part:
                raise EOFError
            data += part
        return data
    magic, version, kind, reserved, length = struct.unpack("!4sBBHI", exact(12))
    if magic != b"BLK6" or version != 1 or reserved or length >= 4096:
        raise ValueError("Mensaje inválido")
    return kind, exact(length)


def routes(path):
    result = []
    if not path.exists():
        return result
    for line in path.read_bytes().splitlines(keepends=True):
        if not line.endswith(b"\n"):
            continue
        line = line.rstrip(b"\n")
        prefix, crc = line.rsplit(b"|", 1)
        if f"{zlib.crc32(prefix):08x}".encode() != crc:
            continue  # El proceso puede estar terminando de escribir esta fila.
        magic, version, count, targets, sale = prefix.decode().split("|")
        assert magic == "R7"
        result.append({"version": int(version), "targets": [x.split(",") for x in targets.split(";") if x],
                       "id": sale.split(",")[0], "count": int(count)})
    return result


class HoldingPeer:
    """Par A que registra la entrega en memoria y retiene su ACK hasta una señal."""
    def __init__(self, port):
        self.port = port
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", port))
        self.listener.listen()
        self.listener.settimeout(0.1)
        self.release = threading.Event()
        self.received = threading.Event()
        self.stopping = threading.Event()
        self.sockets = []
        self.threads = []
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        while not self.stopping.is_set():
            try:
                sock, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self.sockets.append(sock)
            thread = threading.Thread(target=self.handle, args=(sock,), daemon=True)
            self.threads.append(thread)
            thread.start()

    def handle(self, sock):
        try:
            sock.sendall(wire(1, f"X A 100000 127.0.0.1 {self.port} 1 1 0".encode()))
            while not self.stopping.is_set():
                kind, body = receive(sock)
                if kind == 2:
                    self.received.set()
                    while not self.release.wait(0.02):
                        if self.stopping.is_set():
                            return
                    identity = body.split(b"|")[4].split(b",")[0]
                    sock.sendall(wire(3, identity))
                elif kind == 9:
                    sock.sendall(wire(3, body.split(b"|")[4].split(b",")[0]))
                elif kind == 4:
                    sock.sendall(wire(5))
        except (OSError, EOFError, ValueError):
            pass

    def close(self):
        self.stopping.set()
        self.release.set()
        self.listener.close()
        for sock in self.sockets:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()
        self.thread.join(timeout=2)
        for thread in self.threads:
            thread.join(timeout=2)


class MultipleStores(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="blackout-multi-")
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.running = []
        self.current = {}
        sockets = [socket.socket() for _ in "ABCD"]
        for sock in sockets:
            sock.bind(("127.0.0.1", 0))
        self.ports = dict(zip("ABCD", [sock.getsockname()[1] for sock in sockets]))
        for sock in sockets:
            sock.close()
        for name in "ABCD":
            self.configure(name)

    def configure(self, name, seeds="ABC", delay=0, limit=100000, timeout=1000):
        catalog = self.folder / f"{name}.peers"
        catalog.write_text("".join(f"{peer} 127.0.0.1 {self.ports[peer]}\n" for peer in seeds))
        text = (ROOT / "config/demo-dos/tienda-a.conf").read_text()
        for old, new in [("store_id=A", f"store_id={name}"), ("port=5101", f"port={self.ports[name]}"),
                         ("timeout_ms=2000", f"timeout_ms={timeout}"), ("sale_share_delay_ms=5000", f"sale_share_delay_ms={delay}"),
                         ("queue_capacity=256", "queue_capacity=4"), ("limit_cent=100000", f"limit_cent={limit}"),
                         ("data_dir=data/demo-dos/A", f"data_dir={self.folder}/{name}"),
                         ("peers_file=config/demo-dos/cadena-x.peers", f"peers_file={catalog}")]:
            text = text.replace(old, new)
        path = self.folder / f"{name}.conf"
        path.write_text(text)

    def start(self, name):
        store = RunningStore(self.folder / f"{name}.conf")
        self.running.append(store)
        self.current[name] = store
        self.addCleanup(lambda: not store.closed and store.stop(kill=True))
        self.until(lambda: b"Estado:" in store.output)
        return store

    def until(self, condition, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for store in self.running:
                if not store.closed:
                    store.pump(0.005)
            if condition():
                return
        self.fail("Tiempo agotado:\n" + "\n".join((s.output + s.errors).decode(errors="replace") for s in self.running))

    def rows(self, name):
        path = self.folder / name / "sales.csv"
        if not path.exists():
            return []
        with path.open() as file:
            values = list(csv.DictReader(file))
        return values if all(row.get("crc32") and len(row["crc32"]) == 8 for row in values) else []

    def status(self, name):
        store = self.current[name]
        marker = "Orden de incorporación conocido:".encode()
        previous = store.output.count(marker)
        store.send("estado\n")
        self.until(lambda: store.output.count(marker) > previous)
        return store.output.rsplit(b"Estado:", 1)[1]

    def settle(self, names="ABC", expected=None):
        required = len(names) - 1 if expected is None else expected
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if all(self.status(name).count(b"Red: CONECTADA") >= required for name in names):
                return
        self.fail("No se estableció la malla de conexiones")

    def equal(self, names, count):
        values = [self.rows(name) for name in names]
        return all(len(rows_) == count for rows_ in values) and all(
            {tuple(row.items()) for row in rows_} == {tuple(row.items()) for row in values[0]} for rows_ in values)

    def test_origin_first_then_incorporation_order(self):
        self.start("A"); self.start("B"); self.settle("AB")
        self.start("C"); self.settle()
        self.assertIn(b"A, B, C", self.status("A"))
        self.current["B"].send("venta 100\n")
        self.until(lambda: self.equal("ABC", 1))
        history = routes(self.folder / "B/routes.log")
        self.assertEqual([target[0] for target in history[0]["targets"]], ["A", "C"])
        self.until(lambda: routes(self.folder / "B/routes.log")[-1]["targets"][-1][2] == "A")
        states = ["".join(t[2] for t in row["targets"]) for row in routes(self.folder / "B/routes.log")]
        self.assertEqual(states, ["PP", "AP", "AA"])

    def test_later_store_waits_for_previous_registration_ack(self):
        fake = HoldingPeer(self.ports["A"])
        self.addCleanup(fake.close)
        for name in "BC":
            self.configure(name, timeout=3000)
            self.start(name)
        self.settle("BC", expected=2)
        self.current["B"].send("venta 100\n")
        self.until(fake.received.is_set)
        deadline = time.monotonic() + 0.3
        self.until(lambda: time.monotonic() >= deadline)
        self.assertEqual(self.rows("C"), [])
        fake.release.set()
        self.until(lambda: len(self.rows("C")) == 1)

    def test_recovery_cannot_bypass_an_unresolved_original_turn(self):
        self.start("C")
        identity = "A:" + "1" * 32 + ":1"
        sale = f"{identity},2026-11-20T12:30:00-0500,1.00"
        sale += f",{zlib.crc32(sale.encode()):08x}"

        def proof(version, states):
            prefix = f"R7|{version}|2|B,1,{states[0]};C,1,{states[1]}|{sale}"
            return f"{prefix}|{zlib.crc32(prefix.encode()):08x}\n".encode()

        def listening():
            try:
                with socket.create_connection(("127.0.0.1", self.ports["C"]), timeout=0.1):
                    return True
            except OSError:
                return False

        self.until(listening)
        with socket.create_connection(("127.0.0.1", self.ports["C"]), timeout=3) as sock:
            sock.sendall(wire(1, f"X A 100000 127.0.0.1 {self.ports['A']} 1 1 0".encode()))

            def reply():
                while True:
                    kind, body = receive(sock)
                    if kind == 4:
                        sock.sendall(wire(5))
                    if kind in (3, 6, 10):
                        return kind, body

            sock.sendall(wire(9, proof(1, "PP")))
            self.assertEqual(reply(), (10, identity.encode()))
            self.assertEqual(self.rows("C"), [])
            # La prueba durable del ACK de B habilita el turno de C.
            sock.sendall(wire(2, proof(2, "AP")))
            self.assertEqual(reply(), (3, identity.encode()))
            self.assertEqual(len(self.rows("C")), 1)
            sock.sendall(wire(2, proof(2, "AP")))
            self.assertEqual(reply(), (3, identity.encode()))
            self.assertEqual(len(self.rows("C")), 1)

    def test_skip_unavailable_then_rejoin_last_and_recover(self):
        self.start("A"); self.start("B"); self.settle("AB")
        self.start("C"); self.settle()
        self.configure("A", delay=300)
        # Reabrir A con la espera permite apagar B después de aceptar la venta.
        self.current["A"].stop(); self.start("A"); self.settle()
        # En este orden B precede a C dentro del recorrido originado en A.
        self.current["A"].send("venta 100\n")
        self.until(lambda: b"Venta aceptada:" in self.current["A"].output)
        self.current["B"].stop(kill=True)
        self.until(lambda: len(self.rows("C")) == 1)
        entries = routes(self.folder / "A/routes.log")
        self.assertEqual([t[0] for t in entries[0]["targets"]], ["B", "C"])
        self.assertIn("S", [t[2] for t in entries[-1]["targets"]])
        self.start("B"); self.settle()
        self.until(lambda: self.equal("ABC", 1))
        self.assertTrue(self.status("A").strip().endswith(b", B"))

    def test_restart_origin_resumes_pending_route_without_duplicates(self):
        self.start("A"); self.start("B"); self.settle("AB")
        self.start("C"); self.settle()
        self.current["A"].stop()
        self.configure("A", delay=1000)
        self.start("A"); self.settle()
        self.current["A"].send("venta 125\n")
        self.until(lambda: b"Venta aceptada:" in self.current["A"].output)
        original = routes(self.folder / "A/routes.log")[0]
        self.current["A"].stop(kill=True)
        self.start("A"); self.settle()
        self.until(lambda: self.equal("ABC", 1))
        self.assertEqual(routes(self.folder / "A/routes.log")[0], original)

    def test_dynamic_discovery_from_one_seed_and_persisted_catalog(self):
        for name in "ABC":
            self.start(name)
        self.settle()
        self.configure("D", seeds="AD")
        self.start("D"); self.settle("ABCD")
        self.assertTrue(self.status("A").strip().endswith(b", D"))
        self.current["D"].send("venta 200\n")
        self.until(lambda: self.equal("ABCD", 1))
        self.current["D"].stop()
        self.start("D"); self.settle("ABCD")
        self.assertIn(b"Par: C", self.status("D"))

    def test_notifications_reach_other_connected_members(self):
        for name in "ABC":
            self.start(name)
        self.settle()
        self.current["B"].stop(kill=True)
        self.until(lambda: all("Se perdió la conexión con la tienda B".encode() in self.current[name].errors for name in "AC"))
        self.until(lambda: "Aviso de cadena X:".encode() in self.current["C"].errors)
        self.current["C"].send("venta 50\n")
        self.until(lambda: self.equal("AC", 1))

    def test_simultaneous_origins_and_independent_limits(self):
        for name in "ABC":
            self.configure(name, limit=100)
            self.start(name)
        self.settle()
        for name in "ABC":
            self.current[name].send("venta 50\n")
        self.until(lambda: self.equal("ABC", 3))
        for name in "ABC":
            self.assertIn(b"ABIERTA", self.status(name).splitlines()[0])
        self.current["A"].send("venta 50\n")
        self.until(lambda: self.equal("ABC", 4))
        self.assertIn(b"SOLO_CONSULTA", self.status("A").splitlines()[0])
        self.assertIn(b"ABIERTA", self.status("B").splitlines()[0])

    def test_control_logs_recover_partial_tail_and_reject_complete_corruption(self):
        for name, filename in (("A", "routes.log"), ("B", "membership.log")):
            with self.subTest(file=filename):
                self.configure(name, seeds=name)
                self.start(name)
                self.current[name].send("venta 100\n")
                self.until(lambda: b"Venta aceptada:" in self.current[name].output)
                self.current[name].stop()
                path = self.folder / name / filename
                original = path.read_bytes()
                path.write_bytes(original + b"registro-interrumpido")
                self.start(name)
                self.current[name].send("historial\n")
                self.until(lambda: b" | 1.00" in self.current[name].output)
                self.current[name].stop()
                self.assertNotIn(b"registro-interrumpido", path.read_bytes())
                content = path.read_bytes()
                damaged = bytes([content[0] ^ 1]) + content[1:]
                path.write_bytes(damaged)
                result = subprocess.run(PREFIX + [str(BINARY), "--config", str(self.folder / f"{name}.conf")],
                                        input=b"", capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b"Iniciar red de tienda", result.stderr)
                self.assertEqual(path.read_bytes(), damaged)
                self.assertEqual(len(self.rows(name)), 1)


if __name__ == "__main__":
    unittest.main()
