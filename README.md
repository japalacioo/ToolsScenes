# Tools Scenes

Plugin para **OBS Studio 31+** al estilo vMix, con tres paneles:

- **Vista de Escenas**: vista previa en vivo de todas las escenas.
- **Multimedia y Capas**: biblioteca de archivos y entradas, envío al aire sin crear escenas y 4 capas superpuestas.
- **Salida Limpia**: Programa sin capas (o con las elegidas) hacia un monitor o por NDI.

## Instalación en Windows

1. Cierre OBS Studio.
2. Ejecute `dist/ToolsScenes-1.4.0-Windows-x64-Instalador.exe` (pide permisos de administrador).
3. Abra OBS y active los paneles en **Paneles (Docks)**: *Vista de Escenas*, *Multimedia y Capas* y *Salida Limpia*.
4. Para NDI (entradas o salida) instale además **DistroAV** y el **NDI Runtime**.

El instalador detecta la versión de OBS y copia el plugin en `C:\ProgramData\obs-studio\plugins\tools-scenes\`:

- **OBS 33 o superior:** `tools-scenes\tools-scenes.dll` + `tools-scenes\data\` (estructura nueva; OBS no lo
  marca como "heredado").
- **OBS 31–32:** `tools-scenes\bin\64bit\tools-scenes.dll` (la única que cargan esas versiones).

También elimina la versión anterior del plugin (antes llamada ScenePrevio, carpeta `scene-previo`) para que no se
carguen dos copias. Los grupos, la biblioteca, las capas y los ajustes se conservan. Se desinstala
desde *Configuración → Aplicaciones* ("Tools Scenes para OBS Studio").

Instalación manual (OBS 33+): descomprima `ToolsScenes-1.4.0-Windows-x64-manual.zip` en
`C:\ProgramData\obs-studio\plugins\` y borre la carpeta `scene-previo` si existe.

## Multimedia y Capas

- **+ Carpeta / + Archivos / + Entrada**: imágenes, videos, audio, **PDF**, HTML; entradas NDI,
  audio, cámara, navegador, texto, color o cualquier fuente existente de OBS.
- Cada elemento es una **tarjeta** (como las entradas de vMix) con miniatura y botones:
  **Vista** (Vista previa), **Aire** (Programa) y **1 2 3 4** (mostrar/quitar en esa capa;
  naranja = al aire en la capa, contorno naranja = cargado sin mostrar).
  Marco rojo = en Programa, verde = en Vista previa. Doble clic = Vista previa (o Programa
  sin Modo Estudio).
- **Editar posición y tamaño…** (botón del panel, clic derecho en la tarjeta, o al agregar un
  archivo o entrada) abre una **ventana aparte** con el resultado exacto y las herramientas de
  transformación de OBS: mover; 8 asas de tamaño (las esquinas y *Mantener proporción* usan la
  proporción del contenido; Mayús en esquinas = libre); asa
  superior para **rotar** (Mayús = cada 15°); **Alt + asa = recortar**; flechas para mover 1 px
  (Mayús = 10 px); campos numéricos de posición, tamaño, rotación y recorte; Ajustar, Llenar,
  Original, Centrar, Girar ±90°, Voltear H/V y Restablecer. Se aplica en Programa (también con
  "Duplicar escena" del Modo Estudio), Vista previa y capas, en vivo si ya está al aire.
  Toma el tamaño real del contenido (también de un video que no está al aire) y los PDF tienen
  la forma de su página, no la del lienzo.
- Controles del elemento: reproducir/pausar, reiniciar, bucle; en PDF, página anterior/siguiente
  (sin recargar; el visor recuerda la página).
- Una carpeta con imágenes ofrece además una tarjeta de **presentación**.
- **Capas 1–4**: van sobre la salida de Programa (canales de salida de OBS 10–13), con fundido
  configurable. En **Modo Estudio** sus botones aparecen en la columna central, debajo de
  *Transición* (como en vMix); clic = mostrar/ocultar, clic derecho = vaciar. Sin Modo Estudio
  están al pie del panel. La Vista previa no las muestra (igual que vMix).
- Para enviar al aire sin que usted cree escenas, el plugin usa dos escenas propias,
  **SP Multimedia A/B**, que se alternan para poder hacer la transición. Los archivos usados se
  crean como fuentes "SP: nombre" (reutilizables desde OBS).

## Salida Limpia

- **Monitor**: elija la pantalla y pulse *Enviar al monitor*; muestra Programa sin capas, salvo
  las marcadas en *Incluir capas*. Esc o *Quitar del monitor* la cierra. *Identificar* muestra
  durante 3 s el número de cada pantalla (la elegida en rojo).
- **NDI**: nombre (por defecto "OBS Limpio"), capas a incluir e *Iniciar salida NDI*.
- Las salidas que estaban encendidas al cerrar OBS se reanudan solas al abrirlo (el monitor, solo
  si la pantalla guardada está conectada).
  Requiere DistroAV. El audio es la mezcla principal de OBS.

## Vista de Escenas

| Acción | Resultado |
| --- | --- |
| Clic | Modo Estudio: enviar a Vista previa. Sin Modo Estudio: enviar a Programa |
| Doble clic | Modo Estudio: enviar a Programa (con la transición actual) |
| Clic derecho en una escena | Vista previa / Programa / Asignar a grupo / Filtros / Proyector |
| Clic derecho en vacío o ⚙ | Nuevo grupo, renombrar, color, eliminar, opciones |
| Ctrl + rueda | Tamaño de las miniaturas |

- Marco y cabecera **rojos** = Programa; **verdes** = Vista previa.
- **Grupos (pestañas)** con color, guardados dentro de cada colección de escenas
  (por UUID: renombrar una escena no pierde su grupo).
- **Actualización**: Tiempo real, 30, 15, 10 (predeterminado), 5, 2 o 1 fps.
- **Autoajustar**: el tamaño más grande que muestra toda la pestaña sin desplazamiento.

## Por qué no produce tirones

(Vista de Escenas; los visores de Multimedia y Salida Limpia siguen las mismas reglas.)


- **Un solo `obs_display`** dibuja toda la cuadrícula en el hilo gráfico de OBS,
  igual que el Multiview nativo; no hay un display por miniatura.
- **Sin lecturas GPU→CPU** (`stagesurface`/`map`) y sin `obs_enter_graphics` desde la
  interfaz durante el uso normal. Esa es la causa típica de bloqueos en plugins similares.
- **Presupuesto por fotograma**: a N fps, las escenas visibles se reparten entre los
  fotogramas de OBS, así el costo es constante y sin picos.
- Lo que no se ve **no cuesta nada**: escenas fuera de pantalla, otras pestañas y panel oculto.
- El hilo gráfico solo lee una instantánea inmutable (mutex de nanosegundos); las
  referencias a fuentes se liberan siempre en el hilo de interfaz.
- Todas las referencias se sueltan antes de cambiar de colección o al salir.

Medido en OBS 33 (macOS, Apple Silicon): 12 miniaturas a 10 fps ≈ 0,7 ms por fotograma;
48 miniaturas en Tiempo real ≈ 3 ms por fotograma (OBS dispone de 33 ms a 30 fps).
Cierre de OBS con 0 fugas de memoria, también tras cambiar de colección con el monitor y una
capa activos.

## Compilar

Dependencias en `.deps/` (no se versionan):

```bash
brew install llvm lld cmake ninja makensis xwin
git clone --depth 1 --branch 31.0.0 https://github.com/obsproject/obs-studio.git .deps/obs-studio
# Qt6 oficial de obs-deps 2024-09-12 (windows-deps-qt6-…-x64.zip → .deps/qt6-win)
# SDK de Windows: xwin --accept-license --arch x86_64 splat --output .deps/xwin
```

- Windows (DLL + instalador + ZIP): `scripts/build-windows.sh` → `dist/` (`tools-scenes.dll`).
- macOS (para probar con el OBS local): ver `CMakeLists.txt` (sección APPLE);
  `-DSCENE_PREVIO_DEBUG=ON` agrega mediciones y captura de diagnóstico.

El plugin no usa `moc` (sin `Q_OBJECT`), por eso se compila de forma cruzada con
clang-cl. Si agrega llamadas nuevas a la API de OBS, regenere `cmake/windows/*.def`
con los símbolos indefinidos (`llvm-nm -u` sobre los `.obj`).

## Licencia

Tools Scenes se distribuye bajo **GPL-2.0 o posterior** (ver [LICENSE](LICENSE)), igual que
OBS Studio, con cuya biblioteca libobs enlaza. El visor de PDF incluye
[pdf.js](https://mozilla.github.io/pdf.js/) (Apache-2.0, ver `data/pdf/LICENSE-pdfjs.txt`).
