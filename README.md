# Compilación

Requisitos: Linux, GCC con soporte para C11 y Pthreads, y GNU Make.

Desde la raíz del repositorio, ejecuta:

```sh
make
```

El ejecutable se genera en `bin/blackout`.

Para eliminar los archivos de compilación y volver a compilar desde cero:

```sh
make clean
make
```

## Iniciar las tiendas

Ejecuta todos los comandos desde la raíz del repositorio, después de compilar. Abre una terminal diferente para cada tienda que quieras mantener activa. Después de actualizar el programa, termina las instancias anteriores y vuelve a iniciar todas las tiendas con el ejecutable nuevo.

Las tiendas A, B y C pertenecen a la cadena X:

```sh
# Terminal 1: tienda A
./bin/blackout --config config/tienda-a.conf
```

```sh
# Terminal 2: tienda B
./bin/blackout --config config/tienda-b.conf
```

```sh
# Terminal 3: tienda C
./bin/blackout --config config/tienda-c.conf
```

La tienda D es independiente:

```sh
# Terminal 4: tienda D
./bin/blackout --config config/tienda-d.conf
```

Las configuraciones incluidas permiten ejecutar todas las tiendas en la misma computadora: A, B y C usan los puertos 5101, 5102 y 5103. Cada tienda guarda sus datos por separado en `data/A`, `data/B`, `data/C` o `data/D`; los directorios se crean al iniciar por primera vez. Para volver a abrir una tienda conservando su historial, ejecuta su mismo comando.

Las tiendas de la cadena pueden iniciar en cualquier orden. Usa `estado` para comprobar sus conexiones y el orden de incorporación conocido. Las ventas propias nuevas esperan cinco segundos antes de compartirse; durante esa espera puedes seguir usando la consola.

Para probar una cadena con límite compartido de 300000 centavos, mayor que los 100000 de D, puedes usar estos ejemplos en tres terminales diferentes:

```sh
./bin/blackout --config config/demo-ocho/tienda-a.conf
./bin/blackout --config config/demo-ocho/tienda-b.conf
./bin/blackout --config config/demo-ocho/tienda-c.conf
```

Usan los puertos 5301–5303 y guardan historiales separados en `data/demo-ocho/A`, B y C. Elige un conjunto de configuraciones para cada prueba.

## Comandos de la consola

Escribe un comando y pulsa Enter en la terminal de la tienda correspondiente:

| Comando | Uso |
| --- | --- |
| `venta CENTAVOS` | Registra una venta propia con un importe entero positivo en centavos. Por ejemplo, `venta 1250` registra 12,50 unidades monetarias. |
| `estado` | Muestra el estado de la tienda, cantidad de ventas, ingresos propios y límite. En una cadena también muestra ingresos conocidos de cadena, límite compartido, conexiones, cierre y recuperación pendiente. |
| `historial` | Muestra las ventas registradas con identificador, fecha y hora e importe. En una cadena incluye las ventas recibidas de otras tiendas. |
| `ayuda` | Muestra los comandos disponibles. |
| `desconectar` | Aísla la tienda de su cadena: no envía ni recibe ventas ni intenta reconectarse automáticamente. Conserva las ventas locales y los demás comandos. El aislamiento se mantiene al reiniciar hasta usar `conectar`. |
| `conectar` | Reactiva la red de la cadena, asigna una nueva posición y recupera las ventas faltantes en ambos sentidos, sin duplicarlas. |
| `cerrar` | Solicita detener ventas de la cadena antes de alcanzar el límite y sincronizar el grupo conectado. Mantiene las consolas disponibles. |
| `salir` | Termina el proceso y conserva el historial. Intenta confirmar sus ventas con las tiendas conectadas durante un plazo limitado; informa de los pendientes que quedan para el próximo inicio. |

La tienda independiente tiene su propio límite. Las tiendas de una cadena comparten un único límite sobre la suma de ventas de toda la cadena; todas deben configurar el mismo `limit_cent`.

Al detectar el límite compartido se detienen las compras de las tiendas conectadas y se completan los intercambios pendientes. `estado` muestra `SINCRONIZANDO` mientras se verifica el cierre y `SINCRONIZADO_CONECTADAS` cuando coinciden los historiales del grupo conectado. Las consolas permanecen en `SOLO_CONSULTA` hasta escribir `salir`; el programa no apaga la computadora.

Las tiendas ausentes no impiden cerrar el grupo conectado: se muestran como recuperación pendiente. Una tienda aislada puede seguir vendiendo mientras su información local no indique cierre ni alcance el límite conocido; al reconectar recibe el cierre y recupera las ventas aceptadas. Las ventas simultáneas o realizadas durante una desconexión pueden hacer que el total final supere el límite. Una tienda que ya conoce el cierre no vuelve a habilitar compras al reiniciar o reconectar.

Para simular una desconexión, escribe `desconectar` en C, registra ventas en C y en las demás tiendas y después escribe `conectar` en C. Consulta `historial` y `estado` para comprobar la recuperación. Si las otras tiendas están apagadas, los pendientes se conservan hasta que vuelvan a estar conectadas al mismo tiempo. Los comandos de red y `cerrar` corresponden a tiendas de cadena.

Para consultar únicamente el historial guardado y terminar, sin abrir la consola ni iniciar conexiones de red:

```sh
./bin/blackout --config config/tienda-a.conf --historial
```

Sustituye el archivo de configuración para consultar otra tienda. La tienda debe estar detenida antes de abrir esta consulta sobre sus mismos datos.
