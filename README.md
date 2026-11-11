# Transformaciones

Menú de administración para Skyrim Special Edition (pensado para servidores de **SkyMP**) que permite convertirse en cualquier NPC del juego: un lobo, un draugr, un mudcrab, un dragón, un guardia, un jarl…

Se abre con **F11** y tiene un buscador con todos los NPC cargados. Al elegir una criatura, el jugador pasa a tener su raza y su piel, con sus animaciones, y puede moverse, atacar y lanzar sus acciones. Al elegir una persona, el jugador pasa a tener su cara y su cuerpo: sexo, raza, rasgos, pelo, pinturas, color de piel, peso y altura. La ropa no se copia.

> Estado: en pruebas. Funciona en el juego local; la sincronización con los demás jugadores necesita el código del servidor de [`servidor/`](#lo-que-necesita-el-servidor-de-skymp), que aún no se ha probado en un servidor real.

## Contenido del repositorio

| Ruta | Qué es |
|---|---|
| `src/plugin.cpp` | El plugin SKSE: menú, transformación (criaturas y personas), aspecto original, cámara, acciones, ataques con el ratón y desatascar. Todo el código C++ está aquí |
| `src/PCH.h` | Cabecera precompilada (CommonLibSSE y la biblioteca estándar) |
| `Scripts/Source/TransformacionesAPI.psc` | Script de Papyrus que hace el cambio de raza (`Actor.SetRace`) y el desatasco. El plugin lo llama directamente |
| `Scripts/TransformacionesAPI.pex` | El script compilado |
| `Platform/Plugins/transformaciones-sync.js` | Plugin de Skyrim Platform (el entorno de JavaScript del cliente de SkyMP) que avisa al servidor cuando cambia el aspecto del jugador (raza, cara o piel) |
| `servidor/transformaciones-gamemode.js` | Código para el **gamemode del servidor** de SkyMP, para que todos vean al jugador transformado. No va en el cliente |
| `Transformaciones.ini` | Ajustes: la tecla del menú y las razas que no se muestran. El plugin guarda aquí también la cámara de cada criatura |
| `CMakeLists.txt`, `CMakePresets.json` | Compilación con CMake y Ninja |
| `vcpkg.json`, `vcpkg-configuration.json` | Dependencias: CommonLibSSE-NG e ImGui (con DirectX 11) |
| `compilar.ps1` | Compila todo, rehace el paquete y, con `-Instalar`, lo copia al juego |
| `paquete/Transformaciones/` | El mod listo para instalar, con la misma estructura que la carpeta `Data` del juego |
| `paquete/Transformaciones.zip` | Lo mismo en un `.zip` para repartir |

## Instalación (jugador o admin que se transforma)

Requisitos:

- Skyrim Special Edition **1.6.1170** (es la versión probada; el plugin se adapta a las versiones 1.6.1130 y posteriores).
- [SKSE](https://skse.silverlock.org/) para esa versión.
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444).
- El cliente de SkyMP (trae Skyrim Platform), solo si se quiere sincronizar con el servidor.

Instalación: copia el contenido de `paquete/Transformaciones.zip` dentro de la carpeta `Data` del juego. Quedan así:

| Archivo | Destino |
|---|---|
| `Transformaciones.dll`, `Transformaciones.ini` | `Data/SKSE/Plugins/` |
| `TransformacionesAPI.pex` | `Data/Scripts/` |
| `transformaciones-sync.js` | `Data/Platform/Plugins/` |

Solo necesita el mod **quien se transforma**. Los demás jugadores no tienen que instalar nada (ver [la parte del servidor](#lo-que-necesita-el-servidor-de-skymp)).

## Uso

Pulsa **F11** para abrir o cerrar el menú (también se cierra con **Esc**). Mientras está abierto, el personaje no se mueve ni ataca, para poder escribir y hacer clic.

### Pestañas

| Pestaña | Qué tiene |
|---|---|
| **Humanos** | Las personas: NPC de razas jugables (nórdicos, orcos, elfos, khajiit, argonianos…) o que se mueven como personas (vampiros, ancianos, dremora…). Cada uno con su nombre, raza y sexo |
| **Criaturas** | Todo lo demás, sin repetidos |
| **Ocultos (repetidos)** | Los NPC que se transforman exactamente igual que otro de las pestañas anteriores, con el aviso "igual que: …". También se puede transformar desde aquí |
| **Acciones** | Solo aparece **siendo una criatura**. Lista las animaciones de esa criatura (ataques, aullidos, gestos…) para lanzarlas a mano |

**Qué se copia:**

- **Criaturas:** la raza y la piel. La piel es el cuerpo de la criatura: todos los lobos son de la raza `WolfRace`, pero el lobo de nieve, el rojo y el de fuego tienen pieles distintas, igual que cada tipo de draugr.
- **Personas:** su cara y su cuerpo, los mismos datos que se eligen en el menú de creación de personaje: sexo, raza, piel, peso, altura, color de piel y de pelo, textura de la cara, partes de la cabeza (cabeza, pelo, barba, cejas, ojos, cicatrices), los 19 rasgos de la cara, los preajustes y las pinturas (pinturas de guerra, suciedad, maquillaje). Se puede pasar a cualquier sexo y raza. **La ropa no se copia**: se ve su cara y su cuerpo con el equipo que lleves.

La cara se genera al momento con los datos del NPC, igual que la del jugador. Los NPC del juego usan una cara precalculada a partir de esos mismos datos, así que casi siempre se ven iguales, aunque alguno puede cambiar un poco, sobre todo en el tono de piel o el maquillaje. Con NPC de mods, ese mod tiene que estar instalado.

**Cómo se agrupan los repetidos:** dos criaturas son "la misma" si tienen la misma raza y la misma piel; dos personas, si toda su cara es idéntica (muchos NPC genéricos, como bandidos o guardias, comparten cara). De cada grupo queda el nombre que usan más NPC; los demás van a Ocultos.

**Marcas en la lista:** `[criatura]`; `[inmóvil]`, `[no camina]` o `[solo vuela]` si la raza no puede andar; y `[sin animaciones]`. Con esas, el personaje se quedará quieto.

### Botones

| Botón | Qué hace |
|---|---|
| **Transformarme** (o doble clic, o Intro) | Te convierte en el NPC elegido |
| **Volver a mi aspecto** | Te devuelve tu aspecto original completo (raza, piel, cara y cuerpo) y te desatasca. Ver [Aspecto original](#aspecto-original) |
| **Desatascar** | Para cuando el personaje se queda sin control: devuelve los controles, quita el control por IA, la parálisis y el bloqueo de movimiento, y vuelve a la postura de pie |
| **Cerrar** | Cierra el menú |

### Aspecto original

Antes de la primera transformación, el plugin guarda tu aspecto completo en `Data/SKSE/Plugins/Transformaciones/<nombre del personaje>.txt`. Puedes transformarte en varios NPC seguidos: al volver, siempre recuperas el aspecto que tenías antes de la primera transformación. Después, el archivo se borra.

Va en un archivo y no en la partida porque SkyMP no usa las partidas guardadas: así aguanta reconexiones y cierres del juego. **No lo borres mientras estés transformado**, o no habrá a qué volver.

### Cámara de las criaturas

Muchas criaturas no tienen punto de cámara, o lo tienen casi en el suelo (el mudcrab), y la cámara atravesaba el terreno. Con cualquier criatura, el plugin pone un punto de cámara propio, siempre a 70 unidades de alto como mínimo, y en el menú aparecen dos deslizadores:

- **Altura**: dónde mira la cámara.
- **Distancia**: lo lejos que está (0 = la del juego).

Lo que ajustes se guarda **por raza** en `Transformaciones.ini`, sección `[Camara]`. Con las razas humanas se usa la cámara normal del juego.

### Atacar siendo criatura

- **Criaturas que luchan con armas** (draugr y esqueletos, falmer, rieklings, hombre lobo, autómatas enanos): usan el ataque normal del juego, que respeta el arma que lleves.
- **El resto** (lobo, oso, troll, araña, mudcrab, dragón…) no entiende el ataque del juego. Con ellas:
  - **Clic izquierdo**: un ataque normal de la criatura.
  - **Clic derecho**: un ataque potente. Si la criatura no tiene, o ninguno va hacia esa dirección, otro ataque normal distinto del que daría el clic izquierdo.
  - **La dirección la dan las teclas de movimiento** que tengas pulsadas al hacer clic (ver abajo).
  - Si no estás en guardia, el primer clic te pone en guardia (como sacar el arma). La tecla de sacar el arma (R) funciona como siempre.

Los ataques salen de la lista de ataques de la raza (la misma que usa la IA), elegidos por su probabilidad. Se quitan las embestidas, los ataques a la carrera y los golpes de escudo.

**Ataques por dirección.** Cada ataque tiene un ángulo en sus datos (`attackAngle`, el que usa la IA para atacar según dónde está su objetivo). Al hacer clic, se elige el ataque más cercano a la dirección de las teclas:

| Teclas | Dirección | Dragón, por ejemplo |
|---|---|---|
| Ninguna o **W** | De frente | Mordisco (clic derecho: mordisco lateral) |
| **W + A** / **W + D** | Delante a un lado (±45°) | Mordisco lateral (clic derecho: ala) |
| **A** / **D** | A un lado (±90°) | Ala (clic derecho: cola lateral) |
| **S + A** / **S + D** | Atrás a un lado (±135°) | Cola lateral |
| **S** | Detrás (180°) | Coletazo |

- Se lee la dirección de movimiento que calcula el juego, así que vale igual con las teclas cambiadas o con mando.
- De frente, si avanzas (W) se prefieren los ataques "hacia delante" (los saltos del lobo o del oso), y si no, los demás.
- Si la criatura no tiene ataques hacia esa dirección, sale el más cercano: por ejemplo, el oso no tiene ataques hacia atrás, y con **S** da un zarpazo lateral.
- Algunas razas (lobo, troll) tienen el ángulo a 0 en todos sus ataques. Para ellas se usa el nombre: los `Left`/`Right` cuentan como ±45°.
- Funciona porque, **con el arma desenfundada, el personaje mira hacia la cámara** y las teclas lo mueven sin girarlo: S es de verdad su espalda. Eso depende de la configuración de TrueDirectionalMovement; si la tuya hace girar al personaje con el arma desenfundada, al mantener S la criatura se dará la vuelta antes de atacar. Con la tecla pulsada, la criatura da también un paso en esa dirección justo antes del ataque.

El plugin decide solo a cuál de los dos grupos pertenece cada criatura, mirando si su comportamiento entiende el ataque del juego.

### Acciones

La pestaña **Acciones** lista los eventos de animación de la criatura en la que estás transformado. **Ejecutar** (o doble clic) lanza la animación, y sus sonidos suenan solos porque van dentro de ella. Los nombres son los internos del juego, en inglés (`attackStart_Attack1`, `HowlStart`…).

- Por defecto se ocultan las **peligrosas** (muerte, ragdoll, ejecuciones, muebles…), que pueden dejarte tirado, y las **internas** (pasos, sonidos, golpes), que las animaciones ya lanzan solas. **Mostrar todas** las enseña igualmente; si alguna te deja atascado, usa **Desatascar**.
- **Cerrar el menú al ejecutar** viene activado, para ver a la criatura. Desactívalo para probar varias seguidas.
- Algunas acciones solo funcionan en combate o estando quieto. Si la criatura no hace caso, el menú lo avisa.

## Configuración (`Transformaciones.ini`)

Ejemplo con todas las secciones:

```ini
[General]
; Tecla del menú, en código DIK decimal. 87 = F11. No uses 67 (F9): carga la partida rápida.
Tecla=87

[Ocultar]
; Razas que no salen en el menú, por su EditorID, separadas por comas.
Razas=ChickenRace, HareRace

[Camara]
; La escribe el plugin: "altura distancia" para cada raza.
WolfRace=120 300
```

El EditorID de una raza sale en el registro al transformarte (`Transformar en WolfRace ...`).

**Al actualizar el mod**, no sobrescribas el `.ini` instalado si ya tiene cámaras guardadas: añade solo las secciones nuevas. `compilar.ps1 -Instalar` ya lo hace así.

## Registro

El plugin escribe en `Documentos\My Games\<carpeta del juego>\SKSE\Transformaciones.log`:

- cada transformación y cada vuelta, paso a paso, con la raza, la piel y, con personas, la cara aplicada (sexo, número de partes de la cabeza y de pinturas, peso y altura);
- dónde se guarda el aspecto original y cuándo se lee;
- si el juego no avisó del fin de un cambio de raza (ver [Notas técnicas](#notas-técnicas));
- el estado del jugador al transformarse y al desatascar (controles, IA, bloqueos, marcas de la raza): si alguien se queda atascado, ahí se ve por qué;
- los ataques que se cargan para cada raza, cada ataque lanzado y, si un clic no ataca, el motivo;
- la cámara que se guarda para cada raza.

## Lo que necesita el servidor de SkyMP

Por defecto, **los demás jugadores no ven la transformación**. El cliente de SkyMP solo envía la apariencia del jugador al cerrar el menú de creación de personaje, y el servidor descarta cualquier cambio de apariencia que llegue fuera de ese menú.

### Qué hace ya el cliente

`transformaciones-sync.js` comprueba dos veces por segundo el aspecto del jugador, y cuando cambia avisa al servidor:

1. **Si cambia la apariencia, la apariencia completa** (mensaje `UpdateAppearance`, el mismo que usa SkyMP): raza, sexo, peso, colores, partes de la cabeza, rasgos, preajustes y pinturas. Con una persona, eso es toda su cara, así que en el cliente **para las personas no hace falta nada más** (en el servidor sí: ver abajo). Al volver, se envía el aspecto original completo.
2. **Siempre, un paquete propio con la raza y la piel**, porque la apariencia de SkyMP no incluye la piel:
   ```json
   { "customPacketType": "transformacion", "raceId": 123456, "skinId": 654321 }
   ```
   `skinId` es `0` cuando se usa la piel de la raza (por ejemplo, al volver a la propia).

### Qué falta en el servidor

**No hay que modificar ni recompilar SkyMP.** Solo hay que añadir al **gamemode** (el JavaScript del servidor) el contenido de [`servidor/transformaciones-gamemode.js`](servidor/transformaciones-gamemode.js), pegándolo o cargándolo con `require` desde el gamemode. Hace tres cosas:

1. **Raza:** con `mp.onUpdateAppearanceAttempt`, SkyMP avisa al gamemode de los cambios de apariencia que ha descartado. Si viene de un admin, el código la aplica con `mp.set(actorId, "appearance", appearance)`, y el servidor la reenvía a los jugadores cercanos. Si el gamemode ya tenía su propio `onUpdateAppearanceAttempt`, se sigue llamando.
2. **Piel:** escucha el paquete propio con `mp.on("customPacket", ...)` y guarda la piel en una propiedad del actor, `transformacionesPiel`.
3. **Mostrar la piel a los demás:** la propiedad se crea con `mp.makeProperty`, junto con un código que el servidor manda a los clientes. En el juego de cada jugador cercano, ese código pone la piel en el modelo del transformado y recarga su 3D. Por eso los demás no necesitan el mod.

Qué parte hace falta para cada caso:

| Transformación | Lo que necesita del gamemode | Por qué |
|---|---|---|
| **Persona** | Solo la 1 (raza) | La apariencia de SkyMP ya es su cara completa. El servidor la reenvía y cada jugador la aplica con el sistema normal de SkyMP, el mismo que usa para cualquier personaje |
| **Criatura** | La 1, la 2 y la 3 | La apariencia lleva la raza, pero no la piel, que es lo que distingue un lobo de nieve de un lobo normal o un tipo de draugr de otro |
| **Volver** | La 1, y la 2 para quitar la piel de criatura | Se envía el aspecto original completo y la piel `0` |

Está preparado para que recargar el gamemode no duplique los manejadores. Los nombres de la API (`mp.onUpdateAppearanceAttempt`, `mp.on("customPacket")`, `mp.makeProperty`, `mp.getUserActor`, `mp.set`) están comprobados en el código público de SkyMP.

### Antes de ponerlo en marcha

- **Conectar `esAdmin(actorId)`** con el sistema de admins del gamemode. Ahora devuelve `false`, así que **nadie** puede transformarse hasta que se cambie. Sin esa comprobación, cualquiera con el mod podría transformarse en el servidor.
- **Mismos mods y mismo orden de carga** en todos los jugadores: las razas, pieles y partes de la cabeza de mods se identifican por su FormID, que depende del orden de carga. Si un admin se transforma en un NPC de un mod (o en una persona con pelo o cara de un mod), los demás necesitan ese mod. Las del juego base valen para todos.
- **Probarlo en un servidor de pruebas.** Este código no se ha probado todavía en un servidor real.

### Lo que no se sincroniza (o no se sabe aún)

- **El equipo:** no se copian la ropa, las armaduras ni los cascos del NPC (un guardia o un draugr, por ejemplo). Serían objetos en el inventario, y en SkyMP el inventario lo gestiona el servidor.
- **Las animaciones de ataque y las acciones:** SkyMP envía al servidor las animaciones del jugador, pero el servidor puede descartar las que no conoce. Hay que comprobar cuáles llegan a los demás.
- **El daño:** entre jugadores lo calcula el servidor a partir del arma, así que un transformado pega como si no llevara arma, sea un oso o un mudcrab. Que haga el daño de la criatura habría que resolverlo en el gamemode.
- **Hechizos y gritos de criatura** (aliento de hielo, fuego de dragón…): no son animaciones; el jugador tendría que tener ese hechizo o grito.

## Compilar

Requisitos: Visual Studio 2022 con C++ (MSVC), [vcpkg](https://github.com/microsoft/vcpkg) y el Creation Kit (por su compilador de Papyrus).

```powershell
.\compilar.ps1            # compila el .dll y el .pex, y rehace paquete/
.\compilar.ps1 -Instalar  # además lo copia al juego (con el juego cerrado)
```

El script:

1. Busca Visual Studio con `vswhere` y vcpkg en `VCPKG_ROOT`, `%USERPROFILE%\vcpkg`, `C:\vcpkg` o `C:\dev\vcpkg`.
2. Compila con el preset `release` de `CMakePresets.json`.
3. Compila `TransformacionesAPI.psc` con el compilador de Papyrus del Creation Kit.
4. Rehace `paquete/Transformaciones/` y `paquete/Transformaciones.zip`.
5. Con `-Instalar`, copia el `.dll`, el `.pex` y el `.js` al juego, y añade al `.ini` instalado solo las secciones nuevas, para no perder las cámaras guardadas.

Varias rutas están fijas para el PC donde se desarrolla. Si compilas en otro, cámbialas:

| Dónde | Ruta |
|---|---|
| `CMakePresets.json` (`binaryDir`, `VCPKG_INSTALLED_DIR`) | `C:/Users/ASUS/build/Transformaciones` (fuera de OneDrive, para que no sincronice la compilación) |
| `compilar.ps1` (`$ck`) | La carpeta de Skyrim con el Creation Kit y el compilador de Papyrus |
| `compilar.ps1` (`$juego`) | La carpeta de Skyrim donde se instala con `-Instalar` |

## Notas técnicas

- **Aspecto de las personas.** Se copian a la ficha del jugador los mismos datos que aplica el cliente de SkyMP al cargar una apariencia (`applyAppearanceToPlayer`), y se recarga el modelo. Las pinturas de un NPC son "índice de pintura de su raza + color + intensidad", y las del jugador son una lista de texturas: el plugin las traduce con las pinturas de la raza (`faceRelatedData → tintMasks`). Si un NPC no tiene pintura de tono de piel, se añade con su color de piel. Como en SkyMP, nunca se ponen dos pinturas de guerra visibles a la vez, porque el juego se cierra. Si la raza cambia, la cara se vuelve a poner al terminar el cambio de raza, porque el juego puede dejar las partes y pinturas de la raza nueva.
- **La raza solo la cambia `SetRace`.** Si la ficha del jugador ya tiene la raza nueva cuando se llama, `SetRace` cree que no hay nada que cambiar: no monta el esqueleto nuevo ni avisa al terminar (`TESSwitchRaceCompleteEvent`). Por eso la cara se pone en la ficha sin tocar la raza, y la raza se apunta cuando el cambio ha terminado. Una versión anterior lo hacía al revés y había que pulsar dos veces para transformarse, "Volver" no funcionaba, y la cara que se quedaba esperando caía después encima de una criatura (huesos estirados).
- **La cara pendiente no se queda esperando.** Cada transformación descarta lo pendiente de la anterior, y si el juego no avisa del fin del cambio de raza en 3 segundos, la cara se pone igualmente (queda anotado en el registro).
- **El cambio de raza se hace con Papyrus, no con la consola.** El cliente de SkyMP intercepta la consola, y con `setrace` el juego llegó a cerrarse. El plugin llama a `TransformacionesAPI.Transformar`, que usa `Actor.SetRace`. Antes, el plugin pone la piel del NPC en la base del jugador, para que `SetRace` cargue el modelo con ella; si la raza no cambia (de lobo a lobo de nieve), recarga el modelo él mismo.
- **Controles y versión del juego.** Desde la 1.6.1130, la estructura interna del juego que guarda los controles activos (`ControlMap`) es 8 bytes más grande, y CommonLibSSE-NG 3.6 no lo tiene en cuenta. Usar sus funciones de controles tocaba otro dato: el jugador se quedaba sin control y el juego llegó a cerrarse. El plugin lee y cambia esos campos en su sitio correcto según la versión que se ejecuta. **No uses `ControlMap::ToggleControls`, `AreControlsEnabled` ni `enabledControls` de esta versión de CommonLib.**
- **No se quita el control de lucha del juego siendo criatura.** Con él quitado, el juego obliga a guardar el arma y la criatura salía de la guardia al instante.
- **Lectura de los eventos de animación.** Para la pestaña Acciones y para saber si una criatura entiende el ataque del juego, el plugin lee la lista de eventos del grafo de comportamiento cargado. Antes comprueba que la estructura sea la esperada; si no lo es, se queda sin la lista en vez de cerrar el juego.
- **El menú** se dibuja con ImGui sobre DirectX 11 (enganchado a `Present`), y el ratón y el teclado se leen de los eventos de entrada del propio juego.
