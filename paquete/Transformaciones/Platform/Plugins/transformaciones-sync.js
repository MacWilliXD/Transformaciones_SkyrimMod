"use strict";
// Transformaciones: avisa al servidor de SkyMP cuando cambia la raza del jugador, para que los
// demás jugadores lo vean transformado.
//
// El cliente de SkyMP solo envía la apariencia al salir del menú de creación de personaje, así
// que el cambio de raza que hace Transformaciones.dll no le llega a nadie. Este plugin de Skyrim
// Platform manda el mismo mensaje (UpdateAppearance) que el cliente, cada vez que la raza cambia.
//
// La apariencia se envía completa, con la cara y el pelo de siempre: así, al volver a la raza
// original, el servidor la sigue teniendo entera.
//
// La apariencia de SkyMP no lleva la piel (el cuerpo de la criatura: lobo de nieve, cada tipo de
// draugr...). Va aparte, en un paquete propio que el gamemode del servidor puede leer:
//   { customPacketType: "transformacion", raceId, skinId }   (skinId 0: la piel de la raza)
// Ver servidor/transformaciones-gamemode.js.

var sp = globalThis.skyrimPlatform;

var MSG_CUSTOM_PACKET = 1;
var MSG_UPDATE_APPEARANCE = 4;
var ESPERA_TRAS_CONECTAR_MS = 5000;  // a que el cliente aplique la apariencia del servidor
var INTERVALO_MS = 500;

var razaEnviada = 0;     // la raza que el servidor ya conoce; 0 = aún sin referencia
var pielEnviada = 0;     // la piel de la base del jugador que el servidor ya conoce
var idxConectado = -1;   // idx del jugador en el servidor
var conectadoDesde = 0;
var ultimaComprobacion = 0;

function log() {
  var args = ["[Transformaciones]"].concat(Array.prototype.slice.call(arguments));
  sp.printConsole.apply(null, args);
}

// El cliente de SkyMP guarda su modelo del mundo en storage, que comparten todos los plugins.
function idxDelJugador() {
  var mundo = sp.storage["worldModel"];
  if (!mundo || typeof mundo !== "object" || !mundo.forms) return -1;
  var form = mundo.forms[mundo.playerCharacterFormIdx];
  return form && typeof form.idx === "number" ? form.idx : -1;
}

// La piel puesta en la base del jugador (Transformaciones.dll pone la del NPC); 0 si no tiene.
function pielDe(actor) {
  var piel = sp.ActorBase.from(actor.getBaseObject()).getSkin();
  return piel ? piel.getFormID() : 0;
}

// Igual que getAppearance del cliente de SkyMP.
function apariencia(actor) {
  var base = sp.ActorBase.from(actor.getBaseObject());
  var pelo = base.getHairColor();
  var piel = sp.TESModPlatform.getSkinColor(base);
  var a = {
    isFemale: base.getSex() === 1,
    raceId: base.getRace() ? base.getRace().getFormID() : 0,
    weight: base.getWeight(),
    hairColor: pelo ? pelo.getColor() : 0,
    headpartIds: [],
    headTextureSetId: base.getFaceTextureSet() ? base.getFaceTextureSet().getFormID() : 0,
    options: new Array(19),
    presets: new Array(4),
    tints: [],
    skinColor: piel ? piel.getColor() : 0,
    name: actor.getBaseObject().getName(),
  };
  var i;
  for (i = 0; i < base.getNumHeadParts(); ++i) {
    var parte = base.getNthHeadPart(i);
    if (parte) a.headpartIds.push(parte.getFormID());
  }
  for (i = 0; i < a.options.length; ++i) a.options[i] = base.getFaceMorph(i);
  for (i = 0; i < a.presets.length; ++i) a.presets[i] = base.getFacePreset(i);
  for (i = 0; i < sp.Game.getNumTintMasks(); ++i) {
    a.tints.push({
      texturePath: sp.Game.getNthTintMaskTexturePath(i),
      type: sp.Game.getNthTintMaskType(i),
      argb: sp.Game.getNthTintMaskColor(i),
    });
  }
  return a;
}

function comprobar() {
  var jugador = sp.Game.getPlayer();
  var idx = sp.mpClientPlugin.isConnected() ? idxDelJugador() : -1;
  if (!jugador || idx < 0) {
    razaEnviada = 0;
    pielEnviada = 0;
    idxConectado = -1;
    return;
  }
  var ahora = Date.now();
  if (idx !== idxConectado) {
    idxConectado = idx;
    conectadoDesde = ahora;
    razaEnviada = 0;
  }
  var raza = jugador.getRace();
  if (!raza) return;
  var idRaza = raza.getFormID();
  var idPiel = pielDe(jugador);

  // En el menú de creación, la apariencia la envía el propio cliente al cerrarlo.
  if (sp.Ui.isMenuOpen("RaceSex Menu") || ahora - conectadoDesde < ESPERA_TRAS_CONECTAR_MS || razaEnviada === 0) {
    razaEnviada = idRaza;
    pielEnviada = idPiel;
    return;
  }
  if (idRaza === razaEnviada && idPiel === pielEnviada) return;

  if (idRaza !== razaEnviada) {
    var datos = apariencia(jugador);
    // SetRace cambia la raza del actor; la de su base puede seguir siendo la original.
    datos.raceId = idRaza;
    sp.mpClientPlugin.send(JSON.stringify({ t: MSG_UPDATE_APPEARANCE, data: datos, idx: idx }), true);
  }
  var paquete = { customPacketType: "transformacion", raceId: idRaza, skinId: idPiel };
  sp.mpClientPlugin.send(JSON.stringify({ t: MSG_CUSTOM_PACKET, contentJsonDump: JSON.stringify(paquete) }), true);
  log("Enviado al servidor: raza", idRaza.toString(16), "piel", idPiel.toString(16));
  razaEnviada = idRaza;
  pielEnviada = idPiel;
}

sp.on("update", function () {
  var ahora = Date.now();
  if (ahora - ultimaComprobacion < INTERVALO_MS) return;
  ultimaComprobacion = ahora;
  try {
    comprobar();
  } catch (e) {
    log("Error:", e && e.message ? e.message : String(e));
  }
});
