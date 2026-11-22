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

## Comandos de las tiendas

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


## Iniciar y usar el consultor del gobierno

Con las tiendas originales iniciadas, abre otra terminal desde la raíz del repositorio:

```sh
./bin/blackout --gobierno config/gobierno.conf
```

Si estás usando las tiendas de `config/demo-ocho/`, inicia el consultor con su configuración correspondiente:

```sh
./bin/blackout --gobierno config/demo-ocho/gobierno.conf
```

Ambos ejemplos incluyen D, que se inicia con `config/tienda-d.conf`. El gobierno puede iniciarse antes o después de las tiendas; ellas pueden seguir vendiendo si el consultor está apagado.

| Comando del gobierno | Uso |
| --- | --- |
| `consultar A` | Consulta el estado y los ingresos de A. Sustituye A por otra identidad del catálogo. |
| `consultar todas` | Consulta en paralelo todas las tiendas configuradas y muestra una respuesta por tienda. |
| `ayuda` | Muestra los comandos disponibles del consultor. |
| `salir` | Termina el consultor conservando las respuestas guardadas. No cierra las tiendas. |

Una `RESPUESTA ACTUAL` fue recibida en esa consulta. Si una tienda no responde o devuelve información inválida, se muestra esa situación y, si existe, su última respuesta como `INFORMACION HISTORICA`, con fecha de recepción y antigüedad aproximada. Esa información no confirma que la tienda siga abierta o cerrada ahora. Los ingresos de cadena son los conocidos por cada tienda; no deben sumarse entre respuestas de integrantes de la misma cadena.

`desconectar` aísla únicamente la comunicación entre tiendas: mientras el proceso siga activo, el gobierno puede consultarlo y verá `Red de cadena: AISLADA`. Después de `salir` o de apagar su computadora, solo estará disponible la información que el consultor haya conservado.

Las respuestas se guardan en `data/gobierno/consultas.log`, o en `data/demo-ocho/gobierno/consultas.log` para ese ejemplo. Se recuperan al volver a iniciar el consultor. Ejecuta un solo consultor por directorio de historial.

Para configurar otros destinos:

- En cada archivo de tienda, `query_port` indica su puerto de consulta, distinto de `port`. Omitirlo o usar `query_port=0` desactiva las consultas remotas.
- Los ejemplos originales usan los puertos de consulta 6101–6104 para A–D. La demostración de etapa 8 usa 6401–6403 para A–C y 6104 para D.
- En `gobierno.conf`, `stores_file` apunta al catálogo, `data_dir` selecciona el historial del consultor y `timeout_ms` fija el plazo de consulta.
- Cada línea del catálogo tiene formato `ID CADENA IPv4 PUERTO_DE_CONSULTA`; D utiliza `none` como cadena. Las identidades y direcciones deben ser únicas. Las rutas se interpretan respecto al directorio de ejecución.
