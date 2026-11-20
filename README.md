# BlackOut

Proyecto de Sistemas Operativos: tiendas que registran y comparten ventas sin servidor central y recuperan información después de apagones.

Estado: etapa 5, tienda independiente con recepción y procesamiento concurrentes, historial CSV con fecha y hora, recuperación y modo de solo consulta al alcanzar el límite. La comunicación entre tiendas, la lectura del catálogo de pares y las consultas remotas del gobierno todavía no están implementados.

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

`queue_capacity` determina la capacidad de la cola local; su memoria se reserva al abrir la consola. Los demás límites máximos son restricciones de formato. La etapa de red validará el catálogo, la coherencia de cadena y la disponibilidad de puertos. La tienda independiente requiere `chain_id=none` y `peers_file=none`.

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

`salir` termina el proceso ordenadamente y conserva el historial. Una venta que alcance o supere el límite se conserva completa y cambia el estado a `SOLO_CONSULTA`: se rechazan nuevas compras, pero `estado`, `historial`, `ayuda` y `salir` continúan disponibles. La aplicación no termina por alcanzar el límite. Al reiniciar con ese historial vuelve a abrir la consola en modo de solo consulta. Si aún no se alcanzó el límite, inicia en `ABIERTA` y permite seguir vendiendo.

Para consultar el historial sin aceptar compras, incluso después del cierre por límite:

```sh
./bin/blackout --config config/tienda-d.conf --historial
```

La consulta abre y valida el almacenamiento y puede ejecutar la recuperación o migración inicial antes de mostrarlo.

## Concurrencia local

La consola usa dos hilos: el principal recibe y valida comandos; un trabajador procesa ventas, estado, historial y ayuda en orden FIFO. Solo el trabajador modifica las ventas, los ingresos y la secuencia durante la operación. La recuperación inicial y la consulta exclusiva con `--historial` se ejecutan antes o sin crear el trabajador.

Una cola circular de capacidad configurable usa un mutex y dos variables de condición: el consumidor espera cuando está vacía y el productor cuando está llena. Las esperas comprueban su condición en un bucle. Las escrituras, `fsync` y la impresión del historial se ejecutan fuera del mutex de la cola. La recepción puede continuar mientras el trabajador guarda una venta, hasta llenar la cola. Una consulta extensa puede demorar solicitudes posteriores; la ejecución contable sigue siendo secuencial.

Una solicitud encolada todavía no es una venta aceptada: la confirmación conserva el requisito de escritura y sincronización. `estado` e `historial` reflejan las solicitudes anteriores ya procesadas. `salir` deja terminar el trabajo anterior a ese comando e ignora lo que aparezca después en la entrada. El fin de entrada también drena la cola.

Cuando se alcanza el límite, el trabajador rechaza las ventas pendientes o futuras sin escribirlas, pero la cola y la recepción siguen funcionando para atender consultas hasta `salir` o fin de entrada. Ante fallo de almacenamiento o historial, la instancia sí se detiene y las ventas todavía pendientes se rechazan sin confirmación. Las líneas que no llegaron a encolarse no son operaciones aceptadas.

Una tubería notifica al receptor cuando el trabajador debe detener la instancia por un fallo. El receptor espera con `poll` sobre la entrada y esa tubería: puede despertar aunque el usuario no pulse Enter o haya una línea incompleta. La detención también despierta al productor si esperaba espacio en la cola. Se usa `pthread_join` antes de cerrar el historial y destruir mutex, condiciones y cola. En reposo ambos hilos esperan eventos, sin sondeo periódico. Alcanzar el límite no activa esa detención.

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

Si existe el antiguo `sales.journal` y todavía no hay metadatos de inicialización, se valida y convierte mediante un archivo temporal sincronizado y renombrado. El binario original se conserva íntegro como respaldo y no recibe ventas nuevas. Sus fechas se marcan `desconocida`, porque no se almacenaban. La migración no se repite en los siguientes arranques. Después de migrar no debe usarse una versión anterior del programa sobre ese directorio: el CSV es la fuente vigente.

Para empezar un historial nuevo, termina la tienda y elimina únicamente `sales.csv`. En el siguiente arranque se crea un CSV vacío con su cabecera: ingresos y cantidad de ventas comienzan en cero, y la secuencia vuelve a 1. Esto también permite empezar de nuevo si la tienda había alcanzado el límite. Se conserva `store.meta`, por lo que siguen aplicándose la misma identidad y el mismo límite. Un respaldo binario antiguo no se restaura al detectar este reinicio. Un CSV existente pero corrupto continúa provocando error; no se reemplaza silenciosamente por uno vacío.

Una falla de escritura o sincronización detiene la instancia sin confirmar la venta. El resultado de esa última compra puede ser incierto: al reiniciar podría aparecer un registro completo válido aunque no se hubiera mostrado la confirmación. La consola no tiene claves de reintento del cliente; volver a ingresar el mismo importe crea una compra nueva. Hay que comprobar primero el historial recuperado. Una cabecera interrumpida durante la creación inicial también impide abrir y exige diagnóstico; no se reinicializa automáticamente.

La recuperación presupone que el disco conserva sus datos y respeta la sincronización solicitada. Matar un proceso permite probar la recuperación, pero no reproduce por sí solo un corte físico de energía.

## Pruebas

Las pruebas requieren además Python 3, sin paquetes externos:

```sh
make test
make race-check
```

La batería usa procesos y directorios temporales, sin tocar `data/D`. Comprueba reinicios, secuencias, límites, desbordamiento, entradas inválidas, bloqueo entre instancias, interrupciones de escritura y detección de corrupción. Incluye lectura del CSV, fechas con zona, consulta tras cierre y migración sin duplicados ni alteración del respaldo. Un interpositor exclusivo de pruebas (`tests/fault_io.c`, compilado como `build/fault_io.so`) simula escrituras parciales, interrupciones, falta de espacio, fallos de sincronización y caídas antes de confirmar. Se carga únicamente mediante el entorno de los procesos de prueba, no se enlaza con el ejecutable normal.

Las pruebas de concurrencia incluyen capacidad uno, disco lento, consultas ordenadas, cierre con entrada abierta, ventas pendientes y espera en reposo. Una prueba en C utiliza cuatro productores y verifica 16000 solicitudes en dos tamaños de cola, sin pérdidas y con orden por productor. Las pruebas tienen tiempos máximos para detectar bloqueos.

`make race-check` compila ejecutables separados bajo `build/tsan/` con ThreadSanitizer. Requiere que GCC disponga de ese runtime. Comprueba la cola y cuatro escenarios de la tienda; no reemplaza las pruebas funcionales ni demuestra ausencia de carreras en todas las ejecuciones posibles. En este entorno el runtime necesita `setarch -R` para evitar un mapeo de memoria incompatible: el script lo aplica solo a los procesos de prueba, sin cambiar la configuración del sistema. Si el entorno no permite esa opción, la comprobación informa el fallo y no lo considera un resultado limpio.

## Estructura

- `src/`: arranque, configuración, consola concurrente, cola, reglas de tienda y registro durable.
- `include/`: declaraciones y tipos compartidos.
- `config/`: ejemplos de tiendas y catálogo previsto.
- `tests/`: pruebas de integración e inyector de fallos de E/S.
- `build/` y `bin/`: productos locales de compilación, ignorados por Git.
- `data/` y `logs/`: ubicaciones previstas para datos de ejecución, ignoradas por Git.

La guía de seguimiento de las etapas se conserva localmente en `docs/`, excluida del repositorio. Este README contiene las instrucciones necesarias para usar la base sin depender de esa carpeta.

## Autoría

La preparación del proyecto recibió asistencia de Codex, incluido el código de arranque, configuración, tienda independiente y pruebas. La rúbrica del curso exige código no generado por IA: este trabajo no debe presentarse como código escrito íntegramente por el estudiante y tendrá que adecuarse a esa condición para la entrega.
