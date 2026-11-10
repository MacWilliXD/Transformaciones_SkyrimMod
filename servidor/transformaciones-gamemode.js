"use strict";
// Transformaciones: lo que tiene que añadir el gamemode del servidor de SkyMP para que todos vean
// a un admin transformado. Se pega en el gamemode (o se carga con require desde él).
//
// El cliente con Transformaciones manda dos cosas cada vez que se transforma:
//   1. La apariencia (UpdateAppearance) con la raza nueva. SkyMP la descarta fuera del menú de
//      creación de personaje, pero avisa al gamemode con onUpdateAppearanceAttempt: aquí se
//      aplica a mano con mp.set(..., "appearance", ...), y el servidor la reenvía a los demás.
//   2. Un paquete propio { customPacketType: "transformacion", raceId, skinId } con la piel
//      (el cuerpo de la criatura: lobo de nieve, cada tipo de draugr...), que la apariencia de
//      SkyMP no lleva. Se guarda en la propiedad "transformacionesPiel", y el código de
//      updateNeighbor la pone en el modelo del admin en el juego de cada jugador cercano.
//
// Los demás jugadores no necesitan el mod: el código de la propiedad lo manda el servidor. Sí
// necesitan los mods de los que sea la raza o la piel, con el mismo orden de carga (los FormID
// de los mods dependen de él).
//
// Sin probar todavía en un servidor.

// Quién puede transformarse. Hay que conectarlo con el sistema de admins del gamemode: mientras
// devuelva false, nadie puede.
function esAdmin(actorId) {
  return false;
}

// ---- Piel -----------------------------------------------------------------------------------

// Se ejecuta en el cliente de cada jugador, cada fotograma, para cada admin cercano con la
// propiedad. ctx.value es el FormID de la piel; 0 es la de la raza.
const ACTUALIZAR_PIEL = `
  const actor = ctx.sp.Actor.from(ctx.refr);
  if (!actor) return;
  const base = ctx.sp.ActorBase.from(actor.getBaseObject());
  if (!base) return;
  const id = ctx.value || 0;
  const actual = base.getSkin();
  if ((actual ? actual.getFormID() : 0) === id) return;
  // Si la piel no existe en este juego, no se reintenta en cada fotograma.
  if (ctx.state.transformacionesPiel === id && ctx.state.transformacionesBase === base.getFormID()) return;
  ctx.state.transformacionesPiel = id;
  ctx.state.transformacionesBase = base.getFormID();
  base.setSkin(id ? ctx.sp.Armor.from(ctx.sp.Game.getFormEx(id)) : null);
  if (actor.is3DLoaded()) actor.queueNiNodeUpdate();
`;

try {
  mp.makeProperty("transformacionesPiel", {
    isVisibleByOwner: true,      // también al propio admin, por si vuelve a conectarse transformado
    isVisibleByNeighbors: true,
    updateOwner: ACTUALIZAR_PIEL,
    updateNeighbor: ACTUALIZAR_PIEL,
  });
} catch (e) {
  // Al recargar el gamemode la propiedad puede existir ya.
  console.log("[Transformaciones] makeProperty:", e && e.message ? e.message : e);
}

// SkyMP recarga el gamemode cuando cambia, y mp.on no se puede deshacer: se registra una vez y
// se llama siempre a la versión más reciente del manejador.
globalThis.transformacionesPaquete = (userId, contenido) => {
  let datos;
  try {
    datos = typeof contenido === "string" ? JSON.parse(contenido) : contenido;
  } catch (e) {
    return;
  }
  if (!datos || datos.customPacketType !== "transformacion") return;
  const actorId = mp.getUserActor(userId);
  if (!actorId || !esAdmin(actorId)) return;
  const piel = Number(datos.skinId) || 0;
  mp.set(actorId, "transformacionesPiel", piel);
  console.log(`[Transformaciones] ${actorId.toString(16)}: piel ${piel.toString(16)}`);
};
if (!globalThis.transformacionesPaqueteRegistrado) {
  globalThis.transformacionesPaqueteRegistrado = true;
  mp.on("customPacket", (userId, contenido) => globalThis.transformacionesPaquete(userId, contenido));
}

// ---- Raza -----------------------------------------------------------------------------------

// Solo hay una función por evento en mp: si el gamemode ya tenía una, se sigue llamando (sin
// encadenar la nuestra de una carga anterior).
let onUpdateAppearanceAttemptAnterior = mp.onUpdateAppearanceAttempt;
if (onUpdateAppearanceAttemptAnterior && onUpdateAppearanceAttemptAnterior.deTransformaciones) {
  onUpdateAppearanceAttemptAnterior = onUpdateAppearanceAttemptAnterior.anterior;
}

const alIntentarApariencia = (actorId, appearance, isAllowed) => {
  if (typeof onUpdateAppearanceAttemptAnterior === "function") {
    onUpdateAppearanceAttemptAnterior(actorId, appearance, isAllowed);
  }
  // isAllowed: venía del menú de creación y SkyMP ya la ha aplicado.
  if (isAllowed || !esAdmin(actorId)) return;
  mp.set(actorId, "appearance", appearance);
  console.log(`[Transformaciones] ${actorId.toString(16)}: raza ${appearance.raceId.toString(16)}`);
  // No devolver false: SkyMP no admite bloquear este evento.
};
alIntentarApariencia.deTransformaciones = true;
alIntentarApariencia.anterior = onUpdateAppearanceAttemptAnterior;
mp.onUpdateAppearanceAttempt = alIntentarApariencia;
