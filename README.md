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

Ejecuta todos los comandos desde la raíz del repositorio, después de compilar. Abre una terminal diferente para cada tienda que quieras mantener activa.

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

## Comandos de la consola

Escribe un comando y pulsa Enter en la terminal de la tienda correspondiente:

| Comando | Uso |
| --- | --- |
| `venta CENTAVOS` | Registra una venta propia con un importe entero positivo en centavos. Por ejemplo, `venta 1250` registra 12,50 unidades monetarias. |
| `estado` | Muestra el estado de la tienda, cantidad de ventas, ingresos propios y límite. En una cadena también muestra ingresos conocidos de cadena, conexiones y orden de incorporación. |
| `historial` | Muestra las ventas registradas con identificador, fecha y hora e importe. En una cadena incluye las ventas recibidas de otras tiendas. |
| `ayuda` | Muestra los comandos disponibles. |
| `salir` | Termina el proceso ordenadamente y conserva el historial para el siguiente inicio. Las entregas pendientes pueden continuar al reiniciar. |

Cada tienda tiene su propio límite. Al alcanzarlo pasa a `SOLO_CONSULTA`: rechaza nuevas ventas propias, pero mantiene disponibles los demás comandos y sigue recibiendo ventas de la cadena hasta que escribas `salir`.

Para consultar únicamente el historial guardado y terminar, sin abrir la consola ni iniciar conexiones de red:

```sh
./bin/blackout --config config/tienda-a.conf --historial
```

Sustituye el archivo de configuración para consultar otra tienda. La tienda debe estar detenida antes de abrir esta consulta sobre sus mismos datos.
