"""Verificaciones instrumentadas con ThreadSanitizer, sin datos de operación."""
import os
from pathlib import Path
import shutil
import selectors
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ENV = {**os.environ, "TSAN_OPTIONS": "halt_on_error=1:exitcode=66"}
# El runtime de GCC en este entorno necesita evitar el mapeo aleatorio incompatible.
# La opción se aplica únicamente a estos procesos hijos, no al sistema.
PREFIX = ["setarch", os.uname().machine, "-R"] if shutil.which("setarch") else []


def check(result):
    if result.returncode != 0:
        raise SystemExit(result.stdout + result.stderr)


queue = subprocess.run(PREFIX + [str(ROOT / "build/tsan/test_work_queue")],
                       capture_output=True, text=True, env=ENV, timeout=30)
check(queue)
print(queue.stdout.strip())
with tempfile.TemporaryDirectory(prefix="blackout-races-") as folder:
    for index, (capacity, limit, commands) in enumerate([
        (1, 100000, "venta 1\n" * 100 + "estado\nhistorial\nsalir\n"),
        (16, 5, "venta 1\n" * 100),
        (2, 100000, "venta 3\n" * 20),
    ]):
        config = Path(folder) / f"store-{index}.conf"
        text = (ROOT / "config/tienda-d.conf").read_text().replace("query_port=6104\n", "")
        text = text.replace("data_dir=data/D", f"data_dir={folder}/store-{index}")
        text = text.replace("queue_capacity=256", f"queue_capacity={capacity}")
        text = text.replace("limit_cent=100000", f"limit_cent={limit}")
        config.write_text(text)
        result = subprocess.run(PREFIX + [str(ROOT / "build/tsan/blackout"), "--config", str(config)],
                                input=commands, capture_output=True, text=True, env=ENV, timeout=30)
        check(result)
    # Alcanzar el límite no termina el proceso; las consultas posteriores siguen operando.
    config = Path(folder) / "live.conf"
    config.write_text((ROOT / "config/tienda-d.conf").read_text().replace("query_port=6104\n", "")
                      .replace("data_dir=data/D", f"data_dir={folder}/live")
                      .replace("limit_cent=100000", "limit_cent=1"))
    process = subprocess.Popen(PREFIX + [str(ROOT / "build/tsan/blackout"), "--config", str(config)],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               bufsize=0, env=ENV)
    try:
        process.stdin.write(b"venta 1\n")
        selector = selectors.DefaultSelector()
        try:
            selector.register(process.stdout, selectors.EVENT_READ)
            initial = b""
            while b"Estado: SOLO_CONSULTA" not in initial:
                if not selector.select(10):
                    raise SystemExit("ThreadSanitizer: no se recibió el estado de solo consulta.")
                chunk = os.read(process.stdout.fileno(), 4096)
                if not chunk:
                    raise SystemExit("La tienda terminó al alcanzar el límite.")
                initial += chunk
        finally:
            selector.close()
        out, err = process.communicate(b"historial\nestado\nventa 1\nsalir\n", timeout=10)
        check(subprocess.CompletedProcess(process.args, process.returncode, out.decode(), err.decode()))
        if b"D:1 |" not in out or b"Salida ordenada" not in out:
            raise SystemExit("No se atendió el historial antes de la salida manual.")
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate(timeout=5)
print("ThreadSanitizer: cola y cuatro escenarios de tienda sin diagnósticos de carreras.")

for pattern in ("simultaneous", "offline_sales", "remote_limit", "short_share"):
    result = subprocess.run(["python3", "-m", "unittest", "discover", "-s", str(ROOT / "tests"),
                             "-p", "test_network.py", "-k", pattern, "-v"],
                            env={**ENV, "BLACKOUT_NETWORK_TEST_BINARY": str(ROOT / "build/tsan/blackout"),
                                 "BLACKOUT_NETWORK_TEST_TSAN": "1"},
                            capture_output=True, text=True, timeout=45)
    check(result)
print("ThreadSanitizer: ventas simultáneas, reconexión, límite y espera de envío sin diagnósticos de carreras.")

for pattern in ("dynamic_discovery", "simultaneous_origins", "skip_unavailable", "recovery_cannot", "console_blocks"):
    result = subprocess.run(["python3", "-m", "unittest", "discover", "-s", str(ROOT / "tests"),
                             "-p", "test_multi_store.py", "-k", pattern, "-v"],
                            env={**ENV, "BLACKOUT_NETWORK_TEST_BINARY": str(ROOT / "build/tsan/blackout"),
                                 "BLACKOUT_NETWORK_TEST_TSAN": "1"},
                            capture_output=True, text=True, timeout=60)
    check(result)
print("ThreadSanitizer: descubrimiento, ventas simultáneas, reincorporación, recuperación y bloques de consola sin diagnósticos de carreras.")

for pattern in ("disconnect_local", "shared_limit_control", "group_closes", "offline_state", "orphaned_route", "paged_cut", "complete_inventory"):
    result = subprocess.run(["python3", "-m", "unittest", "discover", "-s", str(ROOT / "tests"),
                             "-p", "test_recovery.py", "-k", pattern, "-v"],
                            env={**ENV, "BLACKOUT_NETWORK_TEST_BINARY": str(ROOT / "build/tsan/blackout"),
                                 "BLACKOUT_NETWORK_TEST_TSAN": "1"},
                            capture_output=True, text=True, timeout=60)
    check(result)
print("ThreadSanitizer: aislamiento, límite compartido, cierre, reincorporación y conciliación paginada sin diagnósticos de carreras.")


for pattern in ("independent_live", "chain_queries", "isolated_chain", "queries_observe", "fragmented_request", "readonly_and", "slow_clients"):
    result = subprocess.run(["python3", "-m", "unittest", "discover", "-s", str(ROOT / "tests"),
                             "-p", "test_government.py", "-k", pattern, "-v"],
                            env={**ENV, "BLACKOUT_NETWORK_TEST_BINARY": str(ROOT / "build/tsan/blackout"),
                                 "BLACKOUT_NETWORK_TEST_TSAN": "1"},
                            capture_output=True, text=True, timeout=60)
    check(result)
print("ThreadSanitizer: consultas de gobierno, aislamiento, caché y snapshots contables sin diagnósticos de carreras.")
