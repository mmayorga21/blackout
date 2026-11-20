# BlackOut

Proyecto de Sistemas Operativos: tiendas que registran y comparten ventas sin servidor central y recuperan información después de apagones.

Estado: etapa 4, tienda independiente con ventas por consola, historial durable, recuperación y cierre por límite. Usa un solo hilo. La comunicación entre tiendas, los hilos de trabajo, la lectura del catálogo de pares y las consultas remotas del gobierno todavía no están implementados.

## Compilación

Requisitos: Linux, GCC con C11 y Pthreads, GNU Make.

```sh
make
./bin/blackout --help
./bin/blackout --config config/tienda-d.conf
```

El Makefile utiliza gcc con advertencias y soporte para Pthreads. Puede sobrescribirse el compilador con `make CC=gcc`. Para eliminar únicamente los productos de compilación:

```sh
make clean
```

## Configuración

Desde la raíz del repositorio:

```sh
make run
make run CONFIG=config/tienda-d.conf
```

Se incluyen A, B y C de la cadena X para las etapas posteriores, y D independiente para esta etapa. Cada ejemplo usa un puerto y directorio de datos propios. Actualmente se rechazan configuraciones de cadena para evitar operarlas como independientes por error. El programa no abre sockets; D sí crea su directorio e historial de ventas.

Formato: una `clave=valor` por línea, espacios exteriores opcionales, comentarios en líneas que comienzan con `#`. No se admiten claves repetidas, desconocidas o faltantes. Las rutas no usan comillas y se interpretarán respecto al directorio de ejecución.

| Clave | Valores |
| --- | --- |
| store_id | De 1 a 32 letras ASCII, números, guion o guion bajo; identidad única. |
| chain_id | Mismo formato; `none` identifica una tienda independiente y se reserva para ese uso. |
| bind_address | Dirección IPv4 literal; los ejemplos usan loopback para pruebas locales. |
| port | Entero de 1 a 65535. Para pruebas sin privilegios usar puertos mayores que 1023. |
| limit_cent | Entero positivo representable en 64 bits con signo, en centavos. 100000 equivale a 1000,00 unidades monetarias. |
| timeout_ms | Entero de 1 a 3600000 milisegundos. |
| queue_capacity | Entero de 1 a 1000000 elementos. |
| data_dir | Ruta no vacía de hasta 255 bytes; cada tienda necesita una propia. |
| peers_file | Ruta de hasta 255 bytes al catálogo previsto; `none` para D. Su contenido aún no se valida. |

Los límites máximos son restricciones de formato de esta base; el programa todavía no reserva colas. La etapa de red validará el catálogo, la coherencia de cadena y la disponibilidad de puertos. La tienda independiente requiere `chain_id=none` y `peers_file=none`.

Si la configuración es inválida, se muestra el error y se permite editar el archivo y pulsar Enter para volver a cargarlo. `q` cancela; con entrada estándar cerrada finaliza con estado de error. Los argumentos de línea de órdenes inválidos muestran el uso y devuelven error.

## Ventas y cierre

Después del arranque se aceptan estos comandos:

```text
venta 1250
estado
ayuda
salir
```

Los importes son centavos enteros positivos: `venta 1250` registra 12,50 unidades monetarias. Cada aceptación informa su ID, por ejemplo `D:1`. Se rechazan importes inválidos o que desborden la suma de 64 bits, y se permite ingresar otro comando. Un comando demasiado largo se descarta completo. El fin de entrada también termina ordenadamente.

`estado` muestra estado comercial, cantidad de ventas, ingresos y límite. `salir` conserva el historial y permite continuar en el siguiente arranque si todavía no se alcanzó el límite. Una venta que alcance o supere el límite se conserva completa y el proceso termina después de informar el estado CERRADA. Al reiniciar con ese historial permanece cerrada y no procesa nuevas compras.

## Persistencia y recuperación

El historial binario `data_dir/sales.journal` contiene una cabecera de 64 bytes y registros de venta de 32 bytes, con formato versión 1, enteros little-endian y CRC32 para detectar daños. No es un mecanismo de autenticación. La cabecera vincula el archivo con la identidad y el límite configurados; cambiarlos sobre un historial existente provoca rechazo, evitando mezclar tiendas o reabrir una tienda elevando su límite. Los nuevos períodos comerciales todavía no están implementados.

Las ventas se añaden al final mediante `write`, manejando escrituras parciales e interrupciones. Solo se confirman después de `fsync`. Al crear directorios y archivos también se sincronizan los directorios correspondientes. Esta decisión sigue las condiciones de durabilidad descritas en el [manual de fsync](https://man7.org/linux/man-pages/man2/fsync.2.html).

Al iniciar se reconstruyen ingresos y secuencia desde el historial válido. Un último registro de venta incompleto se elimina y se informa; los registros completos previos se conservan. Un registro completo con checksum incorrecto, secuencia inválida o cabecera dañada impide abrir sin descartar registros. Los registros completos recuperados se sincronizan antes de informar el estado. Un bloqueo consultivo con `fcntl` sobre el historial impide que dos instancias cooperantes escriban a la vez. No se admite reemplazar o eliminar el archivo mientras una tienda lo usa, ni acceder a él mediante enlaces simbólicos o físicos adicionales.

Una falla de escritura o sincronización detiene la instancia sin confirmar la venta. El resultado de esa última compra puede ser incierto: al reiniciar podría aparecer un registro completo válido aunque no se hubiera mostrado la confirmación. La consola no tiene claves de reintento del cliente; volver a ingresar el mismo importe crea una compra nueva. Hay que comprobar primero el historial recuperado. Una cabecera interrumpida durante la creación inicial también impide abrir y exige diagnóstico; no se reinicializa automáticamente.

La recuperación presupone que el disco conserva sus datos y respeta la sincronización solicitada. Matar un proceso permite probar la recuperación, pero no reproduce por sí solo un corte físico de energía.

## Pruebas

Las pruebas requieren además Python 3, sin paquetes externos:

```sh
make test
```

La batería usa procesos y directorios temporales, sin tocar `data/D`. Comprueba reinicios, secuencias, límites, desbordamiento, entradas inválidas, bloqueo entre instancias, interrupciones de escritura y detección de corrupción. Un interpositor exclusivo de pruebas (`tests/fault_io.c`, compilado como `build/fault_io.so`) simula escrituras parciales, interrupciones, falta de espacio, fallos de sincronización y caídas antes de confirmar. Se carga únicamente mediante el entorno de los procesos de prueba, no se enlaza con el ejecutable normal.

## Estructura

- `src/`: arranque, configuración, consola, reglas de tienda y registro durable.
- `include/`: declaraciones y tipos compartidos.
- `config/`: ejemplos de tiendas y catálogo previsto.
- `tests/`: pruebas de integración e inyector de fallos de E/S.
- `build/` y `bin/`: productos locales de compilación, ignorados por Git.
- `data/` y `logs/`: ubicaciones previstas para datos de ejecución, ignoradas por Git.

La guía de seguimiento de las etapas se conserva localmente en `docs/`, excluida del repositorio. Este README contiene las instrucciones necesarias para usar la base sin depender de esa carpeta.

## Autoría

La preparación del proyecto recibió asistencia de Codex, incluido el código de arranque, configuración, tienda independiente y pruebas. La rúbrica del curso exige código no generado por IA: este trabajo no debe presentarse como código escrito íntegramente por el estudiante y tendrá que adecuarse a esa condición para la entrega.
