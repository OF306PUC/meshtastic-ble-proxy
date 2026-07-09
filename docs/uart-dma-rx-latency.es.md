# Latencia de recepción UART + DMA: por qué los mensajes entrantes parecían "reactivos"

> Post-mortem del bug en que un mensaje recibido desde la malla LoRa sólo aparecía
> en el teléfono cercano **justo después** de que ese teléfono enviara algo.

## 1. El síntoma

Los mensajes del teléfono lejano se mostraban en el teléfono cercano **sólo cuando el
teléfono cercano enviaba un mensaje**. Todo lo de "aguas arriba" se veía sano:

- **Nodo (LILYGO):** su log mostraba que el paquete FromRadio se recibía de LoRa y se
  **escribía por serial** en ~1 s (`phone downloaded packet`), sin depender de actividad
  del cliente. El envío hacia el cliente funciona.
- **Cómputo del proxy (nRF):** ruteo en sub-milisegundos.
- **Malla:** con `MEDIUM_FAST`, tránsito de ~1–2 s.

Y aun así, de punta a punta se sentían 5–30 s **acoplados al acto de enviar**. El retardo
tenía que estar en el único lugar sin visibilidad: **el cable entre el TX UART del nodo y
el callback de RX UART del proxy.**

## 2. El razonamiento del diagnóstico

La pista clave: `rx_process_byte` / `ROUTE_TRACE` **no se dispara hasta que el teléfono
envía por BLE**. Eso es un **acoplamiento entre dos flujos que no comparten código**:
el procesamiento de RX (UART entrante) y la actividad de TX (BLE saliente).

> Regla general: cuando dos flujos independientes aparecen misteriosamente acoplados,
> busca un **recurso compartido** cuyo estado cambia uno y el otro espera. Aquí ese
> recurso es el **buffer de recepción DMA**.

Pasos de eliminación:

1. El log del nodo prueba que los bytes **salen** del nodo pronto → el retardo es
   *después* del TX del nodo.
2. El ruteo del proxy es sub-ms y la work queue no está bloqueada → no es cómputo del proxy.
3. Entonces los bytes están en el hardware UART del proxy, pero a la aplicación **no se le
   avisa**. En un UART async/DMA, "avisar" = el evento `UART_RX_RDY`. La pregunta se
   reduce a: **¿qué hace que `UART_RX_RDY` dispare, y por qué sólo al enviar?**
4. La única forma en que enviar (BLE → proxy → TX UART al nodo) puede destrabar la
   **recepción** es indirecta: tu ToRadio hace que el nodo emita **más** bytes de vuelta
   (queueStatus, ACK, rebroadcast) — y esos bytes extra son los que empujan el buffer de RX
   sobre su umbral. Ese umbral es **buffer lleno** → hipótesis: `UART_RX_RDY` dispara por
   **buffer lleno**, no por **línea inactiva (idle)**.
5. Confirmado instrumentando `UART_RX_RDY` con longitud de chunk + timestamp.

## 3. El mecanismo — RX por UART con EasyDMA en nRF

Un UART "a pelo" genera una interrupción **por byte**. Simple, pero a altas tasas o con la
CPU ocupada pierdes bytes y quemas ciclos. Por eso se usa **DMA**: el periférico UART
escribe los bytes recibidos directo a un buffer en RAM sin intervención de la CPU. En nRF52
esto es **EasyDMA** dentro del periférico UARTE: le das un puntero (`RXD.PTR`) y un largo
máximo (`RXD.MAXCNT`).

El punto fino es **¿cuándo se le avisa a la CPU "hay datos listos"?** Un RX por DMA tiene
tres señales posibles:

| Señal | Evento nRF | Dispara cuando |
|---|---|---|
| **Buffer lleno** | `ENDRX` | se reciben exactamente `MAXCNT` bytes (DMA completo) |
| **Parada explícita** | `RXTO` tras `STOPRX` | detienes la recepción a propósito |
| **Línea inactiva / timeout** | *(emulado)* | la línea RX queda en silencio un tiempo, buffer *no* lleno |

Para un **stream de tamaño fijo**, "buffer lleno" basta. Para un protocolo **con tramas y
ráfagas** como Meshtastic (tramas de largo variable que llegan cuando la malla entrega),
**necesitas la tercera señal** — si no, una trama de 30 bytes queda en un buffer de 256 y
la CPU nunca se entera hasta que lleguen 226 bytes más. **Tu latencia de RX pasa a ser
función del volumen de tráfico, no del tiempo.** Ése es exactamente el bug.

**La trampa específica de nRF:** a diferencia de STM32 (que tiene interrupción de "IDLE
line" por hardware), **el UARTE de nRF NO tiene detección de línea inactiva nativa.** El
driver async de Zephyr la *emula*. Para vaciar un buffer parcial por idle, el driver debe
saber *cuántos bytes hay en el buffer ahora mismo*, antes del `ENDRX`. Los cuenta de dos
maneras:

- **Conteo por hardware** (`CONFIG_UART_n_NRF_HW_ASYNC=y`): un **TIMER** libre se conecta
  al evento `RXDRDY` del UARTE vía **PPI** (la malla de ruteo de eventos de nRF), de modo
  que el timer incrementa por byte sin costo de CPU. Un `k_timer` vacía el buffer tras el
  `timeout` de idle. Confiable.
- **Conteo por software** (nuestra config previa — HW async **apagado**): el driver
  habilita la interrupción `RXDRDY` por byte y cuenta en la ISR. Funciona en teoría, pero
  es el camino frágil — y en nuestra configuración **no** estaba entregando el vaciado por
  idle, así que `UART_RX_RDY` colapsaba a disparar sólo en `ENDRX` (buffer lleno).

Ésa es la causa raíz: **`UART_RX_TIMEOUT_US = 2000` estaba puesto, pero con el conteo
HW-async apagado el vaciado por idle no ocurría, así que `UART_RX_RDY` quedaba condicionado
al buffer lleno.**

## 3b. Dos "relojes" distintos: el contador de bytes (TIMER) y el reloj de inactividad (k_timer)

<p align="center">
  <img src="figs/UARTE-OP.svg" alt="Datapath del UARTE1 de nRF: buffers EasyDMA RX/TX en RAM, contador de bytes RXDRDY→PPI→TIMER2, y el k_timer de 2 ms que emite UART_RX_RDY" width="85%">
</p>

<p align="center"><em>Figura — Datapath del UARTE1. <strong>RX:</strong> los bytes van RXD line → RX FIFO → EasyDMA → buffer de RX en RAM; cada byte recibido pulsa <code>RXDRDY</code>, que vía <strong>PPI</strong> incrementa <strong>TIMER2</strong> en modo contador (cuenta <em>bytes</em>, no tiempo). Un <code>k_timer</code> de software lee ese contador cada 400 µs y, tras <strong>2 ms</strong> sin bytes nuevos, emite <code>UART_RX_RDY</code> para entregarle los bytes <code>FromRadio</code> a la aplicación. <strong>TX</strong> es simétrico: los bytes <code>ToRadio</code> del buffer de TX → EasyDMA → línea TX.</em></p>

Un punto que confunde: **el TIMER que habilitamos NO mide tiempo.** Hay dos mecanismos
separados y cada uno responde una pregunta distinta:

| Componente | Qué es | Pregunta que responde | ¿Mide tiempo? |
|---|---|---|---|
| **TIMER2** | Periférico HW en modo **COUNTER** (`NRF_TIMER_MODE_COUNTER`) | *"¿cuántos bytes llevo en el buffer?"* | **No** — cuenta eventos |
| **`rx_timeout_timer`** | Un **`k_timer` de software** (kernel de Zephyr) | *"¿hace cuánto que no llega ningún byte?"* | **Sí** |

- **TIMER2 (el "cuántos").** Está cableado en **modo contador**, no de tiempo. Vía **PPI**,
  cada evento `RXDRDY` del UARTE (uno por byte recibido) dispara la tarea `COUNT` del timer,
  así que el timer se incrementa por hardware, sin CPU. Se necesita porque EasyDMA no revela
  el conteo hasta que **termina** (`ENDRX` = buffer lleno); para entregar un buffer *parcial*
  el driver necesita saber cuántos bytes hay **ahora**.

- **`k_timer` (el "hace cuánto").** Éste sí mide tiempo. El driver divide el timeout en
  `RX_TIMEOUT_DIV = 5` tramos: `rx_timeout_slab = 2000 µs / 5 = 400 µs`. Cada 400 µs el
  `k_timer` lee el contador de TIMER2 y:
  - si **cambió** (siguen llegando bytes) → resetea; estamos en medio de una trama, no hay flush;
  - si **no cambió** → resta 400 µs al tiempo restante; al acumular 2 ms **quieto** →
    concluye "línea inactiva" → **flush** (`UART_RX_RDY`).

**La aritmética del umbral** (no la mide el TIMER; sirve para entenderlo). A 115200 baudios:

```
tiempo de bit  = 1 / 115200                         ≈ 8.68 µs
byte en el cable (8N1 = 1 start + 8 datos + 1 stop) ≈ 86.8 µs   (10 bits)
umbral de idle = 2 ms / 86.8 µs                     ≈ 23 bytes
```

Es decir, "inactivo" = **~23 tiempos-de-byte sin recibir nada**. Dentro de una trama los
bytes llegan pegados (~87 µs) y el contador nunca se congela, así que jamás se corta a mitad
de trama; el flush cae en el **borde** de la trama.

**Qué es "flush" exactamente.** Los bytes ya están en RAM (EasyDMA los escribe uno a uno al
llegar). El timeout ejecuta `STOPRX`, que además **drena el pequeño FIFO interno de HW** del
UARTE a RAM y fija el conteo final (`RXD.AMOUNT`), emite `UART_RX_RDY(offset, len)` para
**avisar a la aplicación** (ahí se dispara `rx_process_byte`) y reinicia la recepción. O sea:
flush ≈ **notificar + drenar el FIFO**, no mover el grueso de los datos.

> Resumen: **TIMER2 responde *"¿cuántos?"*; el `k_timer` responde *"¿hace cuánto?"*.** Sin
> `HW_ASYNC`, ese conteo/flush no era fiable, y `UART_RX_RDY` sólo salía por buffer lleno.

## 4. Por qué el "tráfico de retorno" llena el buffer de RX (la parte confusa)

Esto es lo clave y lo menos intuitivo. **El buffer de RX DMA es UN solo buffer contiguo que
acumula TODOS los bytes que el nodo manda por el UART, en orden de llegada, sin importar qué
significan.** No es por-mensaje. El nodo transmite de forma continua un **flujo de bytes**
(telemetría, ACKs, queueStatus, el mensaje de texto reenviado…) hacia ese único buffer. El
disparo de "buffer lleno" cuenta el **total acumulado de bytes**, no tramas.

Paso a paso, con un buffer de 256 bytes:

```
Estado inicial:  [................................................]  0/256
```

1. Llega tu mensaje de texto de la malla (p. ej. 30 bytes). El DMA lo escribe en las
   posiciones 0–29:

   ```
   [MMMMMMMMMMMMMMMMMMMMMMMMMMMMMM..................]  30/256   → NO lleno → sin evento
   ```
   La trama **queda ahí esperando**. La app no se entera.

2. **Envías** un mensaje desde el teléfono: proxy → TX UART → nodo. El nodo, al recibir tu
   ToRadio, **genera tramas FromRadio de respuesta** (un queueStatus que acusa tu envío, un
   paquete de routing/ACK, etc.). El nodo escribe esos bytes de respuesta en la **misma
   línea UART** → aterrizan en el **mismo buffer de RX**, a continuación del byte 29:

   ```
   [MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMqqqqqqqqqqRRRRRRRR.]  ~200/256  → todavía no
   ...más respuestas / telemetría...
   [MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMqqqqqqqqqqRRRRRR...T]  256/256   → ¡ENDRX!
   ```

3. Al llegar a 256 bytes, dispara **buffer lleno** (`ENDRX`), y el driver entrega el
   **buffer completo** de una sola vez — **incluyendo tu mensaje de texto** que estaba
   esperando en 0–29.

Es decir: **el tráfico de retorno no "apunta" a tu mensaje**; simplemente **agrega más
bytes al mismo buffer acumulado**, y cruzar el umbral de 256 es lo que libera todo.
Enviar es sólo **la forma más confiable de hacer que el nodo emita bytes extra justo ahora.**

> Nota: no es *sólo* tu envío. La **telemetría periódica** del nodo también agrega bytes y
> puede gatillar el vaciado por su cuenta — por eso algunos mensajes llegaban "solos", pero
> tarde y de forma errática. El acoplamiento con "enviar" es fuerte porque enviar produce
> respuesta inmediata y garantizada; la telemetría es esporádica.

## 5. La solución

El arreglo es **garantizar la entrega de tramas parciales por idle, independiente del
llenado del buffer.** Opciones, de más barata a más robusta:

1. **Achicar `UART_DMA_BUF_SIZE`** (256 → 32/64). Hace que `ENDRX` dispare antes, así el
   batching en el peor caso es menor. *Prueba rápida / mitigación, no un arreglo real* — una
   trama sola igual espera a que el buffer se llene; sólo acortaste la espera, y la latencia
   sigue acoplada al tráfico. Además genera más churn de `ENDRX`/swaps de buffer.

2. **Habilitar RX hardware-async (el arreglo correcto, el aplicado).** Darle al UARTE un
   TIMER dedicado para que el conteo de bytes + el vaciado por idle de 2 ms sean por
   hardware y confiables:
   ```
   CONFIG_UART_1_NRF_HW_ASYNC=y
   CONFIG_UART_1_NRF_HW_ASYNC_TIMER=2   # TIMER libre — TIMER0 lo usa el controlador BLE
   ```
   Ahora cada trama se entrega a `UART_RX_RDY` ~2 ms después de su último byte, sin importar
   tamaño ni buffer. Esto **desacopla la latencia de RX del tráfico** — el comportamiento
   correcto para un protocolo con tramas.

3. **Manejar `UART_RX_STOPPED` / `UART_RX_DISABLED`** re-habilitando RX (robustez, tema
   aparte). Antes era `default: break` — si la línea alguna vez erraba, RX moría en silencio.

## 6. El principio general para reutilizar

Siempre que hagas **RX por DMA en UART para un protocolo con tramas o request/response**,
necesitas un **disparador de entrega basado en tiempo** (IDLE line en STM32; timer
HW-async + `timeout` en nRF), o bien dimensionar el buffer al tamaño de trama y asumir la
fragilidad. Si alguna vez ves *"RX sólo procesa cuando además transmito"*, sospecha primero
de un **RX por DMA condicionado al llenado del buffer** — el transmitir es sólo lo que
genera el tráfico de retorno que llena tu buffer de RX.
