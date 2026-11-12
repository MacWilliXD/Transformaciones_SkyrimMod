import { pool } from "./db";
import { log } from "./log";
import { atender, enviar } from "./red";
import { profileDe } from "./jugadores";

// transformaciones1 · Mod Transformaciones (F11): un admin se convierte en un NPC o una criatura y todos lo ven.
// transformaciones4 · El servidor manda: el menú del mod ya no transforma a nadie, solo pide (paquete «transformacion»,
// por Data/Platform/Plugins/transformaciones-sync.js). Aquí se comprueba el rol y se aplica a todos, también a quien lo
// pide: lo que ve el admin es lo que ha aceptado el servidor, igual que los demás.
//   - Persona: se guarda el aspecto original y se pone el nuevo con mp.set(actor, "appearance"). SkyMP lo reparte a
//     todos, y el cliente de cada uno lo aplica con su sistema normal de caras (al propio jugador también).
//   - Criatura: la apariencia NO se toca (el cliente la pondría con el sistema de las personas y la criatura salía
//     deforme o cerraba el juego). La raza y la piel van en la propiedad transformacionesCriatura: su código cambia la
//     raza (SetRace) en el juego del propio admin (updateOwner) y en el de cada jugador cercano (updateNeighbor).
//   - Mods: valen razas, pieles y piezas de cualquier plugin. Con el launcher de Aestherius todos los jugadores cargan
//     los plugins en el orden del servidor, así que los números son los mismos en todos. Solo la raza de una persona
//     necesita un arreglo: si el servidor no tiene esa raza (un .esl, que no carga), guardarla como raza impediría
//     entrar al personaje; se guarda la nórdica y la de verdad delante de las piezas, detrás de CENTINELA, como hace
//     el creador (creador.ts, razaServidor1). El cliente la saca de ahí.
//   - Al volver se pone el aspecto guardado aquí, no el que diga el cliente.
//   - Si alguien sale o se le cierra el juego transformado, recupera su aspecto al entrar.

const ROLES = new Set(["admin", "superadmin"]);
const PROP_ORIGINAL = "private.aesTransfOriginal"; // la apariencia de antes de transformarse en una persona
const PROP_CRIATURA = "transformacionesCriatura"; // { raza, piel } o null
// transformaciones1 ponía la piel con esta propiedad. Se mantiene con la misma piel que PROP_CRIATURA: su código
// sigue corriendo en los clientes y, si no, la desharía.
const PROP_PIEL = "transformacionesPiel";
const MIN_ENTRE_PAQUETES_MS = 300;
const MAX_CAPAS = 16; // como el creador
const CENTINELA = 0xf6000000; // creador.ts: la raza de verdad va detrás, en las piezas
const RAZA_SERVIDOR = 0x013746; // nórdica: la que se guarda como raza si el servidor no tiene la de verdad

type Apariencia = Record<string, unknown> & { raceId: number; headpartIds: number[]; name?: unknown };

// Avisos en la pantalla de quien lo ve (para probar). false los apaga; hace falta reiniciar el servidor.
const TF_AVISOS = true;

// Lo común del código que corre en los clientes: cambia la raza y la piel de un actor y se acuerda de las suyas.
// ctx.value es { raza, piel } o nada (volver). En ctx.state se guarda lo aplicado y la raza y piel propias.
// transformaciones5: además, todo queda en Data/Platform/Logs/Transformaciones-logs.txt del jugador (la consola se
// llena enseguida): si el código se carga, qué valor recibe y qué hace.
const CAMBIAR = (quien: string) => `
  const anotar = (t) => {
    if (!${TF_AVISOS}) return;
    try { ctx.sp.writeLogs("Transformaciones", new Date().toISOString() + " ${quien}: " + t); } catch (e) {}
  };
  const avisar = (t) => {
    if (!${TF_AVISOS}) return;
    anotar(t);
    try { ctx.sp.printConsole("[Transformaciones] ${quien}: " + t); ctx.sp.Debug.notification("Transformaciones: ${quien}: " + t); } catch (e) {}
  };
  try {
    const visto = JSON.stringify(ctx.value === undefined ? "sin valor" : ctx.value);
    if (!ctx.state.tfCargado) { ctx.state.tfCargado = true; anotar("código cargado (actor " + (actor ? actor.getFormID().toString(16) : "ninguno") + ")"); }
    if (ctx.state.tfVisto !== visto) { ctx.state.tfVisto = visto; anotar("valor recibido " + visto); }
    if (!actor || !actor.is3DLoaded()) {
      if (!ctx.state.tfSin3D) { ctx.state.tfSin3D = true; anotar("esperando a que cargue el modelo"); }
      return;
    }
    ctx.state.tfSin3D = false;
    const v = ctx.value && typeof ctx.value === "object" ? ctx.value : null;
    const raza = v && v.raza ? v.raza >>> 0 : 0;
    const piel = v && v.piel ? v.piel >>> 0 : 0;
    const id = actor.getFormID();
    const clave = raza + ":" + piel;
    const base = ctx.sp.ActorBase.from(actor.getBaseObject());
    const actual = actor.getRace();
    const idActual = actual ? actual.getFormID() : 0;
    if (ctx.state.tfActor !== id) {
      // Actor nuevo (o recién conectado): lo que tiene ahora es lo suyo.
      ctx.state.tfActor = id;
      ctx.state.tfClave = "0:0";
      ctx.state.tfPropia = idActual;
      const p = base ? base.getSkin() : null;
      ctx.state.tfPielPropia = p ? p.getFormID() : 0;
      ctx.state.tfAvisadoPerdida = false;
    }
    if (ctx.state.tfClave === clave) {
      if (raza && idActual !== raza && !ctx.state.tfAvisadoPerdida) {
        ctx.state.tfAvisadoPerdida = true;
        avisar("la raza puesta (" + raza.toString(16) + ") se ha perdido: ahora " + idActual.toString(16));
      }
      return;
    }
    if (ctx.state.tfClave === "0:0" && raza) {
      // Primera transformación: lo de ahora es lo propio, para volver.
      ctx.state.tfPropia = idActual;
      const p = base ? base.getSkin() : null;
      ctx.state.tfPielPropia = p ? p.getFormID() : 0;
    }
    ctx.state.tfClave = clave;
    ctx.state.tfAvisadoPerdida = false;
    if (!base) { avisar("sin ficha base"); return; }
    const idDestino = raza || ctx.state.tfPropia;
    const destino = ctx.sp.Race.from(ctx.sp.Game.getFormEx(idDestino));
    if (!destino) { avisar("la raza " + idDestino.toString(16) + " no existe en este juego"); return; }
    const idPiel = raza ? piel : ctx.state.tfPielPropia;
    base.setSkin(idPiel ? ctx.sp.Armor.from(ctx.sp.Game.getFormEx(idPiel)) : null);
    if (idActual === destino.getFormID()) actor.queueNiNodeUpdate(); // solo cambia la piel
    else actor.setRace(destino);
    const despues = actor.getRace();
    avisar((raza ? "criatura " : "vuelta ") + destino.getFormID().toString(16) + " piel " + idPiel.toString(16)
      + " (raza ahora " + (despues ? despues.getFormID().toString(16) : "?") + ")");
  } catch (e) {
    avisar("error: " + (e && e.message ? e.message : e));
  }
`;
// El propio admin: su jugador. Los demás: el actor que lo representa en su juego.
const CRIATURA_DUENO = `const actor = ctx.sp.Game.getPlayer();\n${CAMBIAR("tú")}`;
const CRIATURA_VECINO = `const actor = ctx.sp.Actor.from(ctx.refr);\n${CAMBIAR("otro")}`;

// Código de transformaciones1 (ver PROP_PIEL). ctx.value es el número de la piel; 0, la de la raza.
const ACTUALIZAR_PIEL = `
  const actor = ctx.sp.Actor.from(ctx.refr);
  if (!actor) return;
  const base = ctx.sp.ActorBase.from(actor.getBaseObject());
  if (!base) return;
  const id = ctx.value || 0;
  if (!id) return; // transformaciones4: la piel de volver la pone transformacionesCriatura (la propia del actor)
  const actual = base.getSkin();
  if ((actual ? actual.getFormID() : 0) === id) return;
  if (ctx.state.transformacionesPiel === id && ctx.state.transformacionesBase === base.getFormID()) return;
  ctx.state.transformacionesPiel = id;
  ctx.state.transformacionesBase = base.getFormID();
  base.setSkin(ctx.sp.Armor.from(ctx.sp.Game.getFormEx(id)));
  if (actor.is3DLoaded()) actor.queueNiNodeUpdate();
`;

const ultimoPaquete = new Map<number, number>();

const entero = (x: unknown): x is number => typeof x === "number" && Number.isInteger(x);
const numero = (x: unknown): x is number => typeof x === "number" && Number.isFinite(x);
const color = (x: unknown) => entero(x) && x >= -0x80000000 && x <= 0xffffffff;
const formId = (x: unknown) => entero(x) && x > 0 && x <= 0xffffffff;

// Si el servidor tiene ese registro y es del tipo pedido (lo de un .esl o un plugin que no carga, no lo tiene).
function servidorTiene(id: number, tipo: string): boolean {
  try {
    const r = (mp as unknown as { lookupEspmRecordById(id: number): { record?: { type?: string } } }).lookupEspmRecordById(id).record;
    return r?.type === tipo;
  } catch {
    return false;
  }
}

async function puedeTransformarse(profileId: number): Promise<boolean> {
  const r = await pool.query<{ rol: string }>(
    "SELECT c.rol FROM personajes p JOIN cuentas c ON c.discord_id = p.discord_id WHERE p.profile_id = $1", [profileId]);
  return ROLES.has(r.rows[0]?.rol ?? "");
}

// Deja la apariencia de persona que manda el mod en lo que el servidor puede guardar. Devuelve el motivo si no vale.
function limpiar(a: Record<string, unknown>): Apariencia | string {
  if (typeof a.isFemale !== "boolean") return "falta la cara (¿el mod está al día?)";
  const raza = Number(a.raceId) >>> 0;
  if (!formId(raza)) return "raza";
  if (!numero(a.weight) || a.weight < 0 || a.weight > 100) return "peso";
  if (!color(a.skinColor) || !color(a.hairColor)) return "color";
  const piezas = a.headpartIds;
  if (!Array.isArray(piezas) || piezas.length > 120 || !piezas.every(formId)) return "piezas";
  const complexion = Number(a.headTextureSetId) >>> 0;
  if (complexion !== 0 && !formId(complexion)) return "complexión";
  const op = a.options;
  if (!Array.isArray(op) || op.length !== 19) return "morfos";
  const pr = a.presets;
  if (!Array.isArray(pr) || pr.length !== 4 || !pr.every(entero)) return "tipos";
  if (!Array.isArray(a.tints) || a.tints.length > 300) return "capas";
  const capas: { texturePath: string; type: number; argb: number }[] = [];
  let guerra = 0;
  for (const t of a.tints as Record<string, unknown>[]) {
    if (!t || typeof t.texturePath !== "string" || t.texturePath.length > 260 || !/^[^<>"\u0000-\u001f]+\.dds$/i.test(t.texturePath)) return "capa";
    if (!entero(t.type) || !entero(t.argb)) return "capa";
    if (((t.argb >>> 24) & 0xff) === 0) continue; // apagada
    if (/\\TintMasks\\.*WarPaint/i.test(t.texturePath) && ++guerra > 1) continue; // dos pinturas de guerra cierran el juego
    if (capas.length < MAX_CAPAS) capas.push({ texturePath: t.texturePath, type: t.type, argb: t.argb });
  }
  let headpartIds = (piezas as number[]).map((p) => p >>> 0);
  let raceId = raza;
  if (!servidorTiene(raza, "RACE")) { // como el creador: la de verdad, delante de las piezas
    raceId = RAZA_SERVIDOR;
    headpartIds = [CENTINELA, raza, ...headpartIds];
  }
  return {
    isFemale: a.isFemale, raceId, weight: a.weight, skinColor: a.skinColor, hairColor: a.hairColor, headpartIds,
    headTextureSetId: complexion,
    // Los NPC tienen rasgos sin dato (valores disparatados): a 0, en vez de rechazar la cara entera.
    options: op.map((v) => (numero(v) && v >= -10 && v <= 10 ? v : 0)), presets: pr, tints: capas,
  };
}

function apuntar(profileId: number, texto: string): void {
  pool.query(
    `INSERT INTO registro(tipo, actor_personaje, texto)
     SELECT 'admin_transformar', $1, format('%s (%s) %s', coalesce(nombre, '?'), $1::bigint, $2::text)
       FROM personajes WHERE profile_id = $1`, [profileId, texto])
    .catch((e) => log(`transformaciones: error apuntando en el registro lo de ${profileId}:`, (e as Error).message));
}

// Quita la criatura (raza y piel) a un actor. true si la tenía.
function quitarCriatura(actorId: number): boolean {
  const c = mp.get(actorId, PROP_CRIATURA);
  const tenia = !!c && typeof c === "object";
  if (tenia) mp.set(actorId, PROP_CRIATURA, null);
  if (mp.get(actorId, PROP_PIEL)) mp.set(actorId, PROP_PIEL, 0);
  return tenia;
}

// Devuelve su cara a un actor transformado en otra persona. true si lo estaba.
function quitarPersona(actorId: number): boolean {
  const original = mp.get(actorId, PROP_ORIGINAL);
  if (!original || typeof original !== "object") return false;
  mp.set(actorId, "appearance", original);
  mp.set(actorId, PROP_ORIGINAL, null);
  return true;
}

function restaurar(actorId: number): boolean {
  const criatura = quitarCriatura(actorId);
  const persona = quitarPersona(actorId);
  return criatura || persona;
}

// La acción del paquete. Los plugins de antes de transformaciones4 mandaban volver/criatura en vez de accion.
function accionDe(datos: Record<string, unknown>): string {
  if (typeof datos.accion === "string") return datos.accion;
  if (datos.volver === true) return "volver";
  return datos.criatura === true ? "criatura" : "persona";
}

async function alPaquete(userId: number, datos: Record<string, unknown>): Promise<void> {
  const ahora = Date.now();
  if (ahora - (ultimoPaquete.get(userId) ?? 0) < MIN_ENTRE_PAQUETES_MS) return;
  ultimoPaquete.set(userId, ahora);
  const profileId = profileDe(userId);
  const actorId = mp.getUserActor(userId);
  if (profileId === null || !actorId) return;
  if (!(await puedeTransformarse(profileId))) {
    log(`transformaciones: ${profileId} lo intenta sin ser admin; no se aplica`);
    enviar(userId, "aestheriusAviso", { texto: "Solo los administradores pueden transformarse" });
    return;
  }
  if (mp.getUserActor(userId) !== actorId) return; // ha salido mientras se miraba su rol
  const rechazar = (motivo: string) => {
    log(`transformaciones: la de ${profileId} no se aplica (${motivo})`);
    enviar(userId, "aestheriusAviso", { texto: `No se puede hacer esta transformación: ${motivo}` });
  };
  const accion = accionDe(datos);

  if (accion === "volver") {
    if (restaurar(actorId)) {
      log(`transformaciones: ${profileId} vuelve a su aspecto`);
      apuntar(profileId, "vuelve a su aspecto");
    }
    return;
  }

  if (accion === "criatura") {
    const raza = Number(datos.raceId) >>> 0;
    const piel = Number(datos.skinId) >>> 0;
    if (!formId(raza)) return rechazar("raza");
    if (piel !== 0 && !formId(piel)) return rechazar("piel");
    quitarPersona(actorId); // venía de otra persona: la cara vuelve a ser la suya
    mp.set(actorId, PROP_PIEL, piel);
    mp.set(actorId, PROP_CRIATURA, { raza, piel });
    const texto = `criatura, raza ${raza.toString(16)}, piel ${piel.toString(16)}`;
    log(`transformaciones: ${profileId} se transforma (${texto})`);
    apuntar(profileId, `se transforma (${texto})`);
    return;
  }

  if (accion !== "persona") return rechazar("petición desconocida");
  const entrada = datos.apariencia && typeof datos.apariencia === "object" ? (datos.apariencia as Record<string, unknown>) : {};
  const nueva = limpiar(entrada);
  if (typeof nueva === "string") return rechazar(nueva);
  const actual = mp.get(actorId, "appearance") as Apariencia | null;
  if (!actual || typeof actual !== "object") return; // personaje sin hacer
  quitarCriatura(actorId);
  const guardada = mp.get(actorId, PROP_ORIGINAL) as Apariencia | null;
  const original = guardada && typeof guardada === "object" ? guardada : actual;
  if (original === actual) mp.set(actorId, PROP_ORIGINAL, JSON.parse(JSON.stringify(actual)));
  nueva.name = original.name; // el nombre del personaje no cambia
  if (JSON.stringify(nueva) !== JSON.stringify(actual)) mp.set(actorId, "appearance", nueva);
  const real = nueva.headpartIds[0] === CENTINELA ? nueva.headpartIds[1] : nueva.raceId;
  const texto = `persona, raza ${real.toString(16)}${real !== nueva.raceId ? " (guardada como nórdica)" : ""}`;
  log(`transformaciones: ${profileId} se transforma (${texto})`);
  apuntar(profileId, `se transforma (${texto})`);
}

// Al entrar al servidor (index.ts): quien salió transformado recupera su aspecto.
export function alEntrarTransformaciones(profileId: number): void {
  for (const actorId of mp.getActorsByProfileId(profileId)) {
    try {
      if (restaurar(actorId)) log(`transformaciones: ${profileId} entra y recupera su aspecto (salió transformado)`);
      // transformaciones5: un valor (null) para que el código de la propiedad se ejecute una vez en su juego y anote que
      // está cargado (sin valor no se ejecuta).
      if (mp.get(actorId, PROP_CRIATURA) === undefined) mp.set(actorId, PROP_CRIATURA, null);
    } catch (e) {
      log(`transformaciones: error devolviendo su aspecto a ${profileId}:`, (e as Error).message);
    }
  }
}

function propiedad(nombre: string, dueno: string, vecino: string): void {
  try {
    mp.makeProperty(nombre, { isVisibleByOwner: !!dueno, isVisibleByNeighbors: true, updateOwner: dueno, updateNeighbor: vecino });
  } catch (e) {
    // Al recargar el gamemode ya existe, con el código de cuando arrancó el servidor: el nuevo, al reiniciarlo.
    log(`transformaciones: makeProperty ${nombre}: ${(e as Error).message}`);
  }
}

export function iniciarTransformaciones(): void {
  propiedad(PROP_PIEL, "", ACTUALIZAR_PIEL);
  propiedad(PROP_CRIATURA, CRIATURA_DUENO, CRIATURA_VECINO);
  atender("transformacion", (userId, datos) => {
    alPaquete(userId, datos).catch((e) => log("transformaciones: error atendiendo el paquete:", (e as Error).message));
  });
}
