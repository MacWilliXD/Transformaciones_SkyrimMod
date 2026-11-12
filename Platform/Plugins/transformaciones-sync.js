"use strict";
// Transformaciones: lleva al servidor de SkyMP las peticiones del menú (F11).
//
// En modo servidor ([Servidor] Modo=1 de Transformaciones.ini) el menú no transforma al jugador: Transformaciones.dll
// manda un ModEvent «Transformaciones_Peticion» con un JSON, y este plugin lo envía al servidor como paquete propio:
//   { customPacketType: "transformacion", version: 4, accion: "persona", apariencia }   (la cara completa del NPC)
//   { customPacketType: "transformacion", version: 4, accion: "criatura", raceId, skinId }
//   { customPacketType: "transformacion", version: 4, accion: "volver" }
// El servidor comprueba el permiso y lo aplica a todos los jugadores, también a quien lo pide: lo que se ve en el
// propio juego es lo que ha aceptado el servidor, igual que lo ven los demás. Ver servidor/transformaciones.ts.
//
// En modo local (Modo=0) la DLL transforma al jugador ella misma y este plugin no hace nada.

var sp = globalThis.skyrimPlatform;

var MSG_CUSTOM_PACKET = 1;
var EVENTO = "Transformaciones_Peticion";
var ACCIONES = { persona: true, criatura: true, volver: true };

// También a Data/Platform/Logs/Transformaciones-logs.txt (el mismo archivo que el código del servidor).
function anotar(texto) {
  try {
    sp.writeLogs("Transformaciones", new Date().toISOString() + " plugin: " + texto);
  } catch (e) {
    // sin archivo
  }
}

function log() {
  var args = ["[Transformaciones]"].concat(Array.prototype.slice.call(arguments));
  sp.printConsole.apply(null, args);
  anotar(Array.prototype.slice.call(arguments).join(" "));
}

function razaDelJugador() {
  try {
    var r = sp.Game.getPlayer().getRace();
    return r ? r.getFormID().toString(16) : "?";
  } catch (e) {
    return "?";
  }
}

// Aviso en la esquina de la pantalla: en este servidor la consola no la puede abrir todo el mundo.
function avisar(texto) {
  log(texto);
  try {
    sp.Debug.notification("Transformaciones: " + texto);
  } catch (e) {
    // sin avisos en pantalla
  }
}

function alPedir(e) {
  if (!e || e.eventName !== EVENTO) return;
  var peticion;
  try {
    peticion = JSON.parse(String(e.strArg));
  } catch (err) {
    avisar("la petición del menú no se entiende");
    return;
  }
  if (!peticion || !ACCIONES[peticion.accion]) return;
  if (!sp.mpClientPlugin.isConnected()) {
    avisar("no estás conectado a un servidor (para probar sin servidor, pon Modo=0 en Transformaciones.ini)");
    return;
  }
  peticion.customPacketType = "transformacion";
  peticion.version = 4;
  sp.mpClientPlugin.send(JSON.stringify({ t: MSG_CUSTOM_PACKET, contentJsonDump: JSON.stringify(peticion) }), true);
  if (peticion.accion === "volver") avisar("pedido al servidor volver a tu aspecto");
  else avisar("transformación pedida al servidor (" + peticion.accion + ")");
  // transformaciones5: si a los 6 s no ha cambiado nada, el código del servidor no ha llegado a este juego.
  var antes = razaDelJugador();
  anotar("petición " + JSON.stringify({ accion: peticion.accion, raceId: peticion.raceId, skinId: peticion.skinId }) + ", raza antes " + antes);
  var hasta = Date.now() + 6000;
  var vigilar = sp.on("update", function () {
    if (Date.now() < hasta) return;
    hasta = Infinity; // aunque no se pueda quitar el manejador, ya no vuelve a anotar
    try {
      sp.unsubscribe(vigilar);
    } catch (err) {
      // versión de Skyrim Platform sin unsubscribe
    }
    anotar("6 s después de pedir " + peticion.accion + ": raza " + razaDelJugador() + " (antes " + antes + ")");
  });
}

sp.on("modEvent", function (e) {
  try {
    alPedir(e);
  } catch (err) {
    log("Error:", err && err.message ? err.message : String(err));
  }
});

log("plugin de peticiones v5 cargado");
