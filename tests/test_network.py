"""Dos procesos reales y pares TCP controlados, con datos y puertos temporales."""
import csv
import os
from pathlib import Path
import selectors
import socket
import struct
import subprocess
import tempfile
import time
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("BLACKOUT_NETWORK_TEST_BINARY", ROOT / "bin/blackout"))
PREFIX = [] if not os.environ.get("BLACKOUT_NETWORK_TEST_TSAN") else ["setarch", os.uname().machine, "-R"]


class RunningStore:
    def __init__(self, config, fault=None):
        env = os.environ.copy()
        if fault:
            env["LD_PRELOAD"] = "$ORIGIN/../build/fault_io.so"
            env["BLACKOUT_TEST_FAULT"] = fault
        self.process = subprocess.Popen(PREFIX + [str(BINARY), "--config", str(config)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, bufsize=0, env=env)
        self.output = b""
        self.errors = b""
        self.closed = False
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ, "output")
        self.selector.register(self.process.stderr, selectors.EVENT_READ, "errors")

    def pump(self, timeout=0):
        for key, _ in self.selector.select(timeout):
            data = os.read(key.fileobj.fileno(), 65536)
            if not data:
                self.selector.unregister(key.fileobj)
            else:
                setattr(self, key.data, getattr(self, key.data) + data)

    def send(self, text):
        self.process.stdin.write(text.encode())

    def stop(self, kill=False):
        if self.closed:
            return self.process.returncode
        if self.process.poll() is None:
            if kill:
                self.process.kill()
            else:
                try:
                    self.send("salir\n")
                except BrokenPipeError:
                    pass
        out, err = self.process.communicate(timeout=10)
        self.output += out
        self.errors += err
        self.selector.close()
        self.closed = True
        return self.process.returncode


def wire(kind, body=b""):
    return struct.pack("!4sBBHI", b"BLK6", 2, kind, 0, len(body)) + body


def receive_frame(sock):
    def exact(length):
        result = b""
        while len(result) < length:
            part = sock.recv(length - len(result))
            if not part:
                raise EOFError("Conexión cerrada")
            result += part
        return result
    magic, version, kind, reserved, length = struct.unpack("!4sBBHI", exact(12))
    if magic != b"BLK6" or version != 2 or reserved or length >= 256:
        raise ValueError("Cabecera inválida")
    return kind, exact(length)


def csv_sale(sequence=1, amount="12.50", period="a" * 32, origin="A"):
    prefix = f"{origin}:{period}:{sequence},2026-11-20T12:30:00-0500,{amount}".encode()
    return prefix + f",{zlib.crc32(prefix):08x}\n".encode()


class TwoStores(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="blackout-network-")
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        # Reservar ambos puertos simultáneamente para que sean distintos.
        sockets = [socket.socket(), socket.socket()]
        for sock in sockets:
            sock.bind(("127.0.0.1", 0))
        self.ports = dict(zip("AB", [sock.getsockname()[1] for sock in sockets]))
        for sock in sockets:
            sock.close()
        self.peers = self.folder / "chain.peers"
        self.peers.write_text("".join(f"{name} 127.0.0.1 {self.ports[name]}\n" for name in "AB"))
        self.stores = []
        for name in "AB":
            self.configure(name)

    def configure(self, name, chain="X", limit=100000, capacity=1, delay=0, timeout=500):
        text = (ROOT / "config/tienda-a.conf").read_text().replace("query_port=6101\n", "")
        text = text.replace("store_id=A", f"store_id={name}")
        text = text.replace("chain_id=X", f"chain_id={chain}")
        text = text.replace("port=5101", f"port={self.ports[name]}")
        text = text.replace("limit_cent=300000", f"limit_cent={limit}")
        text = text.replace("timeout_ms=2000", f"timeout_ms={timeout}")
        text = text.replace("sale_share_delay_ms=5000", f"sale_share_delay_ms={delay}")
        text = text.replace("queue_capacity=256", f"queue_capacity={capacity}")
        text = text.replace("data_dir=data/A", f"data_dir={self.folder}/{name}")
        text = text.replace("peers_file=config/cadena-x.peers", f"peers_file={self.peers}")
        path = self.folder / f"{name}.conf"
        path.write_text(text)
        return path

    def start(self, name, fault=None):
        store = RunningStore(self.folder / f"{name}.conf", fault=fault)
        self.stores.append(store)
        self.addCleanup(self.cleanup_store, store)
        self.until(lambda: b"Estado:" in store.output)
        return store

    def cleanup_store(self, store):
        if store.closed:
            return
        try:
            store.stop(kill=store.process.poll() is None)
        except (ValueError, BrokenPipeError):
            pass

    def until(self, condition, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for store in self.stores:
                if store.selector.get_map():
                    store.pump(0.01)
            if condition():
                return
        diagnostics = "\n".join((s.output + s.errors).decode(errors="replace") for s in self.stores)
        self.fail("Tiempo agotado:\n" + diagnostics)

    def rows(self, name):
        path = self.folder / name / "sales.csv"
        if not path.exists():
            return []
        with path.open() as file:
            rows = list(csv.DictReader(file))
        # No considerar una fila que el otro proceso todavía esté escribiendo.
        if any(not row.get("crc32") or len(row["crc32"]) != 8 for row in rows):
            return []
        return rows

    def synchronized(self, count):
        a, b = self.rows("A"), self.rows("B")
        return len(a) == len(b) == count and {tuple(r.items()) for r in a} == {tuple(r.items()) for r in b}

    def query(self, store):
        previous = store.output.count(b"Par:")
        store.send("estado\n")
        self.until(lambda: store.output.count(b"Par:") > previous)
        return store.output

    def raw_peer(self):
        deadline = time.monotonic() + 5
        while True:
            try:
                sock = socket.create_connection(("127.0.0.1", self.ports["B"]), timeout=2)
                sock.settimeout(5)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                for store in self.stores:
                    store.pump(0.02)
        kind, _ = receive_frame(sock)
        self.assertEqual(kind, 1)
        hello = wire(1, b"X A 100000")
        sock.sendall(hello[:5])
        sock.sendall(hello[5:])
        self.addCleanup(sock.close)
        return sock

    def assert_protocol_disconnect(self, sock):
        # Puede haber un latido ya enviado antes de recibir la entrada inválida.
        # No debe aparecer ACK ni otro dato, y la conexión debe cerrarse en plazo.
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            sock.settimeout(max(.001, deadline - time.monotonic()))
            try:
                kind, body = receive_frame(sock)
            except EOFError:
                return
            self.assertEqual((kind, body), (4, b""))
        self.fail("El par no cerró la conexión tras una entrada inválida")

    def responses(self, sock, expected_kind, count=1):
        results = []
        while len(results) < count:
            kind, body = receive_frame(sock)
            if kind == expected_kind:
                results.append(body)
            elif kind == 2:
                sock.sendall(wire(3, body.split(b",", 1)[0]))
            elif kind == 4:
                sock.sendall(wire(5))
            else:
                self.fail(f"Respuesta inesperada {kind}: {body}")
        return results

    def test_simultaneous_sales_own_totals_and_sequence(self):
        a, b = self.start("A"), self.start("B")
        a.send("venta 50\n" * 10)
        b.send("venta 70\n" * 10)
        self.until(lambda: self.synchronized(20))
        self.assertIn(b"Ingresos propios: 500 | Ingresos conocidos de cadena: 1200", self.query(a))
        self.assertIn(b"Ingresos propios: 700 | Ingresos conocidos de cadena: 1200", self.query(b))
        a.send("venta 1\n")
        self.until(lambda: self.synchronized(21))
        own = [r for r in self.rows("A") if r["identificador"].startswith("A:")]
        self.assertEqual(own[-1]["identificador"].split(":")[-1], "11")
        self.assertEqual(a.stop(), 0)
        self.assertEqual(b.stop(), 0)

    def test_offline_sales_reconnect_and_restart_deduplicate(self):
        b = self.start("B")
        b.send("venta 400\n")
        self.until(lambda: len(self.rows("B")) == 1)
        self.assertIn(b"DESCONECTADA", self.query(b))
        a = self.start("A")
        a.send("venta 600\n")
        self.until(lambda: self.synchronized(2))
        self.assertEqual(a.stop(), 0)
        b.send("venta 500\n")
        self.until(lambda: b"Ingresos conocidos de cadena: 1500" in b.output)
        b.stop(kill=True)
        a, b = self.start("A"), self.start("B")
        self.until(lambda: self.synchronized(3))
        self.assertIn(b"Ingresos conocidos de cadena: 1500", self.query(a))
        self.assertIn(b"Ingresos propios: 900", self.query(b))

    def test_new_csv_new_period_and_normal_restart_same_period(self):
        a = self.start("A")
        a.send("venta 100\n")
        self.until(lambda: len(self.rows("A")) == 1)
        old_id = self.rows("A")[0]["identificador"]
        self.assertEqual(a.stop(), 0)
        (self.folder / "A/sales.csv").unlink()
        a = self.start("A")
        a.send("venta 200\n")
        self.until(lambda: len(self.rows("A")) == 1)
        new_id = self.rows("A")[0]["identificador"]
        self.assertNotEqual(old_id, new_id)
        self.assertEqual(new_id.split(":")[-1], "1")
        self.assertEqual(a.stop(), 0)
        a = self.start("A")
        a.send("venta 300\n")
        self.until(lambda: len(self.rows("A")) == 2)
        last_id = self.rows("A")[-1]["identificador"]
        self.assertEqual(last_id.rsplit(":", 1)[0], new_id.rsplit(":", 1)[0])
        self.assertTrue(last_id.endswith(":2"))

    def test_remote_limit_keeps_network_and_queries_available(self):
        for name, amount in (("A", 800), ("B", 700)):
            self.configure(name, limit=1000)
            store = self.start(name)
            store.send(f"venta {amount}\n")
            self.until(lambda: len(self.rows(name)) == 1)
            self.assertEqual(store.stop(), 0)
        a, b = self.start("A"), self.start("B")
        self.until(lambda: self.synchronized(2))
        # El límite corresponde al ingreso compartido: ambos quedan en consulta.
        for store in (a, b):
            self.assertIn(b"SOLO_CONSULTA", self.query(store).rsplit(b"Estado:", 1)[1].splitlines()[0])
            previous = len(store.errors)
            store.send("venta 1\nhistorial\nayuda\n")
            self.until(lambda: b"Venta rechazada" in store.errors[previous:])
            self.assertIsNone(store.process.poll())
        self.assertEqual(len(self.rows("A")), 2)
        self.assertEqual(len(self.rows("B")), 2)

    def test_chain_and_limit_mismatch_never_share(self):
        self.configure("B", chain="Y")
        a, b = self.start("A"), self.start("B")
        a.send("venta 100\n")
        b.send("venta 200\n")
        self.until(lambda: any(b"Mensaje de red rechazado" in s.errors for s in (a, b)))
        self.until(lambda: len(self.rows("A")) == 1 and len(self.rows("B")) == 1)
        self.assertIn(b"Ingresos propios: 100", self.query(a))
        self.assertIn(b"Ingresos propios: 200", self.query(b))

    def test_different_chain_limits_reject_connection(self):
        self.configure("B", limit=200000)
        a, b = self.start("A"), self.start("B")
        a.send("venta 100\n"); b.send("venta 200\n")
        self.until(lambda: any("Límite de cadena incompatible".encode() in store.errors for store in (a, b)))
        self.until(lambda: len(self.rows("A")) == 1 and len(self.rows("B")) == 1)
        self.assertNotIn("Conexión de cadena establecida".encode(), a.errors + b.errors)

    def test_restart_preserves_shared_chain_limit(self):
        for name in "AB":
            self.configure(name, limit=1000)
        a, b = self.start("A"), self.start("B")
        a.send("venta 1000\n")
        self.until(lambda: self.synchronized(1))
        self.assertEqual(a.stop(), 0); self.assertEqual(b.stop(), 0)
        a, b = self.start("A"), self.start("B")
        for store in (a, b):
            self.assertIn(b"SOLO_CONSULTA", self.query(store).rsplit(b"Estado:", 1)[1].splitlines()[0])
            previous = len(store.errors)
            store.send("venta 1\n")
            self.until(lambda: b"Venta rechazada" in store.errors[previous:])
        self.assertEqual(len(self.rows("A")), 1)
        self.assertEqual(len(self.rows("B")), 1)

    def test_fragmented_grouped_duplicate_and_conflicting_sales(self):
        b = self.start("B")
        sock = self.raw_peer()
        sale = csv_sale()
        frames = wire(2, sale) + wire(2, sale)
        sock.sendall(frames[:7])
        sock.sendall(frames[7:])
        acks = self.responses(sock, 3, 2)
        self.assertEqual(acks[0], acks[1])
        self.until(lambda: len(self.rows("B")) == 1)
        before = (self.folder / "B/sales.csv").read_bytes()
        sock.sendall(wire(2, csv_sale(amount="99.00")))
        self.responses(sock, 6)
        self.assertEqual((self.folder / "B/sales.csv").read_bytes(), before)
        self.assertIsNone(b.process.poll())

    def test_oversized_message_disconnects_without_stopping_store(self):
        b = self.start("B")
        sock = self.raw_peer()
        sock.sendall(struct.pack("!4sBBHI", b"BLK6", 2, 2, 0, 1000000))
        self.assert_protocol_disconnect(sock)
        self.assertIn(b"Ingresos propios: 0", self.query(b))
        self.assertEqual(self.rows("B"), [])

    def test_invalid_sale_crc_or_origin_never_changes_csv(self):
        b = self.start("B")
        for body in (csv_sale().replace(b"12.50", b"13.50"), csv_sale(origin="Z")):
            with self.subTest(body=body):
                sock = self.raw_peer()
                sock.sendall(wire(2, body))
                self.assert_protocol_disconnect(sock)
                sock.close()
                self.assertEqual(self.rows("B"), [])
                self.assertIsNone(b.process.poll())

    def test_remote_storage_failure_sends_no_registration_ack(self):
        b = self.start("B", fault="sync_error")
        sock = self.raw_peer()
        sock.sendall(wire(2, csv_sale()))
        acknowledged = False
        try:
            while True:
                kind, _ = receive_frame(sock)
                if kind == 3:
                    acknowledged = True
                elif kind == 4:
                    sock.sendall(wire(5))
        except EOFError:
            pass
        self.assertFalse(acknowledged)
        self.until(lambda: b.process.poll() is not None)
        b.pump()
        self.assertNotEqual(b.process.returncode, 0)
        self.assertNotIn(b"Venta remota registrada:", b.output)
        self.assertIn(b"Guardar venta remota", b.errors)

    def test_catalog_rejects_duplicate_store_identity(self):
        self.peers.write_text(self.peers.read_text() + "A 127.0.0.1 5103\n")
        result = subprocess.run(PREFIX + [str(BINARY), "--config", str(self.folder / "A.conf")],
                                input=b"salir\n", capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Catálogo inválido".encode(), result.stderr)
        self.assertFalse((self.folder / "A/sales.csv").exists())

    def test_five_second_default_delay_preserves_local_responsiveness(self):
        for name in "AB":
            path = self.configure(name, capacity=32, timeout=1000)
            # Omitir la opción comprueba también el valor predeterminado de 5 segundos.
            path.write_text(path.read_text().replace("sale_share_delay_ms=0\n", ""))
        connection_started = time.monotonic()
        a, b = self.start("A"), self.start("B")
        self.until(lambda: all("Conexión de cadena establecida".encode() in s.errors for s in (a, b)), timeout=3)
        self.assertLess(time.monotonic() - connection_started, 3)
        started = time.monotonic()
        a.send("venta 1250\nventa 50\nventa 100\n")
        self.until(lambda: a.output.count(b"Venta aceptada:") == 3, timeout=3)
        self.assertLess(time.monotonic() - started, 3)
        self.assertEqual(self.rows("B"), [])
        self.query(a)
        self.assertEqual(self.rows("B"), [])
        self.until(lambda: self.synchronized(3), timeout=10)
        self.assertGreaterEqual(time.monotonic() - started, 4.9)
        self.assertLess(time.monotonic() - started, 8)
        # No esperar otros cinco segundos para recibir la confirmación.
        ack_deadline = time.monotonic() + 2
        while "Entregas sin confirmar en esta conexión: 0".encode() not in self.query(a).split(b"Par:")[-1]:
            self.assertLess(time.monotonic(), ack_deadline)
        for store in (a, b):
            self.assertNotIn("Se perdió la conexión".encode(), store.errors)
            self.assertIsNone(store.process.poll())

    def test_short_share_wait_does_not_delay_control_traffic(self):
        for name in "AB":
            # La etapa de recorridos añade sincronizaciones de control a disco.
            # La espera sigue siendo previa al envío, sin atrasar los ACK.
            self.configure(name, capacity=32, delay=150, timeout=1000)
        a, b = self.start("A"), self.start("B")
        self.until(lambda: all("Conexión de cadena establecida".encode() in s.errors for s in (a, b)))
        a.send("venta 100\n")
        b.send("venta 200\n")
        self.until(lambda: self.synchronized(2))
        for store in (a, b):
            deadline = time.monotonic() + 5
            while "Entregas sin confirmar en esta conexión: 0".encode() not in self.query(store).split(b"Par:")[-1]:
                self.assertLess(time.monotonic(), deadline)
            self.assertNotIn("Se perdió la conexión".encode(), store.errors)

    def test_received_sale_and_ack_immediate_despite_five_second_share_wait(self):
        self.configure("B", delay=5000)
        b = self.start("B")
        started = time.monotonic()
        sock = self.raw_peer()
        self.assertLess(time.monotonic() - started, 2)
        sock.settimeout(2)
        started = time.monotonic()
        sock.sendall(wire(2, csv_sale()))
        self.responses(sock, 3)
        self.assertLess(time.monotonic() - started, 2)
        self.assertEqual(len(self.rows("B")), 1)
        self.assertIsNone(b.process.poll())

    def test_graceful_disconnect_notifies_remaining_store_once(self):
        a, b = self.start("A"), self.start("B")
        self.until(lambda: all("Conexión de cadena establecida".encode() in s.errors for s in (a, b)))
        self.assertEqual(a.stop(), 0)
        self.until(lambda: "Se perdió la conexión con la tienda A en la cadena X".encode() in b.errors)
        self.assertNotIn("Se perdió la conexión".encode(), a.errors)
        self.query(b)
        self.assertEqual(b.errors.count("Se perdió la conexión".encode()), 1)
        b.send("venta 100\n")
        self.until(lambda: len(self.rows("B")) == 1)
        self.assertIsNone(b.process.poll())

    def test_abrupt_disconnect_notifies_once_then_reconnects(self):
        a, b = self.start("A"), self.start("B")
        self.until(lambda: all("Conexión de cadena establecida".encode() in s.errors for s in (a, b)))
        b.stop(kill=True)
        self.until(lambda: "Se perdió la conexión con la tienda B en la cadena X".encode() in a.errors)
        # Dejar ocurrir intentos de reconexión fallidos; no son nuevas desconexiones.
        deadline = time.monotonic() + 1.1
        self.until(lambda: time.monotonic() >= deadline)
        self.assertEqual(a.errors.count("Se perdió la conexión".encode()), 1)
        b = self.start("B")
        self.until(lambda: a.errors.count("Conexión de cadena establecida".encode()) == 2)
        self.assertEqual(b.stop(), 0)
        self.until(lambda: a.errors.count("Se perdió la conexión".encode()) == 2)

    def test_delay_configuration_rejects_invalid_values(self):
        for value in ("-1", "60001", "1.5", "no"):
            with self.subTest(value=value):
                path = self.configure("A")
                path.write_text(path.read_text().replace("sale_share_delay_ms=0", f"sale_share_delay_ms={value}"))
                result = subprocess.run(PREFIX + [str(BINARY), "--config", str(path)],
                                        input=b"q\n", capture_output=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("valor inválido para sale_share_delay_ms".encode(), result.stderr)


if __name__ == "__main__":
    unittest.main()
