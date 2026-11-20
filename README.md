# BlackOut

Proyecto de Sistemas Operativos: tiendas que registran y comparten ventas sin servidor central y recuperan información después de apagones.

Estado: etapa 4, tienda independiente con ventas por consola, historial CSV con fecha y hora, recuperación y cierre por límite. Usa un solo hilo. La comunicación entre tiendas, los hilos de trabajo, la lectura del catálogo de pares y las consultas remotas del gobierno todavía no están implementados.

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
historial
ayuda
salir
```

Los importes son centavos enteros positivos: `venta 1250` registra 12,50 unidades monetarias. Cada aceptación informa su ID, por ejemplo `D:1`. Se rechazan importes inválidos o que desborden la suma de 64 bits, y se permite ingresar otro comando. Un comando demasiado largo se descarta completo. El fin de entrada también termina ordenadamente.

`estado` muestra estado comercial, cantidad de ventas, ingresos y límite. `historial` muestra cada venta con su identificador, fecha y hora con zona horaria e importe con dos decimales. Consultarlo no modifica los ingresos ni la siguiente secuencia.

`salir` conserva el historial y permite continuar en el siguiente arranque si todavía no se alcanzó el límite. Una venta que alcance o supere el límite se conserva completa y el proceso termina después de informar el estado CERRADA. Al reiniciar con ese historial permanece cerrada y no procesa nuevas compras.

Para consultar el historial sin aceptar compras, incluso después del cierre por límite:

```sh
./bin/blackout --config config/tienda-d.conf --historial
```

La consulta abre y valida el almacenamiento y puede ejecutar la recuperación o migración inicial antes de mostrarlo.

## Persistencia y recuperación

El historial principal es `data_dir/sales.csv`, un archivo de texto CSV con estas columnas:

```csv
identificador,fecha_hora,valor_venta,crc32
```

`identificador` tiene formato `D:1`; `fecha_hora` usa, por ejemplo, `2026-11-20T12:30:00-0500`; `valor_venta` usa un punto decimal y dos cifras decimales (1250 centavos se guardan como `12.50`). En memoria se sigue calculando con centavos enteros. La hora procede del reloj y la zona del sistema al registrar la venta; en esta computadora es America/Guayaquil. Puede elegirse otra zona mediante la variable de entorno `TZ`. El orden de ventas depende de la secuencia, no de que el reloj avance de manera continua.

El campo `crc32` detecta daños sobre el texto de los otros tres campos; no autentica ventas. El CSV puede inspeccionarse con un editor o importarse en una hoja de cálculo, pero no debe editarse sobre el archivo de operación. Sus registros tienen un formato estricto; cambiar separadores, finales de línea o importes requiere recalcular la verificación y no es una operación soportada.

`store.meta` conserva en texto la versión, identidad y límite con CRC32 y sirve de archivo de bloqueo. Cambiar identidad o límite sobre el mismo directorio provoca rechazo, evitando mezclar tiendas o reabrir una tienda elevando su límite. No debe borrarse. Los nuevos períodos comerciales todavía no están implementados.

Las ventas se añaden al final mediante `write`, manejando escrituras parciales e interrupciones. Solo se confirman después de `fsync`. Al crear directorios y archivos también se sincronizan los directorios correspondientes. Esta decisión sigue las condiciones de durabilidad descritas en el [manual de fsync](https://man7.org/linux/man-pages/man2/fsync.2.html).

Al iniciar se reconstruyen ingresos y secuencia desde el CSV válido. Una última fila incompleta sin salto de línea se elimina y se informa; las filas completas previas se conservan. Una fila completa con checksum incorrecto, secuencia inválida o cabecera dañada impide abrir sin descartar registros. Los registros completos recuperados se sincronizan antes de informar el estado. Los bloqueos consultivos con `fcntl` impiden que dos instancias cooperantes escriban a la vez. No se admite reemplazar o eliminar archivos de operación mientras una tienda los usa, ni acceder a ellos mediante enlaces simbólicos o físicos adicionales.

Si existe el antiguo `sales.journal` y todavía no se inicializó el CSV, se valida y convierte mediante un archivo temporal sincronizado y renombrado. El binario original se conserva íntegro como respaldo y no recibe ventas nuevas. Sus fechas se marcan `desconocida`, porque no se almacenaban. La migración no se repite en los siguientes arranques. Después de migrar no debe usarse una versión anterior del programa sobre ese directorio: el CSV es la fuente vigente. Si falta un CSV ya inicializado, se informa el problema en lugar de sustituirlo por un respaldo antiguo. Una interrupción de la primera inicialización entre la publicación de metadatos y el renombrado puede requerir diagnóstico del archivo temporal; no se descartan los datos para reiniciar en blanco.

Una falla de escritura o sincronización detiene la instancia sin confirmar la venta. El resultado de esa última compra puede ser incierto: al reiniciar podría aparecer un registro completo válido aunque no se hubiera mostrado la confirmación. La consola no tiene claves de reintento del cliente; volver a ingresar el mismo importe crea una compra nueva. Hay que comprobar primero el historial recuperado. Una cabecera interrumpida durante la creación inicial también impide abrir y exige diagnóstico; no se reinicializa automáticamente.

La recuperación presupone que el disco conserva sus datos y respeta la sincronización solicitada. Matar un proceso permite probar la recuperación, pero no reproduce por sí solo un corte físico de energía.

## Pruebas

Las pruebas requieren además Python 3, sin paquetes externos:

```sh
make test
```

La batería usa procesos y directorios temporales, sin tocar `data/D`. Comprueba reinicios, secuencias, límites, desbordamiento, entradas inválidas, bloqueo entre instancias, interrupciones de escritura y detección de corrupción. Incluye lectura del CSV, fechas con zona, consulta tras cierre y migración sin duplicados ni alteración del respaldo. Un interpositor exclusivo de pruebas (`tests/fault_io.c`, compilado como `build/fault_io.so`) simula escrituras parciales, interrupciones, falta de espacio, fallos de sincronización y caídas antes de confirmar. Se carga únicamente mediante el entorno de los procesos de prueba, no se enlaza con el ejecutable normal.

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
