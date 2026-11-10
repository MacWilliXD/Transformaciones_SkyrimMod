Scriptname TransformacionesAPI Hidden
{Lo llama el plugin Transformaciones.dll: cambiar la raza con Papyrus evita pasar por la consola,
 que el cliente de SkyMP intercepta.}

; El plugin ya ha puesto la piel nueva en la base del actor, y SetRace recarga el modelo con ella.
Function Transformar(Actor akActor, Race akRaza) Global
	akActor.SetRace(akRaza)
EndFunction

; Devuelve el control al jugador cuando una criatura lo deja quieto. abCompleto se usa desde el
; botón Desatascar: quita también el control por IA, que el juego pone en escenas.
Function Desatascar(Actor akActor, Bool abCompleto) Global
	If abCompleto
		Game.SetPlayerAIDriven(False)
	EndIf
	Game.EnablePlayerControls()
	akActor.SetDontMove(False)
	akActor.SetRestrained(False)
	Debug.SendAnimationEvent(akActor, "IdleForceDefaultState")
EndFunction
