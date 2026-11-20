# BlackOut

Proyecto de Sistemas Operativos: tiendas que registran y comparten ventas sin servidor central y recuperan información después de apagones.

Estado: etapa 6, tiendas independientes y cadenas de exactamente dos tiendas con comunicación TCP, ventas compartidas, reconexión y deduplicación. Se conserva el historial CSV y el modo de solo consulta al alcanzar el límite. El orden entre múltiples tiendas y las consultas remotas del gobierno todavía no están implementados.

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

`config/tienda-d.conf` inicia D independiente. `config/demo-dos/` contiene una demostración ejecutable de A y B de la cadena X, con puertos y datos separados. Los ejemplos originales A/B/C y su catálogo de tres tiendas quedan para la etapa 7: actualmente se rechaza un catálogo con más de dos integrantes.

Formato: una `clave=valor` por línea, espacios exteriores opcionales, comentarios en líneas que comienzan con `#`. No se admiten claves repetidas, desconocidas o faltantes. Las rutas no usan comillas y se interpretarán respecto al directorio de ejecución.

| Clave | Valores |
| --- | --- |
| store_id | De 1 a 32 letras ASCII, números, guion o guion bajo; identidad única. |
| chain_id | Mismo formato; `none` identifica una tienda independiente y se reserva para ese uso. |
| bind_address | Dirección IPv4 literal; los ejemplos usan loopback para pruebas locales. |
| port | Entero de 1 a 65535. Para pruebas sin privilegios usar puertos mayores que 1023. |
| limit_cent | Límite de ingresos propios de esta tienda, entero positivo en centavos. 100000 equivale a 1000,00 unidades monetarias. Cada integrante puede tener un valor diferente. |
| timeout_ms | Entero de 1 a 3600000 milisegundos. |
| network_delay_ms | Opcional, atraso simulado de recepción por mensaje: de 0 a 60000 ms. Si se omite, usa 10000 ms (10 segundos). No afecta a la tienda independiente. |
| queue_capacity | Entero de 1 a 1000000 elementos. |
| data_dir | Ruta no vacía de hasta 255 bytes; cada tienda necesita una propia. |
| peers_file | Catálogo de exactamente dos tiendas para una cadena; `none` para D. |

`queue_capacity` determina la capacidad de la cola local; su memoria se reserva al abrir la consola. Los demás límites máximos son restricciones de formato. La tienda independiente requiere `chain_id=none` y `peers_file=none`. En una cadena se validan el catálogo, las identidades, IPv4 y puertos; un fallo de bind impide iniciar el servicio de red. La identidad propia y su puerto deben coincidir con el catálogo; `0.0.0.0` permite escuchar en todas las interfaces.

## Demostración con dos tiendas

Desde la raíz del repositorio, abre dos terminales. En la primera:

```sh
make
./bin/blackout --config config/demo-dos/tienda-a.conf
```

En la segunda:

```sh
./bin/blackout --config config/demo-dos/tienda-b.conf
```

Ingresa `venta 1000` en A y `venta 2500` en B. Después del intercambio, `estado` muestra ingresos propios de 1000 y 2500 respectivamente e ingresos conocidos de cadena de 3500 centavos en ambas. `historial` permite ver los dos eventos y sus fechas originales. Los CSV quedan en `data/demo-dos/A/` y `data/demo-dos/B/`; D conserva sus datos separados en `data/D/`.

El catálogo usa una línea `ID IPv4 PUERTO` por tienda, incluida la propia. Requiere dos identidades y extremos distintos. Para computadoras diferentes, modifica direcciones y rutas de los ejemplos y usa el mismo catálogo en ambas. Las dos pueden empezar en cualquier orden y registrar compras sin que la otra esté conectada, sujetas al límite conocido localmente.

## Ventas compartidas y red

Cada cadena usa tres hilos: recepción de consola, trabajador contable y red. El trabajador registra compras y ventas remotas en la misma cola. Solo él modifica el historial y sus índices. La red conserva una copia protegida de los eventos ya registrados y utiliza sockets no bloqueantes, `poll` y una tubería de notificación. Si la cola está llena conserva una solicitud remota y pausa la lectura; continúa atendiendo envíos y notificaciones, sin esperar espacio bloqueando el hilo de red.

Ambas tiendas escuchan y la de identidad lexicográficamente menor inicia la conexión. Esta regla evita conexiones duplicadas. La identificación inicial comprueba versión, cadena e identidad del par. El límite anunciado debe ser un entero válido, pero no tiene que coincidir con el local ni modifica su configuración. El protocolo está pensado para los pares de confianza configurados en el laboratorio; la identificación no es autenticación criptográfica.

Los mensajes tienen cabecera de 12 bytes: magic BLK6, versión 1, tipo, dos bytes reservados y longitud de cuerpo en orden de red. El cuerpo debe ser menor de 256 bytes. HELLO intercambia identidad/cadena/límite y atraso, SALE contiene una fila CSV de venta, ACK confirma un ID guardado, PING/PONG comprueban comunicación y REJECT informa rechazo. Se manejan mensajes fragmentados o agrupados y envíos parciales. Los plazos base de conexión, confirmación y reconexión proceden de `timeout_ms`, ajustados al atraso descrito a continuación. El formato exacto puede evolucionar al incorporar el recorrido de la etapa 7.

Cada mensaje completo recibido espera `network_delay_ms` antes de entregarse al protocolo. Las configuraciones de demostración y el valor predeterminado usan 10000 ms en ambas tiendas: HELLO, ventas, confirmaciones y latidos reciben 10 segundos de atraso por sentido. Una venta confirmada localmente sigue guardándose sin ese atraso; su recepción remota tarda al menos 10 segundos y su confirmación de ida y vuelta al menos 20. La cola de red conserva el orden y el trabajo pendiente puede añadir espera.

El atraso utiliza una cola limitada por `queue_capacity` y vencimientos de reloj monotónico, atendidos con `poll`; no duerme el hilo de consola ni el trabajador contable. HELLO anuncia además el atraso del receptor (un cuarto campo opcional; mensajes antiguos de tres campos anuncian cero). La espera de ACK suma los atrasos de ambos extremos a `timeout_ms`; el latido usa la mitad de ese plazo efectivo y la identificación permite el atraso local. Esto evita declarar una caída solo por la simulación. `estado` muestra el atraso configurado.

Al perder una conexión establecida, la tienda que permanece activa muestra `Se perdió la conexión con la tienda A en la cadena X (...)`, identificando al par y la cadena. Se emite una vez por pérdida de conexión; los intentos fallidos de reconexión no repiten el aviso. Una nueva conexión vuelve a mostrar el mensaje de conexión establecida. El cierre del propio programa no anuncia falsamente que el otro par se apagó. La detección usa el cierre/error del socket o el vencimiento del plazo de respuesta: no depende de que el par pueda enviar un aviso antes de apagarse. Esta etapa sigue admitiendo exactamente dos tiendas por cadena.

El origen guarda y sincroniza antes de compartir. El receptor confirma únicamente después de guardar y sincronizar, o después de reconocer un duplicado ya registrado. El mismo ID con otro importe o fecha se rechaza. Una venta nueva aumenta ingresos conocidos de cadena; solo si su origen coincide con la tienda incrementa ingresos propios. La secuencia propia no depende del número de ventas ajenas.

Al reconectar se reenvía el historial completo de eventos registrados. Un índice por ID evita duplicaciones; el receptor conserva las fechas e importes del origen. Las confirmaciones y el avance de esta conexión se mantienen en memoria: al reiniciar se reconstruye la entrega reenviando desde el CSV durable. `estado` muestra conexión y entregas todavía sin confirmar en esa conexión. Que se hayan recibido los mismos eventos no garantiza que ambas tiendas observen simultáneamente todas las confirmaciones.

La primera versión intercambia historiales completos en cada reconexión; la optimización por inventarios y la validación exhaustiva de apagones quedan para la etapa 8. El funcionamiento eventual presupone comunicación recuperada, capacidad de almacenamiento y participantes que regresan. Los ingresos de cadena son información compartida y no se comparan con el límite local para autorizar compras.

Cada tienda compara sus ingresos propios con su límite. Si A tiene límite 100000 y llega a 100000 de ventas propias, solo A pasa a SOLO_CONSULTA; B continúa vendiendo hasta alcanzar su propio límite. Esto se cumple también si ambas tienen el mismo valor configurado. A sigue recibiendo las ventas de B y actualizando los ingresos conocidos de cadena. La consola conserva historial y estado hasta salir manualmente. `salir` detiene la red y deja procesar las solicitudes ya encoladas; los eventos sin confirmación permanecen en disco para el siguiente arranque. Esta etapa no implementa todavía el certificado de cierre distribuido.

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

`estado` muestra estado comercial, cantidad de ventas registradas, ingresos propios y límite local. En cadenas, otra línea distingue los ingresos propios del total conocido de cadena. La cantidad de ventas registradas incluye eventos recibidos del par. `historial` muestra cada venta con su identificador, fecha y hora con zona horaria e importe con dos decimales. Consultarlo no modifica los ingresos ni la siguiente secuencia.

`salir` termina el proceso ordenadamente y conserva el historial. Una venta que alcance o supere el límite se conserva completa y cambia el estado a `SOLO_CONSULTA`: se rechazan nuevas compras, pero `estado`, `historial`, `ayuda` y `salir` continúan disponibles. La aplicación no termina por alcanzar el límite. Al reiniciar con ese historial vuelve a abrir la consola en modo de solo consulta. Si aún no se alcanzó el límite, inicia en `ABIERTA` y permite seguir vendiendo.

Para consultar el historial sin aceptar compras, incluso después del cierre por límite:

```sh
./bin/blackout --config config/tienda-d.conf --historial
```

La consulta abre y valida el almacenamiento y puede ejecutar la recuperación o migración inicial antes de mostrarlo.

## Concurrencia local

La tienda independiente usa dos hilos: el principal recibe y valida comandos; un trabajador procesa ventas, estado, historial y ayuda en orden FIFO. En las cadenas el hilo de red produce además solicitudes remotas para esa cola. Solo el trabajador modifica las ventas, los ingresos y la secuencia durante la operación. La recuperación inicial y la consulta exclusiva con `--historial` se ejecutan antes o sin crear el trabajador y no abren servicios de red.

Una cola circular de capacidad configurable usa un mutex y dos variables de condición: el consumidor espera cuando está vacía y el productor cuando está llena. Las esperas comprueban su condición en un bucle. Las escrituras, `fsync` y la impresión del historial se ejecutan fuera del mutex de la cola. La recepción puede continuar mientras el trabajador guarda una venta, hasta llenar la cola. Una consulta extensa puede demorar solicitudes posteriores; la ejecución contable sigue siendo secuencial.

Una solicitud encolada todavía no es una venta aceptada: la confirmación conserva el requisito de escritura y sincronización. `estado` e `historial` reflejan las solicitudes anteriores ya procesadas. `salir` deja terminar el trabajo anterior a ese comando e ignora lo que aparezca después en la entrada. El fin de entrada también drena la cola.

Cuando se alcanza el límite, el trabajador rechaza las ventas pendientes o futuras sin escribirlas, pero la cola y la recepción siguen funcionando para atender consultas hasta `salir` o fin de entrada. Ante fallo de almacenamiento o historial, la instancia sí se detiene y las ventas todavía pendientes se rechazan sin confirmación. Las líneas que no llegaron a encolarse no son operaciones aceptadas.

Una tubería notifica al receptor cuando el trabajador debe detener la instancia por un fallo. El receptor espera con `poll` sobre la entrada y esa tubería: puede despertar aunque el usuario no pulse Enter o haya una línea incompleta. La detención también despierta al productor si esperaba espacio en la cola. Se usa `pthread_join` antes de cerrar el historial y destruir mutex, condiciones y cola. En reposo ambos hilos esperan eventos, sin sondeo periódico. Alcanzar el límite no activa esa detención.

## Persistencia y recuperación

El historial principal es `data_dir/sales.csv`, un archivo de texto CSV con estas columnas:

```csv
identificador,fecha_hora,valor_venta,crc32
```

`identificador` tiene formato `D:1` para la tienda independiente. En cadenas usa `ORIGEN:PERIODO:SECUENCIA`, con un período aleatorio de 128 bits escrito como 32 caracteres hexadecimales. `fecha_hora` usa, por ejemplo, `2026-11-20T12:30:00-0500`; `valor_venta` usa un punto decimal y dos cifras decimales (1250 centavos se guardan como `12.50`). En memoria se sigue calculando con centavos enteros. La hora procede del reloj y la zona del sistema al registrar en origen; en esta computadora es America/Guayaquil. Puede elegirse otra zona mediante la variable de entorno `TZ`. El orden no depende de que el reloj avance de manera continua.

El campo `crc32` detecta daños sobre el texto de los otros tres campos; no autentica ventas. El CSV puede inspeccionarse con un editor o importarse en una hoja de cálculo, pero no debe editarse sobre el archivo de operación. Sus registros tienen un formato estricto; cambiar separadores, finales de línea o importes requiere recalcular la verificación y no es una operación soportada.

`store.meta` conserva en texto la versión, identidad y límite con CRC32 y sirve de archivo de bloqueo. En cadenas también vincula el directorio a su cadena. Cambiar esos datos sobre el mismo directorio provoca rechazo. Cada cadena tiene además `period.id`, que conserva el período local y su CRC32. Un reinicio normal mantiene ese período y continúa la secuencia; un CSV nuevo crea un período distinto, evitando reutilizar IDs. No borres ni edites los metadatos de un historial existente.

Las ventas se añaden al final mediante `write`, manejando escrituras parciales e interrupciones. Solo se confirman después de `fsync`. Al crear directorios y archivos también se sincronizan los directorios correspondientes. Esta decisión sigue las condiciones de durabilidad descritas en el [manual de fsync](https://man7.org/linux/man-pages/man2/fsync.2.html).

Al iniciar se reconstruyen ingresos y secuencia desde el CSV válido. Una última fila incompleta sin salto de línea se elimina y se informa; las filas completas previas se conservan. Una fila completa con checksum incorrecto, secuencia inválida o cabecera dañada impide abrir sin descartar registros. Los registros completos recuperados se sincronizan antes de informar el estado. Los bloqueos consultivos con `fcntl` impiden que dos instancias cooperantes escriban a la vez. No se admite reemplazar o eliminar archivos de operación mientras una tienda los usa, ni acceder a ellos mediante enlaces simbólicos o físicos adicionales.

Si existe el antiguo `sales.journal` y todavía no hay metadatos de inicialización, se valida y convierte mediante un archivo temporal sincronizado y renombrado. El binario original se conserva íntegro como respaldo y no recibe ventas nuevas. Sus fechas se marcan `desconocida`, porque no se almacenaban. La migración no se repite en los siguientes arranques. Después de migrar no debe usarse una versión anterior del programa sobre ese directorio: el CSV es la fuente vigente.

Para empezar un historial independiente nuevo, termina la tienda y elimina únicamente `sales.csv`. En el siguiente arranque se crea un CSV vacío con su cabecera: ingresos y cantidad de ventas comienzan en cero, y la secuencia vuelve a 1. Esto también permite empezar de nuevo si la tienda había alcanzado el límite. Se conserva `store.meta`, por lo que siguen aplicándose la misma identidad y el mismo límite. Un respaldo binario antiguo no se restaura al detectar este reinicio. Un CSV existente pero corrupto continúa provocando error; no se reemplaza silenciosamente por uno vacío.

En una cadena, eliminar su CSV genera un período local nuevo, pero una tienda conectada puede devolver ventas antiguas que conserva como respaldo compartido. No equivale a borrar globalmente las ventas de la cadena: el reinicio coordinado de un período comercial queda fuera de esta etapa. No se convierte automáticamente un historial independiente en uno de cadena.

Una falla de escritura o sincronización detiene la instancia sin confirmar la venta. El resultado de esa última compra puede ser incierto: al reiniciar podría aparecer un registro completo válido aunque no se hubiera mostrado la confirmación. La consola no tiene claves de reintento del cliente; volver a ingresar el mismo importe crea una compra nueva. Hay que comprobar primero el historial recuperado. Una cabecera interrumpida durante la creación inicial también impide abrir y exige diagnóstico; no se reinicializa automáticamente.

La recuperación presupone que el disco conserva sus datos y respeta la sincronización solicitada. Matar un proceso permite probar la recuperación, pero no reproduce por sí solo un corte físico de energía.

## Pruebas

Las pruebas requieren además Python 3, sin paquetes externos:

```sh
make test
make race-check
```

La batería usa procesos y directorios temporales, sin tocar `data/D`. Comprueba reinicios, secuencias, límites, desbordamiento, entradas inválidas, bloqueo entre instancias, interrupciones de escritura y detección de corrupción. Incluye lectura del CSV, fechas con zona, consulta tras cierre y migración sin duplicados ni alteración del respaldo. Un interpositor exclusivo de pruebas (`tests/fault_io.c`, compilado como `build/fault_io.so`) simula escrituras parciales, interrupciones, falta de espacio, fallos de sincronización y caídas antes de confirmar. Se carga únicamente mediante el entorno de los procesos de prueba, no se enlaza con el ejecutable normal.

Las pruebas de concurrencia incluyen capacidad uno, disco lento, consultas ordenadas, cierre manual, ventas pendientes y espera en reposo. Una prueba en C utiliza cuatro productores y verifica 16000 solicitudes en dos tamaños de cola, sin pérdidas y con orden por productor. Las pruebas de red levantan dos procesos en puertos temporales y pares TCP controlados para verificar ventas simultáneas, fechas originales, reconexión, reinicio, cambios de período, límites, duplicados, conflictos, mensajes fragmentados/agrupados y fallos de almacenamiento remoto sin ACK falso. Se miden los 10 segundos reales de atraso y se comprueban avisos de desconexión normal/abrupta sin repeticiones ni falsos fallos por atraso. El resto de pruebas de red configura cero atraso para ejecutarse más rápido. Las pruebas tienen tiempos máximos para detectar bloqueos.

`make race-check` compila ejecutables separados bajo `build/tsan/` con ThreadSanitizer. Requiere que GCC disponga de ese runtime. Comprueba la cola, cuatro escenarios locales y cuatro escenarios entre dos tiendas, incluido atraso; no reemplaza las pruebas funcionales ni demuestra ausencia de carreras en todas las ejecuciones posibles. En este entorno el runtime necesita `setarch -R` para evitar un mapeo de memoria incompatible: el script lo aplica solo a los procesos de prueba, sin cambiar la configuración del sistema. Si el entorno no permite esa opción, la comprobación informa el fallo y no lo considera un resultado limpio.

## Estructura

- `src/`: arranque, configuración, consola concurrente, cola, reglas de tienda y registro durable.
- `include/`: declaraciones y tipos compartidos.
- `config/`: ejemplos de tiendas y catálogo previsto.
- `tests/`: pruebas de integración e inyector de fallos de E/S.
- `build/` y `bin/`: productos locales de compilación, ignorados por Git.
- `data/` y `logs/`: ubicaciones previstas para datos de ejecución, ignoradas por Git.

La guía de seguimiento de las etapas se conserva localmente en `docs/`, excluida del repositorio. Este README contiene las instrucciones necesarias para usar la base sin depender de esa carpeta.

## Autoría

La preparación del proyecto recibió asistencia de Codex, incluidos configuración, persistencia, concurrencia, comunicación de red y pruebas. La rúbrica del curso exige código no generado por IA: este trabajo no debe presentarse como código escrito íntegramente por el estudiante y tendrá que adecuarse a esa condición para la entrega.
