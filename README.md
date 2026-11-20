# BlackOut

Proyecto de Sistemas Operativos: tiendas que registran y comparten ventas sin servidor central y recuperan información después de apagones.

Estado: etapa 3, base de compilación y validación de configuración. El ejecutable valida el archivo, informa sus parámetros y termina. Las ventas, persistencia, hilos de trabajo, sockets, lectura del catálogo de pares y consultas del gobierno todavía no están implementados.

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
make run CONFIG=config/tienda-a.conf
```

Se incluyen A, B y C de la cadena X, y D independiente. Cada ejemplo usa un puerto y directorio de datos propios. Los archivos son ejemplos de desarrollo; no crean sockets ni escriben ventas.

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

Los límites máximos son restricciones de formato de esta base; el programa todavía no reserva colas. La etapa de red validará el catálogo, la coherencia de cadena y la disponibilidad de puertos. La etapa de persistencia verificará acceso y aislamiento de directorios.

Si la configuración es inválida, se muestra el error y se permite editar el archivo y pulsar Enter para volver a cargarlo. `q` cancela; con entrada estándar cerrada finaliza con estado de error. Los argumentos de línea de órdenes inválidos muestran el uso y devuelven error.

## Estructura

- `src/`: arranque y lector de configuración.
- `include/`: declaraciones y tipos compartidos.
- `config/`: ejemplos de tiendas y catálogo previsto.
- `build/` y `bin/`: productos locales de compilación, ignorados por Git.
- `data/` y `logs/`: ubicaciones previstas para datos de ejecución, ignoradas por Git.

La guía de seguimiento de las etapas se conserva localmente en `docs/`, excluida del repositorio. Este README contiene las instrucciones necesarias para usar la base sin depender de esa carpeta.

## Autoría de la base

La preparación de esta base de compilación y configuración recibió asistencia de Codex, incluido el código inicial de arranque. No implementa los algoritmos de ventas del proyecto. La rúbrica del curso exige código no generado por IA: este punto de partida no debe presentarse como código escrito íntegramente por el estudiante y tendrá que adecuarse a esa condición para la entrega.
