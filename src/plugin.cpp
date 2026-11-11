// Transformaciones: un menú de administración para convertirse en cualquier personaje o criatura.
//
// Con la tecla configurada (F11 por defecto) se abre una ventana con un buscador de todos los
// NPC cargados. Al elegir uno, el jugador pasa a la raza de ese NPC con el mismo comando que la
// consola: `setrace`. "Volver a mi raza" deshace el cambio; la raza original se guarda en la
// partida.
//
// La ventana se dibuja con ImGui enganchado al Present de DirectX. El ratón y el teclado se leen
// de los eventos de entrada del propio juego, y mientras está abierta se desactivan los controles
// para que escribir no mueva al personaje.

#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <windows.h>

#include <array>
#include <bit>
#include <fstream>
#include <optional>
#include <sstream>
#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // F9 es la carga rápida de Skyrim: abriría el menú y cargaría la partida a la vez.
    constexpr std::uint32_t TECLA_POR_DEFECTO = 0x57;  // F11

    std::uint32_t tecla = TECLA_POR_DEFECTO;

    // ---- Texto ----------------------------------------------------------------------------

    bool EsUtf8(std::string_view s)
    {
        for (std::size_t i = 0; i < s.size();) {
            const auto c = static_cast<unsigned char>(s[i]);
            const int  largo = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
            if (largo == 0 || i + largo > s.size()) return false;
            for (int k = 1; k < largo; ++k) {
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
            }
            i += largo;
        }
        return true;
    }

    // Los nombres del juego pueden venir en UTF-8 o en Windows-1252; ImGui necesita UTF-8.
    std::string AUtf8(std::string_view s)
    {
        if (EsUtf8(s)) return std::string{ s };
        std::string salida;
        for (const auto ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            if (c < 0x80) {
                salida += static_cast<char>(c);
            } else {
                salida += static_cast<char>(0xC0 | (c >> 6));
                salida += static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        return salida;
    }

    // Minúsculas y sin acentos, para que "lobo" encuentre "Lobo" y "nino" encuentre "Niño".
    std::string Plegar(std::string_view utf8)
    {
        static constexpr std::string_view latin1 = "aaaaaaaceeeeiiiidnooooo ouuuuy saaaaaaaceeeeiiiidnooooo ouuuuy y";
        std::string salida;
        for (std::size_t i = 0; i < utf8.size();) {
            const auto c = static_cast<unsigned char>(utf8[i]);
            if (c < 0x80) {
                salida += static_cast<char>(std::tolower(c));
                ++i;
            } else if ((c >> 5) == 0x6 && i + 1 < utf8.size()) {
                const unsigned cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3F);
                if (cp >= 0xC0 && cp <= 0xFF) salida += latin1[cp - 0xC0];
                i += 2;
            } else {
                ++i;
            }
        }
        return salida;
    }

    // ---- Lista de personajes --------------------------------------------------------------

    // Humanos: razas jugables o con las animaciones de una persona (vampiros, ancianos...).
    // Ocultos: los repetidos, que se transforman igual que uno de las otras dos pestañas.
    // Acciones: las animaciones de la criatura en la que se está transformado (no para personas).
    enum class Pestana { kHumanos, kCriaturas, kOcultos, kAcciones };

    struct Entrada
    {
        std::string   etiqueta;
        std::string   busqueda;
        RE::FormID    raza{ 0 };
        RE::FormID    piel{ 0 };  // la del NPC; 0: la de la raza
        RE::FormID    npc{ 0 };   // de quién se copia la cara (personas)
        std::uint32_t copias{ 1 };
        Pestana       pestana{ Pestana::kCriaturas };
    };

    std::vector<Entrada> entradas;
    std::array<std::size_t, 3> totales{};  // entradas de cada pestaña
    std::filesystem::path rutaIni;

    std::string Minusculas(std::string s)
    {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // El .hkx de animaciones de la raza. BGSBehaviorGraphModel hereda en privado de TESModel, que
    // es su única base.
    const char* GrafoDeAnimacion(const RE::TESRace* raza)
    {
        return reinterpret_cast<const RE::TESModel*>(&raza->behaviorGraphs[RE::SEXES::kMale])->GetModel();
    }

    // Avisos para las razas con las que el jugador no se podrá mover: lo dicen sus marcas, o no
    // tienen animaciones. No son todas las que fallan; las demás se ocultan desde el .ini.
    std::string Avisos(const RE::TESRace* raza)
    {
        using Marca = RE::RACE_DATA::Flag;
        std::string avisos;
        if (raza->data.flags.all(Marca::kImmobile)) {
            avisos += "  [inmóvil]";
        } else if (!raza->data.flags.any(Marca::kWalks)) {
            avisos += raza->data.flags.any(Marca::kFlies) ? "  [solo vuela]" : "  [no camina]";
        }
        const char* grafo = GrafoDeAnimacion(raza);
        if (!grafo || !*grafo) avisos += "  [sin animaciones]";
        return avisos;
    }

    // [Ocultar] Razas=ChickenRace, HareRace: EditorID separados por comas, en minúsculas.
    std::unordered_set<std::string> RazasOcultas()
    {
        std::unordered_set<std::string> ocultas;
        wchar_t texto[4096]{};
        GetPrivateProfileStringW(L"Ocultar", L"Razas", L"", texto, 4096, rutaIni.c_str());
        std::string actual;
        for (const wchar_t* c = texto;; ++c) {
            if (*c == L',' || *c == 0) {
                const auto inicio = actual.find_first_not_of(' ');
                if (inicio != std::string::npos) ocultas.insert(Minusculas(actual.substr(inicio, actual.find_last_not_of(' ') - inicio + 1)));
                actual.clear();
                if (*c == 0) break;
            } else if (*c < 0x80) {
                actual += static_cast<char>(*c);
            }
        }
        return ocultas;
    }

    // La piel del NPC, o la de su plantilla si hereda los rasgos de otro NPC.
    RE::TESObjectARMO* PielDe(RE::TESNPC* npc)
    {
        for (int nivel = 0; npc && nivel < 8; ++nivel) {
            if (npc->skin) return npc->skin;
            if (!npc->UsesTemplate() || !npc->actorData.templateUseFlags.any(RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kTraits)) break;
            npc = npc->baseTemplateForm ? npc->baseTemplateForm->As<RE::TESNPC>() : nullptr;
        }
        return nullptr;
    }

    bool EsHumana(const RE::TESRace* raza)
    {
        if (raza->GetPlayable()) return true;
        const char* grafo = GrafoDeAnimacion(raza);
        return grafo && Minusculas(grafo) == R"(actors\character\defaultmale.hkx)";
    }

    // El NPC del que salen los rasgos: si hereda los rasgos de otro NPC (plantilla), ese otro.
    RE::TESNPC* FuenteDeRasgos(RE::TESNPC* npc)
    {
        for (int nivel = 0; npc && nivel < 8; ++nivel) {
            if (!npc->UsesTemplate() || !npc->actorData.templateUseFlags.any(RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kTraits)) break;
            auto* plantilla = npc->baseTemplateForm ? npc->baseTemplateForm->As<RE::TESNPC>() : nullptr;
            if (!plantilla) break;
            npc = plantilla;
        }
        return npc;
    }

    std::uint32_t ColorEntero(RE::Color c)
    {
        return (static_cast<std::uint32_t>(c.red) << 24) | (static_cast<std::uint32_t>(c.green) << 16) | (static_cast<std::uint32_t>(c.blue) << 8) | c.alpha;
    }

    // Todo lo que se copia de una persona, en texto: dos NPC con la misma huella se ven igual.
    std::string HuellaDeCara(RE::TESNPC* original, RE::TESRace* raza, RE::FormID piel)
    {
        auto*       npc = FuenteDeRasgos(original);
        std::string h = std::format("{:08X}|{}|{:08X}|{:.1f}|{:.2f}|{:08X}", raza->GetFormID(), npc->IsFemale(), piel, npc->weight, npc->height,
                                    ColorEntero(npc->bodyTintColor));
        if (npc->headRelatedData) {
            const auto* hr = npc->headRelatedData;
            h += std::format("|{:08X}|{:08X}", hr->hairColor ? hr->hairColor->GetFormID() : 0, hr->faceDetails ? hr->faceDetails->GetFormID() : 0);
        }
        for (std::int8_t i = 0; npc->headParts && i < npc->numHeadParts; ++i) {
            h += std::format("|p{:08X}", npc->headParts[i] ? npc->headParts[i]->GetFormID() : 0);
        }
        if (npc->faceData) {
            for (const float m : npc->faceData->morphs) h += std::format("|{:.2f}", m);
            for (const auto pr : npc->faceData->parts) h += std::format("|{}", pr);
        }
        if (npc->tintLayers) {
            for (const auto* capa : *npc->tintLayers) {
                if (capa) h += std::format("|t{}:{:08X}:{}", capa->tintIndex, ColorEntero(capa->tintColor), capa->interpolationValue);
            }
        }
        return h;
    }

    // Criaturas: se agrupan por raza y piel, que es lo que cambia al transformarse. Así los lobos
    // de nieve y los draugr de cada tipo salen aparte, y dos criaturas que comparten raza no se
    // juntan si tienen pieles distintas. Personas: se copia toda su cara, así que solo se juntan
    // las que se ven exactamente igual. De cada grupo queda el nombre que más NPC usan; los
    // demás van a Ocultos.
    void CargarLista()
    {
        auto* datos = RE::TESDataHandler::GetSingleton();
        if (!datos) return;
        const auto  ocultas = RazasOcultas();
        std::size_t ocultadosIni = 0;

        struct Variante
        {
            std::string   nombre;
            std::string   archivo;
            RE::TESRace*  raza{ nullptr };
            RE::FormID    piel{ 0 };
            RE::FormID    npc{ 0 };
            bool          mujer{ false };
            std::uint32_t copias{ 0 };
        };
        std::vector<Variante>                                     variantes;
        std::unordered_map<std::string, std::size_t>              porNombre;  // nombre|grupo
        std::unordered_map<std::string, std::vector<std::size_t>> grupos;     // criaturas: raza y piel; personas: su cara

        for (auto* npc : datos->GetFormArray<RE::TESNPC>()) {
            if (!npc) continue;
            const char* nombre = npc->GetName();
            auto*       raza = npc->GetRace();
            if (!nombre || !*nombre || !raza) continue;
            const std::string idRaza = raza->GetFormEditorID();
            if (idRaza.empty()) continue;
            if (ocultas.contains(Minusculas(idRaza))) {
                ++ocultadosIni;
                continue;
            }
            auto*            piel = PielDe(npc);
            const RE::FormID efectiva = piel ? piel->GetFormID() : (raza->skin ? raza->skin->GetFormID() : 0);
            const auto       nombreUtf8 = AUtf8(nombre);
            const auto       grupo = EsHumana(raza) ? HuellaDeCara(npc, raza, efectiva) : std::format("{:08X}|{:08X}", raza->GetFormID(), efectiva);
            auto [it, nueva] = porNombre.try_emplace(nombreUtf8 + "|" + grupo, variantes.size());
            if (nueva) {
                const auto* archivo = npc->GetFile(0);
                variantes.push_back({ nombreUtf8, archivo ? std::string{ archivo->GetFilename() } : "?"s, raza, piel ? piel->GetFormID() : 0,
                                      npc->GetFormID(), FuenteDeRasgos(npc)->IsFemale(), 0 });
                grupos[grupo].push_back(it->second);
            }
            ++variantes[it->second].copias;
        }

        std::size_t ocultos = 0;
        for (auto& [clave, indices] : grupos) {
            // El nombre más usado; si empatan, el más corto, que suele ser el genérico.
            const auto principal = *std::ranges::min_element(indices, [&](std::size_t a, std::size_t b) {
                const auto& va = variantes[a];
                const auto& vb = variantes[b];
                if (va.copias != vb.copias) return va.copias > vb.copias;
                return va.nombre.size() < vb.nombre.size();
            });
            const auto& p = variantes[principal];
            for (const auto i : indices) {
                const auto&       v = variantes[i];
                const std::string idRaza = v.raza->GetFormEditorID();
                const char*       nombreRaza = v.raza->GetName();
                const std::string razaVisible = nombreRaza && *nombreRaza ? AUtf8(nombreRaza) : idRaza;

                Entrada e;
                e.raza = v.raza->GetFormID();
                e.piel = v.piel;
                e.npc = v.npc;
                e.copias = v.copias;
                if (i == principal) {
                    const auto otros = indices.size() - 1;
                    const auto iguales = otros ? std::format("   +{} iguales", otros) : "";
                    if (EsHumana(v.raza)) {
                        e.pestana = Pestana::kHumanos;
                        e.etiqueta = std::format("{}   -   {}, {}{}   ({}){}", v.nombre, razaVisible, v.mujer ? "mujer" : "hombre", Avisos(v.raza),
                                                 v.archivo, iguales);
                    } else {
                        e.pestana = Pestana::kCriaturas;
                        e.etiqueta = std::format("{}   -   {}{}   ({}){}", v.nombre, razaVisible, Avisos(v.raza), v.archivo, iguales);
                    }
                } else {
                    e.pestana = Pestana::kOcultos;
                    e.etiqueta = std::format("{}   -   {}   ({})   igual que: {}", v.nombre, razaVisible, v.archivo, p.nombre);
                    ++ocultos;
                }
                e.busqueda = Plegar(v.nombre + " " + razaVisible + " " + idRaza);
                entradas.push_back(std::move(e));
            }
        }
        std::ranges::sort(entradas, {}, &Entrada::busqueda);
        for (const auto& e : entradas) ++totales[static_cast<int>(e.pestana)];
        spdlog::info("{} personajes: {} únicos y {} repetidos ({} ocultos por el .ini)", entradas.size(), entradas.size() - ocultos,
                     ocultos, ocultadosIni);
    }

    // ---- Cambio de raza -------------------------------------------------------------------

    std::atomic<bool> transformado{ false };  // hay un aspecto original al que volver
    std::atomic<bool> abierto{ false };       // el menú
    std::string             estado;
    std::mutex              mutexEstado;

    void PonerEstado(std::string texto)
    {
        std::scoped_lock bloqueo{ mutexEstado };
        estado = std::move(texto);
    }

    // Actor.SetRace de Papyrus, a través de TransformacionesAPI.pex. No se usa la consola porque
    // el cliente de SkyMP la intercepta, y con ella el juego se cerró al transformarse.
    bool LlamarApi(const char* funcion, RE::BSScript::IFunctionArguments* argumentos)
    {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            spdlog::error("No hay máquina virtual de Papyrus");
            delete argumentos;
            return false;
        }
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> respuesta;
        const bool lanzada = vm->DispatchStaticCall("TransformacionesAPI", funcion, argumentos, respuesta);
        delete argumentos;
        return lanzada;
    }

    // Si el actor ya es de esa raza, TransformacionesAPI solo recarga su modelo (para la piel).
    bool PonerRaza(RE::Actor* actor, RE::TESRace* raza)
    {
        return LlamarApi("Transformar", RE::MakeFunctionArguments(std::move(actor), std::move(raza)));
    }

    // ---- Controles ------------------------------------------------------------------------
    //
    // Desde la 1.6.1130 el juego tiene un contexto de entrada más y ControlMap creció 8 bytes, pero
    // CommonLibSSE-NG 3.6 sigue con la estructura antigua: su enabledControls (0x118) cae en el
    // tamaño de la pila de contextos de entrada. Al quitar los controles con el menú se vaciaba esa
    // pila y el juego dejaba de hacer caso al jugador, aunque volviera a su raza; al ponerlos todos
    // (FFF), el juego leía contextos que no existen y se cerraba. Aquí se usa la estructura de la
    // versión que se está ejecutando.

    constexpr std::uint32_t TODOS_LOS_CONTROLES = 0xFFF;  // los 12 de UserEvents::USER_EVENT_FLAG
    constexpr std::uint32_t CONTROL_INVALIDO = 1u << 31;  // UserEvents::USER_EVENT_FLAG::kInvalid

    struct CamposDeControles
    {
        std::uint32_t activos;  // enabledControls
        std::uint32_t copia;    // unk11C
    };

    CamposDeControles* Controles()
    {
        auto* mapa = RE::ControlMap::GetSingleton();
        if (!mapa) return nullptr;
        const bool nueva = REL::Module::get().version() >= REL::Version(1, 6, 1130, 0);
        auto*      base = reinterpret_cast<std::byte*>(mapa);
        // Justo antes va el tamaño de la pila de contextos, que nunca pasa de unos pocos. Si no
        // cuadra, es otra estructura: mejor no tocar nada.
        const auto tamanoPila = *reinterpret_cast<const std::uint32_t*>(base + (nueva ? 0x118 : 0x110));
        if (tamanoPila > 32) {
            static bool avisado = false;
            if (!avisado) spdlog::error("ControlMap no tiene la forma esperada (pila de {}); no se tocan los controles", tamanoPila);
            avisado = true;
            return nullptr;
        }
        return reinterpret_cast<CamposDeControles*>(base + (nueva ? 0x120 : 0x118));
    }

    bool ControlActivo(std::uint32_t control)
    {
        const auto* c = Controles();
        return c && (c->activos & control) == control;
    }

    // Igual que ControlMap::ToggleControls, con los campos en su sitio. En el hilo principal.
    void PonerControl(std::uint32_t control, bool activar)
    {
        auto* mapa = RE::ControlMap::GetSingleton();
        auto* c = Controles();
        if (!mapa || !c) return;
        const auto antes = c->activos;
        if (activar) {
            c->activos |= control;
            if (c->copia != CONTROL_INVALIDO) c->copia |= control;
        } else {
            c->activos &= ~control;
            if (c->copia != CONTROL_INVALIDO) c->copia &= ~control;
        }
        RE::UserEventEnabled evento;
        evento.newUserEventFlag = static_cast<RE::UserEvents::USER_EVENT_FLAG>(c->activos);
        evento.oldUserEventFlag = static_cast<RE::UserEvents::USER_EVENT_FLAG>(antes);
        mapa->SendEvent(std::addressof(evento));
    }

    // ---- Desatascar -----------------------------------------------------------------------
    //
    // Si el jugador se queda sin control, lo que puede dejarlo quieto se anota en el registro
    // antes y después de desatascarlo, para saber qué fue si vuelve a pasar.

    std::string MarcasDeRaza(const RE::TESRace* raza)
    {
        using Marca = RE::RACE_DATA::Flag;
        const auto& f = raza->data.flags;
        const char* grafo = GrafoDeAnimacion(raza);
        return std::format("camina {}, nada {}, vuela {}, inmóvil {}, animaciones \"{}\"", f.any(Marca::kWalks), f.any(Marca::kSwims),
                           f.any(Marca::kFlies), f.any(Marca::kImmobile), grafo ? grafo : "");
    }

    void RegistrarEstado(const char* momento)
    {
        auto* jugador = RE::PlayerCharacter::GetSingleton();
        if (!jugador) return;
        auto*       raza = jugador->GetRace();
        const auto* campos = Controles();
        auto*       controles = RE::PlayerControls::GetSingleton();
        const auto* actor = jugador->AsActorState();
        const auto& datos = jugador->GetActorRuntimeData();
        bool        porAnimacion = false;
        jugador->GetGraphVariableBool("bAnimationDriven", porAnimacion);
        const auto manejador = [](RE::PlayerInputHandler* h) { return h ? (h->inputEventHandlingEnabled ? "sí" : "NO") : "?"; };

        spdlog::info("Estado ({}): raza {} ({})", momento, raza ? raza->GetFormEditorID() : "?", raza ? MarcasDeRaza(raza) : "");
        spdlog::info("  controles {:03X} de {:03X}, mover {}, mirar {}, IA {}, movimiento bloqueado {}, paralizado {}",
                     campos ? campos->activos & TODOS_LOS_CONTROLES : 0, TODOS_LOS_CONTROLES,
                     manejador(controles ? controles->movementHandler : nullptr), manejador(controles ? controles->lookHandler : nullptr),
                     jugador->IsAIEnabled(), datos.boolFlags.all(RE::Actor::BOOL_FLAGS::kMovementBlocked),
                     datos.boolBits.all(RE::Actor::BOOL_BITS::kParalyzed));
        spdlog::info("  sentado {}, derribado {}, vida {}, ejecución {}, bAnimationDriven {}", static_cast<int>(actor->GetSitSleepState()),
                     static_cast<int>(actor->GetKnockState()), static_cast<int>(actor->GetLifeState()), jugador->IsInKillMove(), porAnimacion);
    }

    void RegistrarEstadoMasTarde(const char* momento)
    {
        std::thread([momento]() {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            SKSE::GetTaskInterface()->AddTask([momento]() { RegistrarEstado(momento); });
        }).detach();
    }

    // En el hilo principal. Con completo también se quitan el control por IA y la parálisis, que solo
    // tocan al pulsar el botón: el juego los usa en escenas y efectos.
    void Desatascar(bool completo)
    {
        auto* jugador = RE::PlayerCharacter::GetSingleton();
        if (!jugador) return;
        RegistrarEstado(completo ? "antes de desatascar" : "al volver, antes de desatascar");
        if (!abierto) {
            for (std::uint32_t bit = 0; bit < 12; ++bit) PonerControl(1u << bit, true);
        }
        auto& datos = jugador->GetActorRuntimeData();
        datos.boolFlags.reset(RE::Actor::BOOL_FLAGS::kMovementBlocked);
        if (completo) datos.boolBits.reset(RE::Actor::BOOL_BITS::kParalyzed);
        jugador->SetGraphVariableBool("bAnimationDriven", false);
        // Game.EnablePlayerControls no puede ir con el menú abierto: movería al personaje al escribir.
        RE::Actor* actor = jugador;
        if (!abierto && !LlamarApi("Desatascar", RE::MakeFunctionArguments(std::move(actor), std::move(completo)))) {
            spdlog::error("  no se pudo llamar a TransformacionesAPI.Desatascar");
        }
        RegistrarEstadoMasTarde("después de desatascar");
    }

    std::atomic<bool> cambioPropio{ false };     // el próximo fin de cambio de raza es nuestro
    std::atomic<bool> desatascarAlVolver{ false };

    // ---- Apariencia -----------------------------------------------------------------------
    //
    // Con una criatura basta la raza y la piel. Con una persona, lo que la hace reconocible es su
    // cara: se copian a la ficha del jugador los mismos datos del menú de creación de personaje
    // (y de la apariencia que sincroniza SkyMP): sexo, raza, piel, peso, altura, color de piel y
    // de pelo, textura de la cara, partes de la cabeza, rasgos, preajustes y pinturas. La ropa no.
    //
    // Antes de la primera transformación se guarda el aspecto del jugador, en memoria y en un
    // archivo por personaje (SkyMP no usa las partidas guardadas), y "Volver" lo restaura tal cual.

    // La TintMask del juego, que CommonLib solo declara (la misma que usan SKSE y SkyMP).
    struct MascaraDeTinte
    {
        RE::TESTexture* texture;
        RE::Color       color;
        float           alpha;
        std::uint32_t   tintType;
    };
    static_assert(sizeof(MascaraDeTinte) == 0x18);

    constexpr std::uint32_t TINTE_TONO_DE_PIEL = 6;  // TintLayer::SkinTone::kSkinTone

    struct Tinte
    {
        std::string   ruta;
        std::uint32_t tipo{ 0 };
        RE::Color     color{};
        float         alfa{ 0 };
    };

    struct Apariencia
    {
        bool                        conCara{ true };  // false: solo raza y piel (criaturas)
        RE::TESRace*                raza{ nullptr };
        RE::TESObjectARMO*          piel{ nullptr };
        bool                        mujer{ false };
        float                       peso{ 50.0f };
        float                       altura{ 1.0f };
        RE::Color                   colorPiel{};
        RE::BGSColorForm*           colorPelo{ nullptr };
        std::optional<RE::Color>    valorColorPelo;  // por si el color de pelo era una forma creada en partida
        RE::BGSTextureSet*          texturaCara{ nullptr };
        std::vector<RE::BGSHeadPart*> partes;
        std::array<float, RE::TESNPC::FaceData::Morphs::kTotal>      rasgos{};
        std::array<std::int32_t, RE::TESNPC::FaceData::Parts::kTotal> preajustes{};
        std::vector<Tinte>          tintes;
    };

    std::optional<Apariencia> original;   // el aspecto del jugador antes de transformarse
    std::optional<Apariencia> caraPendiente;  // la cara que se pone al terminar el cambio de raza

    auto* PinturasDeRaza(RE::TESRace* raza, bool mujer)
    {
        auto* cara = raza ? raza->faceRelatedData[mujer ? RE::SEXES::kFemale : RE::SEXES::kMale] : nullptr;
        return cara ? cara->tintMasks : nullptr;
    }

    // Una textura propia para una pintura que no está entre las de la raza (por ejemplo, una que
    // venía de SkyMP). Se le copia la tabla virtual de una de verdad, para que sea un objeto válido.
    RE::TESTexture* TexturaPropia(const std::string& ruta)
    {
        static void* tabla = nullptr;
        if (!tabla) {
            if (auto* datos = RE::TESDataHandler::GetSingleton()) {
                for (auto* raza : datos->GetFormArray<RE::TESRace>()) {
                    auto* pinturas = PinturasDeRaza(raza, false);
                    if (pinturas && !pinturas->empty() && (*pinturas)[0]) {
                        tabla = *reinterpret_cast<void**>(&(*pinturas)[0]->texture.file);
                        break;
                    }
                }
            }
        }
        if (!tabla) return nullptr;
        auto* textura = RE::calloc<RE::TESTexture>(1);
        *reinterpret_cast<void**>(textura) = tabla;
        new (&textura->textureName) RE::BSFixedString(ruta.c_str());
        return textura;
    }

    // La textura de la pintura en la raza (es la que usa el juego); si no está, una propia.
    RE::TESTexture* TexturaDeTinte(RE::TESRace* raza, bool mujer, const std::string& ruta)
    {
        if (auto* pinturas = PinturasDeRaza(raza, mujer)) {
            const auto buscada = Minusculas(ruta);
            for (auto* activo : *pinturas) {
                if (activo && Minusculas(activo->texture.file.textureName.c_str()) == buscada) return &activo->texture.file;
            }
        }
        return ruta.empty() ? nullptr : TexturaPropia(ruta);
    }

    Apariencia CapturarJugador(RE::PlayerCharacter* jugador)
    {
        Apariencia a;
        auto*      base = jugador->GetActorBase();
        a.raza = jugador->GetRace();
        a.piel = base->skin;
        a.mujer = base->IsFemale();
        a.peso = base->weight;
        a.altura = base->height;
        a.colorPiel = base->bodyTintColor;
        if (const auto* hr = base->headRelatedData) {
            a.colorPelo = hr->hairColor;
            a.texturaCara = hr->faceDetails;
        }
        if (a.colorPelo) a.valorColorPelo = a.colorPelo->color;
        for (std::int8_t i = 0; base->headParts && i < base->numHeadParts; ++i) {
            if (base->headParts[i]) a.partes.push_back(base->headParts[i]);
        }
        if (base->faceData) {
            std::ranges::copy(base->faceData->morphs, a.rasgos.begin());
            std::ranges::copy(base->faceData->parts, a.preajustes.begin());
        }
        if (auto* lista = jugador->GetTintList()) {
            for (auto* m : *lista) {
                const auto* t = reinterpret_cast<const MascaraDeTinte*>(m);
                if (!t) continue;
                a.tintes.push_back({ t->texture ? t->texture->textureName.c_str() : "", t->tintType, t->color, t->alpha });
            }
        }
        return a;
    }

    // El aspecto de un NPC. Las pinturas de un NPC son "índice de pintura de su raza + color +
    // intensidad"; el jugador tiene una lista de texturas, que se sacan de las pinturas de la raza.
    Apariencia DesdeNpc(RE::TESNPC* elegido)
    {
        auto*      npc = FuenteDeRasgos(elegido);
        Apariencia a;
        a.raza = elegido->GetRace();
        a.piel = PielDe(elegido);
        a.mujer = npc->IsFemale();
        a.peso = npc->weight;
        a.altura = npc->height;
        a.colorPiel = npc->bodyTintColor;
        if (const auto* hr = npc->headRelatedData) {
            a.colorPelo = hr->hairColor;
            a.texturaCara = hr->faceDetails;
        }
        for (std::int8_t i = 0; npc->headParts && i < npc->numHeadParts; ++i) {
            if (npc->headParts[i]) a.partes.push_back(npc->headParts[i]);
        }
        if (npc->faceData) {
            std::ranges::copy(npc->faceData->morphs, a.rasgos.begin());
            std::ranges::copy(npc->faceData->parts, a.preajustes.begin());
        }
        auto* pinturas = PinturasDeRaza(a.raza, a.mujer);
        bool  tonoDePiel = false;
        if (npc->tintLayers && pinturas) {
            for (const auto* capa : *npc->tintLayers) {
                if (!capa) continue;
                for (auto* activo : *pinturas) {
                    if (!activo || activo->texture.index != capa->tintIndex) continue;
                    Tinte t;
                    t.ruta = activo->texture.file.textureName.c_str();
                    t.tipo = activo->texture.skinTone.underlying();
                    t.alfa = std::clamp(capa->interpolationValue / 100.0f, 0.0f, 1.0f);
                    t.color = capa->tintColor;
                    t.color.alpha = static_cast<std::uint8_t>(t.alfa * 255.0f);
                    tonoDePiel = tonoDePiel || t.tipo == TINTE_TONO_DE_PIEL;
                    a.tintes.push_back(std::move(t));
                    break;
                }
            }
        }
        // Sin pintura de tono de piel, la cara quedaría de otro color que el cuerpo.
        if (!tonoDePiel && pinturas) {
            for (auto* activo : *pinturas) {
                if (activo && activo->texture.skinTone.underlying() == TINTE_TONO_DE_PIEL) {
                    Tinte t{ activo->texture.file.textureName.c_str(), TINTE_TONO_DE_PIEL, a.colorPiel, 1.0f };
                    t.color.alpha = 255;
                    a.tintes.push_back(std::move(t));
                    break;
                }
            }
        }
        return a;
    }

    RE::BGSColorForm* CrearColor(RE::Color color)
    {
        auto* fabrica = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSColorForm>();
        auto* forma = fabrica ? static_cast<RE::BGSColorForm*>(fabrica->Create()) : nullptr;
        if (forma) forma->color = color;
        return forma;
    }

    // Como en SkyMP: fuera las pinturas invisibles, y nunca dos pinturas de guerra visibles a la
    // vez (una de la raza y otra general), porque el juego se cierra.
    std::vector<const Tinte*> PinturasVisibles(const std::vector<Tinte>& tintes)
    {
        std::vector<const Tinte*> visibles;
        bool                      guerra = false;
        for (const auto& t : tintes) {
            if (t.color.alpha == 0 && t.alfa <= 0.0f) continue;
            if (Minusculas(t.ruta).find("warpaint") != std::string::npos) {
                if (guerra) continue;
                guerra = true;
            }
            visibles.push_back(&t);
        }
        return visibles;
    }

    // En el hilo principal. Pone la cara en la ficha del jugador; el modelo se recarga aparte.
    // La raza solo con ponerRaza, una vez hecho el SetRace: si la ficha ya tiene la raza nueva,
    // SetRace cree que no hay nada que cambiar y no cambia el esqueleto ni avisa al terminar.
    void AplicarCara(RE::PlayerCharacter* jugador, const Apariencia& a, bool ponerRaza)
    {
        auto* base = jugador->GetActorBase();
        if (!base) return;
        if (a.mujer) {
            base->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kFemale);
        } else {
            base->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kFemale);
        }
        if (ponerRaza && a.raza) base->race = a.raza;
        base->skin = a.piel;
        base->weight = a.peso;
        base->height = a.altura;
        base->bodyTintColor = a.colorPiel;
        auto* pelo = a.colorPelo;
        if (!pelo && a.valorColorPelo) pelo = CrearColor(*a.valorColorPelo);
        base->SetHairColor(pelo);
        base->SetFaceTexture(a.texturaCara);

        // Una lista nueva: la vieja puede ser la de otro NPC o la del original, que se conserva.
        RE::BGSHeadPart** partes = nullptr;
        if (!a.partes.empty()) {
            partes = RE::calloc<RE::BGSHeadPart*>(a.partes.size());
            std::ranges::copy(a.partes, partes);
        }
        base->headParts = partes;
        base->numHeadParts = static_cast<std::int8_t>(a.partes.size());

        if (!base->faceData) base->faceData = RE::calloc<RE::TESNPC::FaceData>(1);
        if (base->faceData) {
            std::ranges::copy(a.rasgos, base->faceData->morphs);
            std::ranges::copy(a.preajustes, base->faceData->parts);
        }

        if (auto* lista = jugador->GetTintList()) {
            lista->clear();
            for (const auto* t : PinturasVisibles(a.tintes)) {
                auto* textura = TexturaDeTinte(a.raza, a.mujer, t->ruta);
                if (!textura) continue;
                auto* m = RE::calloc<MascaraDeTinte>(1);
                m->texture = textura;
                m->color = t->color;
                m->alpha = t->alfa;
                m->tintType = t->tipo;
                lista->push_back(reinterpret_cast<RE::TintMask*>(m));
            }
        }
        spdlog::info("  cara: {}, {} partes de la cabeza, {} pinturas, peso {:.0f}, altura {:.2f}", a.mujer ? "mujer" : "hombre", a.partes.size(),
                     a.tintes.size(), a.peso, a.altura);
    }

    // ---- Archivo con el aspecto original ----
    //
    // Data\SKSE\Plugins\Transformaciones\<personaje>.txt. Las formas se guardan como
    // "plugin|id local", que no depende del orden de carga.

    std::filesystem::path RutaOriginal()
    {
        auto*       jugador = RE::PlayerCharacter::GetSingleton();
        auto*       base = jugador ? jugador->GetActorBase() : nullptr;
        std::string nombre = base && base->GetName() ? base->GetName() : "";
        for (auto& c : nombre) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
        }
        if (nombre.empty()) nombre = "jugador";
        return rutaIni.parent_path() / "Transformaciones" / (nombre + ".txt");
    }

    std::string Referencia(const RE::TESForm* forma)
    {
        const auto* archivo = forma ? forma->GetFile(0) : nullptr;
        return archivo ? std::format("{}|{:X}", archivo->GetFilename(), forma->GetLocalFormID()) : "";
    }

    template <class T>
    T* Resolver(const std::string& texto)
    {
        const auto barra = texto.rfind('|');
        auto*      datos = RE::TESDataHandler::GetSingleton();
        if (barra == std::string::npos || !datos) return nullptr;
        const auto id = static_cast<RE::FormID>(std::strtoul(texto.c_str() + barra + 1, nullptr, 16));
        return datos->LookupForm<T>(id, texto.substr(0, barra));
    }

    std::string Color(RE::Color c) { return std::format("{} {} {} {}", c.red, c.green, c.blue, c.alpha); }

    RE::Color LeerColor(const std::string& texto)
    {
        unsigned r = 0, g = 0, b = 0, a = 0;
        sscanf_s(texto.c_str(), "%u %u %u %u", &r, &g, &b, &a);
        return RE::Color(static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a));
    }

    void GuardarOriginal(const Apariencia& a)
    {
        const auto ruta = RutaOriginal();
        std::error_code error;
        std::filesystem::create_directories(ruta.parent_path(), error);
        std::ofstream f(ruta, std::ios::trunc);
        f << "# Aspecto original de este personaje, para volver a él (Transformaciones). No lo borres estando transformado.\n";
        f << "raza=" << Referencia(a.raza) << "\npiel=" << Referencia(a.piel) << "\nmujer=" << a.mujer << "\npeso=" << a.peso << "\naltura=" << a.altura
          << "\ncolorPiel=" << Color(a.colorPiel) << "\ncolorPelo=" << Referencia(a.colorPelo) << "\n";
        if (a.valorColorPelo) f << "valorColorPelo=" << Color(*a.valorColorPelo) << "\n";
        f << "texturaCara=" << Referencia(a.texturaCara) << "\n";
        for (const auto* parte : a.partes) f << "parte=" << Referencia(parte) << "\n";
        f << "rasgos=";
        for (const float r : a.rasgos) f << r << " ";
        f << "\npreajustes=";
        for (const auto p : a.preajustes) f << p << " ";
        f << "\n";
        for (const auto& t : a.tintes) f << "tinte=" << t.tipo << "|" << Color(t.color) << "|" << t.alfa << "|" << t.ruta << "\n";
        spdlog::info("Aspecto original guardado en {}", ruta.string());
    }

    std::optional<Apariencia> LeerOriginal()
    {
        std::ifstream f(RutaOriginal());
        if (!f) return std::nullopt;
        Apariencia  a;
        std::string linea;
        while (std::getline(f, linea)) {
            const auto igual = linea.find('=');
            if (linea.empty() || linea[0] == '#' || igual == std::string::npos) continue;
            const auto clave = linea.substr(0, igual);
            const auto valor = linea.substr(igual + 1);
            if (clave == "raza") a.raza = Resolver<RE::TESRace>(valor);
            else if (clave == "piel") a.piel = Resolver<RE::TESObjectARMO>(valor);
            else if (clave == "mujer") a.mujer = valor == "1";
            else if (clave == "peso") a.peso = std::strtof(valor.c_str(), nullptr);
            else if (clave == "altura") a.altura = std::strtof(valor.c_str(), nullptr);
            else if (clave == "colorPiel") a.colorPiel = LeerColor(valor);
            else if (clave == "colorPelo") a.colorPelo = Resolver<RE::BGSColorForm>(valor);
            else if (clave == "valorColorPelo") a.valorColorPelo = LeerColor(valor);
            else if (clave == "texturaCara") a.texturaCara = Resolver<RE::BGSTextureSet>(valor);
            else if (clave == "parte") {
                if (auto* parte = Resolver<RE::BGSHeadPart>(valor)) a.partes.push_back(parte);
            } else if (clave == "rasgos") {
                std::istringstream e(valor);
                for (auto& r : a.rasgos) e >> r;
            } else if (clave == "preajustes") {
                std::istringstream e(valor);
                for (auto& p : a.preajustes) e >> p;
            } else if (clave == "tinte") {
                // tipo|r g b a|alfa|ruta (la ruta puede llevar cualquier cosa menos saltos de línea)
                const auto b1 = valor.find('|'), b2 = valor.find('|', b1 + 1), b3 = valor.find('|', b2 + 1);
                if (b3 == std::string::npos) continue;
                a.tintes.push_back({ valor.substr(b3 + 1), static_cast<std::uint32_t>(std::strtoul(valor.c_str(), nullptr, 10)),
                                     LeerColor(valor.substr(b1 + 1, b2 - b1 - 1)), std::strtof(valor.c_str() + b2 + 1, nullptr) });
            }
        }
        if (!a.raza) {
            spdlog::warn("Aspecto original: no encuentro la raza guardada en {}", RutaOriginal().string());
            return std::nullopt;
        }
        return a;
    }

    // En el hilo principal, al abrir el menú: si hay un aspecto original guardado de este
    // personaje (de otra sesión), "Volver" lo restaura.
    void CargarOriginal()
    {
        if (original) return;
        if (auto leido = LeerOriginal()) {
            original = std::move(leido);
            transformado = true;
            spdlog::info("Aspecto original leído de {}", RutaOriginal().string());
        }
    }

    std::atomic<int> numeroDeCambio{ 0 };  // para saber si el aviso de 3 segundos sigue siendo del último

    // En el hilo principal, al terminar un cambio de raza nuestro.
    void AplicarCaraPendiente()
    {
        auto* jugador = RE::PlayerCharacter::GetSingleton();
        if (!jugador || !caraPendiente) return;
        spdlog::info("Cambio de raza terminado: se pone la cara");
        AplicarCara(jugador, *caraPendiente, true);
        caraPendiente.reset();
        jugador->DoReset3D(true);
    }

    // Si el juego no avisa del fin del cambio de raza, la cara no se queda esperando: a los 3
    // segundos se pone igualmente. Si se quedara, la pondría encima de la siguiente transformación
    // (una cara de persona sobre el esqueleto de una criatura).
    void VigilarCambioDeRaza()
    {
        const int numero = numeroDeCambio.load();
        std::thread([numero]() {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            SKSE::GetTaskInterface()->AddTask([numero]() {
                if (numero != numeroDeCambio.load() || !caraPendiente) return;
                spdlog::warn("  el cambio de raza no avisó al terminar: se pone la cara igualmente");
                cambioPropio = false;
                AplicarCaraPendiente();
                if (desatascarAlVolver.exchange(false)) Desatascar(false);
            });
        }).detach();
    }

    // En el hilo principal.
    void Aplicar(Apariencia destino, bool volviendo)
    {
        auto* jugador = RE::PlayerCharacter::GetSingleton();
        auto* base = jugador ? jugador->GetActorBase() : nullptr;
        if (!jugador || !base || !destino.raza) {
            PonerEstado("No encuentro esa raza.");
            return;
        }
        auto* razaActual = jugador->GetRace();
        // Paso a paso, para saber dónde se queda si el juego se cierra.
        spdlog::info("{} {} ({:08X}) con piel {:08X}, desde {}", volviendo ? "Volver a" : "Transformar en", destino.raza->GetFormEditorID(),
                     destino.raza->GetFormID(), destino.piel ? destino.piel->GetFormID() : 0, razaActual ? razaActual->GetFormEditorID() : "?");
        if (!volviendo && !original && razaActual) {
            original = CapturarJugador(jugador);
            GuardarOriginal(*original);
        }
        // Las criaturas no tienen primera persona.
        if (auto* camara = RE::PlayerCamera::GetSingleton()) camara->ForceThirdPerson();

        // Lo pendiente de una transformación anterior ya no vale.
        caraPendiente.reset();
        ++numeroDeCambio;
        const bool mismaRaza = razaActual == destino.raza;
        if (destino.conCara) {
            AplicarCara(jugador, destino, mismaRaza);
        } else {
            base->skin = destino.piel;
        }
        if (mismaRaza) {
            // Lo mismo que QueueNiNodeUpdate de SKSE: vuelve a montar el modelo y la cabeza.
            spdlog::info("  misma raza: se recarga el modelo");
            jugador->DoReset3D(true);
        } else {
            if (destino.conCara) caraPendiente = destino;
            cambioPropio = true;
            desatascarAlVolver = volviendo;
            spdlog::info("  llamando a Actor.SetRace");
            if (!PonerRaza(jugador, destino.raza)) {
                cambioPropio = false;
                desatascarAlVolver = false;
                caraPendiente.reset();
                PonerEstado("No se pudo llamar a TransformacionesAPI. ¿Está instalado el .pex?");
                spdlog::error("  DispatchStaticCall falló");
                return;
            }
            if (caraPendiente) VigilarCambioDeRaza();
        }
        if (volviendo) {
            original.reset();
            transformado = false;
            std::error_code error;
            std::filesystem::remove(RutaOriginal(), error);
            // Con la misma raza no llega el fin del cambio de raza: se desatasca aquí.
            if (mismaRaza) Desatascar(false);
            PonerEstado("Has vuelto a tu aspecto.");
        } else {
            transformado = true;
            PonerEstado(std::format("Ahora eres: {}", destino.raza->GetFormEditorID()));
        }
    }

    // Personas: toda su cara. Criaturas: raza y piel.
    void Transformar(RE::FormID idNpc, RE::FormID idRaza, RE::FormID idPiel)
    {
        SKSE::GetTaskInterface()->AddTask([idNpc, idRaza, idPiel]() {
            auto* raza = RE::TESForm::LookupByID<RE::TESRace>(idRaza);
            auto* npc = idNpc ? RE::TESForm::LookupByID<RE::TESNPC>(idNpc) : nullptr;
            Apariencia destino;
            if (npc && raza && EsHumana(raza)) {
                destino = DesdeNpc(npc);
            } else {
                destino.conCara = false;
                destino.raza = raza;
                destino.piel = idPiel ? RE::TESForm::LookupByID<RE::TESObjectARMO>(idPiel) : nullptr;
            }
            Aplicar(std::move(destino), false);
        });
    }

    void Volver()
    {
        SKSE::GetTaskInterface()->AddTask([]() {
            CargarOriginal();
            if (!original) {
                PonerEstado("No hay un aspecto original guardado.");
                return;
            }
            Aplicar(*original, true);
        });
    }

    // ---- Ataques con el ratón -------------------------------------------------------------
    //
    // Los clics del juego mandan eventos de ataque genéricos (attackStart...) que la mayoría de
    // criaturas no entienden. Cada raza tiene su lista de ataques (los datos de ataque del Creation
    // Kit, la que usa la IA): siendo criatura, el clic izquierdo lanza uno normal y el derecho uno
    // potente (o normal, si no tiene), elegidos como la IA, por su probabilidad.
    //
    // El control de lucha del juego no se quita: con él quitado el juego obliga a guardar el arma
    // (como en las escenas), y la criatura salía de la guardia nada más entrar. Tampoco hace falta:
    // el ataque del juego no hace nada con estas criaturas, porque su grafo no lo entiende. La
    // tecla de sacar el arma sigue siendo la del juego.
    //
    // Las criaturas hechas para luchar con armas (draugr y esqueletos, falmer, rieklings, hombre
    // lobo, autómatas enanos) sí entienden el attackStart del juego: con ellas se deja el ataque
    // del juego, que respeta el arma que se lleve. Se sabe mirando los eventos de su grafo.

    struct Ataque
    {
        std::string evento;
        float       probabilidad{ 1.0f };
    };

    std::vector<Ataque> ataquesNormales, ataquesPotentes;
    RE::FormID          razaDeAtaques = 0;      // la raza ya mirada; 0: ninguna
    bool                ataquesDelJuego = false;  // la criatura entiende el ataque del juego
    int                 intentosGrafo = 0;
    std::atomic<bool>   esCriatura{ false };      // criatura que ataca con los clics del mod

    constexpr float ANGULO_DE_FRENTE = 30.0f;  // el mordisco a un lado del dragón va a 25°

    // Algunas razas (lobo, troll) tienen el ángulo a 0 en todos sus ataques, también en los
    // laterales: esos se quitan por el nombre, pero solo si quedan otros. Al mudcrab y a la araña
    // les quedarían solo LeftChop/RightChop, que son golpes de frente con una pinza o la otra.
    void QuitarLaterales(std::vector<Ataque>& lista)
    {
        const auto lateral = [](const Ataque& a) {
            const auto m = Minusculas(a.evento);
            return m.find("left") != std::string::npos || m.find("right") != std::string::npos || m.find("side") != std::string::npos;
        };
        if (std::ranges::all_of(lista, lateral)) return;
        std::erase_if(lista, lateral);
    }

    // En el hilo principal. Si la raza no tiene ataques propios, usa los de su raza de datos (ATKR).
    void CargarAtaques(RE::TESRace* raza)
    {
        ataquesNormales.clear();
        ataquesPotentes.clear();
        razaDeAtaques = raza ? raza->GetFormID() : 0;
        RE::BGSAttackDataMap* mapa = nullptr;
        for (auto* r = raza; r && !mapa; ) {
            auto* m = r->attackDataMap.get();
            if (m && !m->attackDataMap.empty()) {
                mapa = m;
            } else {
                auto* siguiente = m ? m->defaultDataRace : nullptr;
                r = siguiente != r ? siguiente : nullptr;
            }
        }
        if (!mapa) {
            spdlog::info("Ataques de {}: ninguno", raza ? raza->GetFormEditorID() : "?");
            return;
        }
        using Bandera = RE::AttackData::AttackFlag;
        for (const auto& [nombre, datos] : mapa->attackDataMap) {
            if (!datos) continue;
            const std::string evento = datos->event.c_str();
            const auto        minusculas = Minusculas(evento);
            const auto&       d = datos->data;
            // Las que la IA solo usa en situaciones concretas (embestidas, a la carrera, hacia
            // atrás) tienen probabilidad 0; los golpes de escudo no son ataques.
            if (evento.empty() || d.attackChance <= 0.0f || d.flags.any(Bandera::kBashAttack, Bandera::kChargeAttack)) continue;
            if (minusculas.find("lunge") != std::string::npos || minusculas.find("rush") != std::string::npos ||
                minusculas.find("backward") != std::string::npos) {
                continue;
            }
            // Solo de frente: attackAngle es hacia dónde va el golpe respecto al frente (el oso y el
            // dragón tienen sus golpes laterales a ±90°, la cola a ±135° y 180°).
            if (std::abs(d.attackAngle) > ANGULO_DE_FRENTE) continue;
            (d.flags.any(Bandera::kPowerAttack) ? ataquesPotentes : ataquesNormales).push_back({ evento, d.attackChance });
        }
        QuitarLaterales(ataquesNormales);
        QuitarLaterales(ataquesPotentes);
        const auto lista = [](const std::vector<Ataque>& v) {
            std::string texto;
            for (const auto& a : v) texto += (texto.empty() ? "" : ", ") + a.evento;
            return texto.empty() ? "-"s : texto;
        };
        spdlog::info("Ataques de {}: normales [{}], potentes [{}]", raza->GetFormEditorID(), lista(ataquesNormales), lista(ataquesPotentes));
    }

    // Por orden de preferencia: primero uno elegido por probabilidad entre los que encajan con el
    // movimiento (los "Forward" son para atacar avanzando), y luego los demás, por si la criatura
    // no acepta el primero en ese momento.
    std::vector<std::string> OrdenarAtaques(const std::vector<Ataque>& lista, bool avanzando)
    {
        static std::mt19937 azar{ std::random_device{}() };
        std::vector<const Ataque*> encajan, resto;
        for (const auto& a : lista) {
            const bool adelante = Minusculas(a.evento).find("forward") != std::string::npos;
            (adelante == avanzando ? encajan : resto).push_back(&a);
        }
        if (encajan.empty()) std::swap(encajan, resto);
        std::vector<std::string> orden;
        while (!encajan.empty()) {
            float total = 0;
            for (const auto* a : encajan) total += a->probabilidad;
            float tirada = std::uniform_real_distribution<float>(0.0f, total)(azar);
            std::size_t elegido = 0;
            for (; elegido + 1 < encajan.size(); ++elegido) {
                tirada -= encajan[elegido]->probabilidad;
                if (tirada <= 0) break;
            }
            orden.push_back(encajan[elegido]->evento);
            encajan.erase(encajan.begin() + elegido);
        }
        for (const auto* a : resto) orden.push_back(a->evento);
        return orden;
    }

    // El contexto de entrada de arriba de la pila (0: jugando). Con un menú del juego abierto
    // (inventario, mapa, consola...) es otro y los clics no deben atacar. Con la misma
    // corrección de estructura que Controles().
    int ContextoDeEntrada()
    {
        auto* mapa = RE::ControlMap::GetSingleton();
        if (!mapa) return -1;
        const bool nueva = REL::Module::get().version() >= REL::Version(1, 6, 1130, 0);
        auto*      base = reinterpret_cast<std::byte*>(mapa);
        auto*      pila = *reinterpret_cast<std::uint32_t**>(base + (nueva ? 0x108 : 0x100));
        const auto tamano = *reinterpret_cast<std::uint32_t*>(base + (nueva ? 0x118 : 0x110));
        if (!pila || tamano == 0 || tamano > 32) return -1;
        return static_cast<int>(pila[tamano - 1]);
    }

    // Con __try: si en esta versión del juego la estructura no fuera la esperada, se pierde la
    // lista en vez de cerrarse el juego. Antes se comprueba la tabla virtual del grafo de Havok.
    int LeerEventosDeGrafo(RE::BShkbAnimationGraph* grafo, const char** salida, int maximo)
    {
        __try {
            auto* comportamiento = grafo->behaviorGraph;
            if (!comportamiento) return 0;
            if (*reinterpret_cast<std::uintptr_t*>(comportamiento) != RE::VTABLE_hkbBehaviorGraph[0].address()) return -1;
            auto* datos = comportamiento->data.get();
            auto* cadenas = datos ? datos->stringData.get() : nullptr;
            if (!cadenas) return 0;
            const int total = cadenas->eventNames.size();
            if (total < 0 || total > 20000) return -1;
            const int n = total < maximo ? total : maximo;
            for (int i = 0; i < n; ++i) salida[i] = cadenas->eventNames[i].c_str();
            return n;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -2;
        }
    }

    // El nombre del proyecto del grafo ("DraugrProject"), con __try por si la estructura no
    // fuera la esperada.
    const char* ProyectoDeGrafo(RE::BShkbAnimationGraph* grafo)
    {
        __try {
            return grafo->projectName.c_str();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // 1 si el grafo de la raza (ya cargado en el jugador) entiende el evento, 0 si no, -1 si aún
    // no está cargado. Con forzar, vale cualquier grafo cargado aunque no sea el de la raza.
    int GrafoEntiende(RE::PlayerCharacter* jugador, RE::TESRace* raza, std::string_view evento, bool forzar)
    {
        const char*  ruta = GrafoDeAnimacion(raza);
        const auto   esperado = Minusculas(ruta ? std::filesystem::path(ruta).stem().string() : "");
        const auto   buscado = Minusculas(std::string{ evento });
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> gestor;
        if (!jugador->GetAnimationGraphManager(gestor) || !gestor) return -1;
        static const char* nombres[8192];
        for (auto& grafo : gestor->graphs) {
            if (!grafo) continue;
            const char* proyecto = ProyectoDeGrafo(grafo.get());
            if (!forzar && (!proyecto || Minusculas(proyecto) != esperado)) continue;
            const int n = LeerEventosDeGrafo(grafo.get(), nombres, 8192);
            if (n <= 0) continue;
            for (int i = 0; i < n; ++i) {
                if (nombres[i] && Minusculas(nombres[i]) == buscado) return 1;
            }
            return 0;
        }
        return -1;
    }

    bool PuedeAtacar(RE::PlayerCharacter* jugador)
    {
        auto* ui = RE::UI::GetSingleton();
        return jugador && esCriatura && !abierto && ui && !ui->GameIsPaused() && ContextoDeEntrada() == 0 &&
               ControlActivo(1u << 0);  // si el juego (o SkyMP) ha quitado el movimiento, tampoco
    }

    // En el hilo principal, al pulsar un botón del ratón.
    void Atacar(bool potente)
    {
        auto* jugador = RE::PlayerCharacter::GetSingleton();
        if (!PuedeAtacar(jugador)) {
            auto* ui = RE::UI::GetSingleton();
            spdlog::info("Clic sin ataque: criatura {}, menú {}, pausa {}, contexto {}, movimiento {}", esCriatura.load(), abierto.load(),
                         ui && ui->GameIsPaused(), ContextoDeEntrada(), ControlActivo(1u << 0));
            return;
        }
        // Como con un arma: si está guardada, el clic la saca (la criatura se pone en guardia).
        if (!jugador->AsActorState()->IsWeaponDrawn()) {
            jugador->DrawWeaponMagicHands(true);
            return;
        }
        const auto& lista = potente && !ataquesPotentes.empty() ? ataquesPotentes : ataquesNormales;
        if (lista.empty()) return;
        const bool avanzando = jugador->AsActorState()->actorState1.movingForward;
        for (const auto& evento : OrdenarAtaques(lista, avanzando)) {
            if (jugador->NotifyAnimationGraph(RE::BSFixedString(evento.c_str()))) {
                spdlog::info("Ataque {}: {}", potente ? "potente" : "normal", evento);
                return;
            }
        }
        spdlog::info("Ataque {}: la criatura no aceptó ninguno ahora", potente ? "potente" : "normal");
    }

    // En el hilo principal, varias veces por segundo: mantiene la lista de ataques según la raza.
    void VigilarAtaques(RE::PlayerCharacter* jugador)
    {
        auto*      raza = jugador ? jugador->GetRace() : nullptr;
        const bool esDeCriatura = raza && !EsHumana(raza);
        if (!esDeCriatura) {
            razaDeAtaques = 0;
            intentosGrafo = 0;
        } else if (raza->GetFormID() != razaDeAtaques) {
            // Justo tras el cambio de raza puede seguir cargado el grafo anterior: se espera al
            // de la raza, y si su nombre no cuadra, tras unos 3 segundos vale el que haya.
            const int entiende = GrafoEntiende(jugador, raza, "attackStart", ++intentosGrafo > 30);
            if (entiende >= 0) {
                intentosGrafo = 0;
                ataquesDelJuego = entiende == 1;
                if (ataquesDelJuego) {
                    ataquesNormales.clear();
                    ataquesPotentes.clear();
                    razaDeAtaques = raza->GetFormID();
                    spdlog::info("Ataques de {}: entiende el ataque del juego (lucha con armas), se deja el del juego",
                                 raza->GetFormEditorID());
                } else {
                    CargarAtaques(raza);
                }
            }
        }
        esCriatura = esDeCriatura && razaDeAtaques == raza->GetFormID() && !ataquesDelJuego;
    }

    // ---- Cámara ---------------------------------------------------------------------------
    //
    // La cámara en tercera persona sigue un punto del modelo (ThirdPersonState::thirdPersonCameraObj):
    // el hueso "Camera3rd [Cam3]" del esqueleto humano. Muchas criaturas no lo tienen y la cámara se
    // queda en sus pies. A esas se les añade un nodo propio a la altura elegida y la cámara lo sigue.
    // La altura y la distancia se ajustan desde el menú y se guardan por raza en el .ini.

    constexpr std::string_view HUESO_CAMARA = "Camera3rd [Cam3]";

    RE::NiPointer<RE::NiNode> anclaje;      // el nodo añadido, mientras se es una criatura
    RE::NiPointer<RE::NiNode> raizAnclaje;  // se retiene para poder soltar el nodo sin riesgo
    std::string               razaCamara;
    std::mutex                mutexCamara;
    std::atomic<float>        alturaCamara{ 0.0f };
    std::atomic<float>        distanciaCamara{ 0.0f };  // 0: la del juego
    std::atomic<bool>         camaraPropia{ false };
    std::atomic<bool>         guardarCamara{ false };
    float                     distanciaAplicada = 0.0f;
    float                     distanciaMinOriginal = -1.0f;
    float                     distanciaMaxOriginal = -1.0f;

    RE::Setting* Ajuste(const char* nombre)
    {
        if (auto* ini = RE::INISettingCollection::GetSingleton()) {
            if (auto* ajuste = ini->GetSetting(nombre)) return ajuste;
        }
        if (auto* prefs = RE::INIPrefSettingCollection::GetSingleton()) return prefs->GetSetting(nombre);
        return nullptr;
    }

    RE::ThirdPersonState* TerceraPersona()
    {
        auto* camara = RE::PlayerCamera::GetSingleton();
        return camara ? static_cast<RE::ThirdPersonState*>(camara->cameraStates[RE::CameraState::kThirdPerson].get()) : nullptr;
    }

    RE::NiAVObject* BuscarHueso(RE::NiAVObject* objeto, const RE::NiAVObject* excluir)
    {
        if (!objeto || objeto == excluir) return nullptr;
        if (const char* nombre = objeto->name.c_str(); nombre && HUESO_CAMARA == nombre) return objeto;
        if (auto* nodo = objeto->AsNode()) {
            for (auto& hijo : nodo->GetChildren()) {
                if (auto* hallado = BuscarHueso(hijo.get(), excluir)) return hallado;
            }
        }
        return nullptr;
    }

    // La distancia se cambia con las del modo libre de la cámara: la mínima es la que se elige y la
    // máxima se aleja lo necesario para que la rueda del ratón siga funcionando.
    void PonerDistancia(float distancia)
    {
        auto* minima = Ajuste("fVanityModeMinDist:Camera");
        auto* maxima = Ajuste("fVanityModeMaxDist:Camera");
        if (!minima || !maxima) {
            spdlog::warn("Cámara: no encuentro los ajustes de distancia");
            distanciaAplicada = distancia;
            return;
        }
        if (distanciaMinOriginal < 0) {
            distanciaMinOriginal = minima->data.f;
            distanciaMaxOriginal = maxima->data.f;
        }
        if (distancia > 0) {
            minima->data.f = distancia;
            maxima->data.f = std::max(distanciaMaxOriginal, distancia + 400.0f);
        } else {
            minima->data.f = distanciaMinOriginal;
            maxima->data.f = distanciaMaxOriginal;
        }
        distanciaAplicada = distancia;
    }

    void QuitarAnclaje()
    {
        if (!anclaje) return;
        // La cámara no puede quedarse apuntando a un nodo que ya no está en el modelo.
        if (auto* tercera = TerceraPersona(); tercera && tercera->thirdPersonCameraObj == anclaje.get()) {
            auto* hueso = BuscarHueso(raizAnclaje.get(), anclaje.get());
            tercera->thirdPersonCameraObj = hueso ? hueso : raizAnclaje.get();
        }
        if (raizAnclaje) raizAnclaje->DetachChild(anclaje.get());
        anclaje.reset();
        raizAnclaje.reset();
        camaraPropia = false;
        PonerDistancia(0.0f);
    }

    std::pair<float, float> LeerCamara(const std::string& raza, float alturaPorDefecto)
    {
        wchar_t texto[64]{};
        const std::wstring clave(raza.begin(), raza.end());
        GetPrivateProfileStringW(L"Camara", clave.c_str(), L"", texto, 64, rutaIni.c_str());
        float altura = 0, distancia = 0;
        if (swscanf_s(texto, L"%f %f", &altura, &distancia) == 2) return { altura, distancia };
        return { alturaPorDefecto, 0.0f };
    }

    void GuardarCamara()
    {
        std::string raza;
        {
            std::scoped_lock bloqueo{ mutexCamara };
            raza = razaCamara;
        }
        if (raza.empty()) return;
        const std::wstring clave(raza.begin(), raza.end());
        const auto valor = std::format(L"{:.0f} {:.0f}", alturaCamara.load(), distanciaCamara.load());
        WritePrivateProfileStringW(L"Camara", clave.c_str(), valor.c_str(), rutaIni.c_str());
        spdlog::info("Cámara guardada para {}: altura {:.0f}, distancia {:.0f}", raza, alturaCamara.load(), distanciaCamara.load());
    }

    // Por debajo, en criaturas pequeñas (cangrejos, ratas, gallinas) la cámara se mete en el suelo.
    constexpr float ALTURA_MINIMA = 70.0f;

    // La del hueso de la cámara si la criatura lo tiene a una altura razonable; si no, la del
    // modelo. Siempre por encima de ALTURA_MINIMA.
    float EstimarAltura(RE::NiNode* raiz, RE::Actor* jugador)
    {
        if (auto* hueso = BuscarHueso(raiz, anclaje.get())) {
            const float altura = hueso->world.translate.z - jugador->GetPositionZ();
            if (altura > 0.0f && altura < 800.0f) return std::max(altura, ALTURA_MINIMA);
        }
        const auto& limites = raiz->worldBound;
        return std::clamp(limites.center.z - jugador->GetPositionZ() + limites.radius * 0.5f, ALTURA_MINIMA, 800.0f);
    }

    // Se ejecuta en el hilo principal varias veces por segundo y al terminar un cambio de raza.
    void Vigilar()
    {
        auto* jugador = RE::PlayerCharacter::GetSingleton();
        VigilarAtaques(jugador);
        auto* raza = jugador ? jugador->GetRace() : nullptr;
        auto* modelo = jugador ? jugador->Get3D(false) : nullptr;
        auto* raiz = modelo ? modelo->AsNode() : nullptr;
        if (!raza || !raiz) return;

        // Las personas se dejan como están. Las criaturas siempre llevan el punto propio, aunque
        // tengan el hueso de la cámara: en algunas (los cangrejos del barro) está casi en el suelo y
        // la cámara lo atraviesa.
        if (EsHumana(raza)) {
            if (anclaje) {
                QuitarAnclaje();
                spdlog::info("Cámara: raza de persona, cámara del juego");
            }
            return;
        }

        const std::string idRaza = raza->GetFormEditorID();
        bool cambio = false;
        {
            std::scoped_lock bloqueo{ mutexCamara };
            cambio = idRaza != razaCamara;
        }
        if (!anclaje || raizAnclaje.get() != raiz || cambio) {
            QuitarAnclaje();
            anclaje = RE::NiPointer<RE::NiNode>(RE::NiNode::Create(0));
            anclaje->name = "Transformaciones Camara";
            raiz->AttachChild(anclaje.get());
            raizAnclaje = RE::NiPointer<RE::NiNode>(raiz);
            const auto [altura, distancia] = LeerCamara(idRaza, EstimarAltura(raiz, jugador));
            alturaCamara = altura;
            distanciaCamara = distancia;
            {
                std::scoped_lock bloqueo{ mutexCamara };
                razaCamara = idRaza;
            }
            camaraPropia = true;
            spdlog::info("Cámara: {} con punto propio a {:.0f} de alto", idRaza, altura);
        }

        // La altura es en el mundo; el nodo cuelga del modelo, que puede estar escalado.
        const float escala = raiz->world.scale > 0.01f ? raiz->world.scale : 1.0f;
        anclaje->local.translate = { 0.0f, 0.0f, alturaCamara.load() / escala };
        RE::NiUpdateData datos{};
        anclaje->Update(datos);
        if (auto* tercera = TerceraPersona(); tercera && tercera->thirdPersonCameraObj != anclaje.get()) {
            tercera->thirdPersonCameraObj = anclaje.get();
        }
        if (const float distancia = distanciaCamara.load(); distancia != distanciaAplicada) PonerDistancia(distancia);
        if (guardarCamara.exchange(false)) GuardarCamara();
    }

    class FinDeCambioDeRaza final : public RE::BSTEventSink<RE::TESSwitchRaceCompleteEvent>
    {
    public:
        static FinDeCambioDeRaza* Instancia()
        {
            static FinDeCambioDeRaza unico;
            return &unico;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESSwitchRaceCompleteEvent* evento,
                                              RE::BSTEventSource<RE::TESSwitchRaceCompleteEvent>*) override
        {
            if (evento && evento->subject.get() == RE::PlayerCharacter::GetSingleton()) {
                spdlog::info("Cambio de raza terminado");
                SKSE::GetTaskInterface()->AddTask(Vigilar);
                // Los cambios de raza del juego (hombre lobo, señor vampiro) se dejan en paz.
                if (cambioPropio.exchange(false)) {
                    // El cambio de raza puede dejar las partes de la cabeza y las pinturas de la raza
                    // nueva: la cara se vuelve a poner encima.
                    SKSE::GetTaskInterface()->AddTask(AplicarCaraPendiente);
                    if (desatascarAlVolver.exchange(false)) {
                        SKSE::GetTaskInterface()->AddTask([]() { Desatascar(false); });
                    } else {
                        SKSE::GetTaskInterface()->AddTask([]() { RegistrarEstado("recién transformado"); });
                        RegistrarEstadoMasTarde("un segundo después de transformarse");
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // ---- Acciones -------------------------------------------------------------------------
    //
    // Los eventos que entiende el grafo de comportamiento de la criatura (wolfproject.hkx,
    // draugrproject.hkx...): ataques, aullidos, gestos. Los sonidos van dentro de las animaciones,
    // así que suenan solos. Se leen del grafo cargado del jugador al abrir el menú.

    std::vector<std::string> acciones;
    std::mutex               mutexAcciones;
    std::atomic<bool>        hayAcciones{ false };
    std::atomic<int>         versionAcciones{ 0 };

    // En el hilo principal.
    void CargarAcciones()
    {
        auto* jugador = RE::PlayerCharacter::GetSingleton();
        auto* raza = jugador ? jugador->GetRace() : nullptr;
        std::vector<std::string> nuevas;
        if (raza && !EsHumana(raza)) {
            RE::BSTSmartPointer<RE::BSAnimationGraphManager> gestor;
            if (jugador->GetAnimationGraphManager(gestor) && gestor) {
                static const char* nombres[8192];
                for (auto& grafo : gestor->graphs) {
                    if (!grafo) continue;
                    const int n = LeerEventosDeGrafo(grafo.get(), nombres, 8192);
                    if (n < 0) {
                        spdlog::warn("Acciones: el grafo de {} no tiene la forma esperada ({})", raza->GetFormEditorID(), n);
                        continue;
                    }
                    for (int i = 0; i < n; ++i) {
                        if (nombres[i] && *nombres[i]) nuevas.emplace_back(nombres[i]);
                    }
                }
            }
            std::ranges::sort(nuevas, {}, [](const std::string& a) { return Minusculas(a); });
            nuevas.erase(std::unique(nuevas.begin(), nuevas.end()), nuevas.end());
        }
        spdlog::info("Acciones de {}: {}", raza ? raza->GetFormEditorID() : "?", nuevas.size());
        {
            std::scoped_lock bloqueo{ mutexAcciones };
            acciones = std::move(nuevas);
            hayAcciones = !acciones.empty();
        }
        ++versionAcciones;
    }

    // Las que pueden dejar al jugador tirado o atascado (muerte, ragdoll, ejecuciones, muebles),
    // y las internas que las animaciones lanzan solas (pasos, sonidos, golpes): no salen salvo
    // con "Mostrar todas".
    bool AccionOculta(const std::string& accion)
    {
        static constexpr std::array<std::string_view, 33> partes{
            "death", "dying", "ragdoll", "bleedout", "kill", "paired", "unconscious", "knockdown", "knockout", "mount",
            "furniture", "chair", "sleep", "decapitat", "cripple", "grab", "tackle", "recoil", "stagger", "soundplay",
            "foot", "hitframe", "weaponswing", "weaponleftswing", "animobj", "camera", "bumper", "sync", "collision",
            "enable", "disable", "graphdeleting", "getup"
        };
        const auto minusculas = Minusculas(accion);
        return std::ranges::any_of(partes, [&](std::string_view p) { return minusculas.find(p) != std::string::npos; });
    }

    void EjecutarAccion(std::string accion)
    {
        SKSE::GetTaskInterface()->AddTask([accion = std::move(accion)]() {
            auto* jugador = RE::PlayerCharacter::GetSingleton();
            if (!jugador) return;
            const bool aceptada = jugador->NotifyAnimationGraph(RE::BSFixedString(accion.c_str()));
            spdlog::info("Acción {}: {}", accion, aceptada ? "aceptada" : "no aceptada");
            PonerEstado(aceptada ? std::format("Acción: {}", accion)
                                 : std::format("La criatura no hace caso a {} ahora (puede que necesite estar en combate o quieta).", accion));
        });
    }

    // ---- Entrada --------------------------------------------------------------------------

    bool enfocarBusqueda = false;

    struct Pendiente
    {
        float                                  dx{ 0 }, dy{ 0 }, rueda{ 0 };
        std::vector<std::pair<int, bool>>      botones;
        std::vector<std::pair<ImGuiKey, bool>> teclas;
        std::vector<char>                      letras;
    };
    Pendiente  pendiente;
    std::mutex mutexPendiente;

    // Controles que estaban activos al abrir, para devolver solo esos al cerrar.
    std::vector<std::uint32_t> controlesQuitados;

    // Los controles se quitan y se devuelven en el hilo principal, en orden. Si el menú se vuelve
    // a abrir antes de que se devuelvan, la lista se conserva y se devuelven al cerrarlo de nuevo:
    // antes se vaciaba, y esos controles se quedaban quitados para siempre.
    void Abrir()
    {
        enfocarBusqueda = true;
        abierto = true;
        SKSE::GetTaskInterface()->AddTask(CargarAcciones);
        SKSE::GetTaskInterface()->AddTask(CargarOriginal);
        SKSE::GetTaskInterface()->AddTask([]() {
            if (!abierto) return;
            for (std::uint32_t bit = 0; bit < 12; ++bit) {
                const auto control = 1u << bit;
                if (ControlActivo(control)) {
                    if (std::ranges::find(controlesQuitados, control) == controlesQuitados.end()) controlesQuitados.push_back(control);
                    PonerControl(control, false);
                }
            }
        });
    }

    // Se puede llamar desde el hilo de dibujo (los botones de la ventana).
    void Cerrar()
    {
        abierto = false;
        SKSE::GetTaskInterface()->AddTask([]() {
            if (abierto) return;
            for (const auto control : controlesQuitados) PonerControl(control, true);
            controlesQuitados.clear();
        });
    }

    bool SePuedeAbrir()
    {
        auto* ui = RE::UI::GetSingleton();
        return ui && RE::PlayerCharacter::GetSingleton() && !ui->GameIsPaused() &&
               !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) && !ui->IsMenuOpen(RE::Console::MENU_NAME) &&
               !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
    }

    // Códigos DIK del teclado. 0x27 es la Ñ en un teclado español.
    char Letra(std::uint32_t codigo)
    {
        static const std::unordered_map<std::uint32_t, char> mapa{
            { 0x10, 'q' }, { 0x11, 'w' }, { 0x12, 'e' }, { 0x13, 'r' }, { 0x14, 't' }, { 0x15, 'y' }, { 0x16, 'u' },
            { 0x17, 'i' }, { 0x18, 'o' }, { 0x19, 'p' }, { 0x1E, 'a' }, { 0x1F, 's' }, { 0x20, 'd' }, { 0x21, 'f' },
            { 0x22, 'g' }, { 0x23, 'h' }, { 0x24, 'j' }, { 0x25, 'k' }, { 0x26, 'l' }, { 0x27, 'n' }, { 0x2C, 'z' },
            { 0x2D, 'x' }, { 0x2E, 'c' }, { 0x2F, 'v' }, { 0x30, 'b' }, { 0x31, 'n' }, { 0x32, 'm' }, { 0x39, ' ' },
            { 0x02, '1' }, { 0x03, '2' }, { 0x04, '3' }, { 0x05, '4' }, { 0x06, '5' }, { 0x07, '6' }, { 0x08, '7' },
            { 0x09, '8' }, { 0x0A, '9' }, { 0x0B, '0' },
        };
        const auto it = mapa.find(codigo);
        return it == mapa.end() ? 0 : it->second;
    }

    ImGuiKey Tecla(std::uint32_t codigo)
    {
        switch (codigo) {
        case 0x0E: return ImGuiKey_Backspace;
        case 0x1C:
        case 0x9C: return ImGuiKey_Enter;
        case 0xC8: return ImGuiKey_UpArrow;
        case 0xD0: return ImGuiKey_DownArrow;
        case 0xCB: return ImGuiKey_LeftArrow;
        case 0xCD: return ImGuiKey_RightArrow;
        case 0xD3: return ImGuiKey_Delete;
        case 0xC7: return ImGuiKey_Home;
        case 0xCF: return ImGuiKey_End;
        default: return ImGuiKey_None;
        }
    }

    class Teclado final : public RE::BSTEventSink<RE::InputEvent*>
    {
    public:
        static Teclado* Instancia()
        {
            static Teclado unico;
            return &unico;
        }

        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* evento, RE::BSTEventSource<RE::InputEvent*>*) override
        {
            if (!evento) return RE::BSEventNotifyControl::kContinue;
            for (auto* e = *evento; e; e = e->next) {
                if (e->GetEventType() == RE::INPUT_EVENT_TYPE::kMouseMove) {
                    if (!abierto) continue;
                    const auto* m = static_cast<RE::MouseMoveEvent*>(e);
                    std::scoped_lock bloqueo{ mutexPendiente };
                    pendiente.dx += static_cast<float>(m->mouseInputX);
                    pendiente.dy += static_cast<float>(m->mouseInputY);
                    continue;
                }
                auto* boton = e->AsButtonEvent();
                if (!boton) continue;
                const auto codigo = boton->GetIDCode();
                const bool pulsado = boton->IsDown();
                const bool soltado = boton->IsUp();
                if (!pulsado && !soltado) continue;

                // Siendo criatura y con el menú cerrado, los clics atacan. Las comprobaciones finas
                // (otros menús, pausa) van en el hilo principal.
                if (!abierto && esCriatura && pulsado && boton->GetDevice() == RE::INPUT_DEVICE::kMouse && codigo <= 1) {
                    const bool potente = codigo == 1;
                    SKSE::GetTaskInterface()->AddTask([potente]() { Atacar(potente); });
                }

                if (boton->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
                    if (pulsado && (codigo == tecla || (abierto && codigo == 0x01))) {
                        if (abierto) {
                            Cerrar();
                        } else if (SePuedeAbrir()) {
                            Abrir();
                        }
                        continue;
                    }
                    if (!abierto) continue;
                    std::scoped_lock bloqueo{ mutexPendiente };
                    if (const auto k = Tecla(codigo); k != ImGuiKey_None) pendiente.teclas.emplace_back(k, pulsado);
                    if (const auto c = Letra(codigo); c && pulsado) pendiente.letras.push_back(c);
                } else if (boton->GetDevice() == RE::INPUT_DEVICE::kMouse && abierto) {
                    std::scoped_lock bloqueo{ mutexPendiente };
                    if (codigo <= 2) {
                        pendiente.botones.emplace_back(static_cast<int>(codigo), pulsado);
                    } else if (pulsado && (codigo == 8 || codigo == 9)) {
                        pendiente.rueda += codigo == 8 ? 1.0f : -1.0f;
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // ---- Ventana --------------------------------------------------------------------------

    char                     textoBusqueda[128]{};
    std::string              busquedaAnterior = "\x01";
    std::vector<std::size_t> filtradas;
    int                      seleccion = -1;
    Pestana                  pestana = Pestana::kCriaturas;
    Pestana                  pestanaAnterior = Pestana::kOcultos;
    std::vector<std::string> accionesFiltradas;  // en la pestaña Acciones, en vez de filtradas
    std::size_t              totalAcciones = 0;
    int                      versionVista = -1;
    bool                     mostrarTodas = false, mostrarTodasAnterior = false;
    bool                     cerrarAlEjecutar = true;

    void Filtrar()
    {
        const auto busqueda = Plegar(textoBusqueda);
        const int  version = versionAcciones.load();
        if (busqueda == busquedaAnterior && pestana == pestanaAnterior && version == versionVista && mostrarTodas == mostrarTodasAnterior) return;
        busquedaAnterior = busqueda;
        pestanaAnterior = pestana;
        versionVista = version;
        mostrarTodasAnterior = mostrarTodas;
        filtradas.clear();
        accionesFiltradas.clear();
        if (pestana == Pestana::kAcciones) {
            std::scoped_lock bloqueo{ mutexAcciones };
            totalAcciones = 0;
            for (const auto& a : acciones) {
                if (!mostrarTodas && AccionOculta(a)) continue;
                ++totalAcciones;
                if (busqueda.empty() || Plegar(a).find(busqueda) != std::string::npos) accionesFiltradas.push_back(a);
            }
            seleccion = accionesFiltradas.empty() ? -1 : 0;
            return;
        }
        for (std::size_t i = 0; i < entradas.size(); ++i) {
            if (entradas[i].pestana != pestana) continue;
            if (busqueda.empty() || entradas[i].busqueda.find(busqueda) != std::string::npos) filtradas.push_back(i);
        }
        seleccion = filtradas.empty() ? -1 : 0;
    }

    std::size_t Visibles() { return pestana == Pestana::kAcciones ? accionesFiltradas.size() : filtradas.size(); }

    void Ejecutar()
    {
        if (seleccion < 0 || seleccion >= static_cast<int>(accionesFiltradas.size())) return;
        EjecutarAccion(accionesFiltradas[seleccion]);
        if (cerrarAlEjecutar) Cerrar();
    }

    void Transformarse()
    {
        if (pestana == Pestana::kAcciones) return Ejecutar();
        if (seleccion < 0 || seleccion >= static_cast<int>(filtradas.size())) return;
        const auto& e = entradas[filtradas[seleccion]];
        Transformar(e.npc, e.raza, e.piel);
        Cerrar();
    }

    void DibujarVentana()
    {
        const auto& io = ImGui::GetIO();
        ImGui::SetNextWindowSize({ 760, 620 }, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({ io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f }, ImGuiCond_FirstUseEver, { 0.5f, 0.5f });
        if (!ImGui::Begin("Transformaciones", nullptr, ImGuiWindowFlags_NoCollapse)) {
            ImGui::End();
            return;
        }

        ImGui::TextUnformatted(pestana == Pestana::kAcciones ? "Busca una acción de tu criatura:" : "Busca un personaje, animal o criatura:");
        ImGui::SetNextItemWidth(-1);
        if (enfocarBusqueda) {
            ImGui::SetKeyboardFocusHere();
            enfocarBusqueda = false;
        }
        const bool intro = ImGui::InputText("##buscar", textoBusqueda, sizeof(textoBusqueda), ImGuiInputTextFlags_EnterReturnsTrue);

        if (ImGui::BeginTabBar("pestanas")) {
            static constexpr std::array<std::pair<Pestana, const char*>, 3> nombres{ { { Pestana::kHumanos, "Humanos" },
                                                                                        { Pestana::kCriaturas, "Criaturas" },
                                                                                        { Pestana::kOcultos, "Ocultos (repetidos)" } } };
            bool alguna = false;
            for (const auto& [cual, nombre] : nombres) {
                const auto titulo = std::format("{} ({})###{}", nombre, totales[static_cast<int>(cual)], static_cast<int>(cual));
                if (ImGui::BeginTabItem(titulo.c_str())) {
                    pestana = cual;
                    alguna = true;
                    ImGui::EndTabItem();
                }
            }
            // Solo transformado en criatura; con una persona no sale.
            if (hayAcciones && ImGui::BeginTabItem("Acciones###3")) {
                pestana = Pestana::kAcciones;
                alguna = true;
                ImGui::EndTabItem();
            }
            if (!alguna) pestana = Pestana::kCriaturas;
            ImGui::EndTabBar();
        }
        Filtrar();

        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && seleccion + 1 < static_cast<int>(Visibles())) ++seleccion;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && seleccion > 0) --seleccion;

        if (pestana == Pestana::kAcciones) {
            ImGui::Text("%zu resultados de %zu", accionesFiltradas.size(), totalAcciones);
            ImGui::SameLine();
            ImGui::Checkbox("Mostrar todas", &mostrarTodas);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("También las de muerte, ragdoll, ejecuciones o muebles, que pueden dejarte tirado\n"
                                  "(usa Desatascar), y las internas de pasos y sonidos.");
            }
            ImGui::SameLine();
            ImGui::Checkbox("Cerrar el menú al ejecutar", &cerrarAlEjecutar);
        } else {
            ImGui::Text("%zu resultados de %zu", filtradas.size(), totales[static_cast<int>(pestana)]);
        }
        const float alto = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 6.5f;
        if (ImGui::BeginChild("lista", { 0, alto }, true)) {
            ImGuiListClipper recorte;
            recorte.Begin(static_cast<int>(Visibles()));
            while (recorte.Step()) {
                for (int i = recorte.DisplayStart; i < recorte.DisplayEnd; ++i) {
                    const char* etiqueta = pestana == Pestana::kAcciones ? accionesFiltradas[i].c_str() : entradas[filtradas[i]].etiqueta.c_str();
                    ImGui::PushID(i);
                    if (ImGui::Selectable(etiqueta, seleccion == i, ImGuiSelectableFlags_AllowDoubleClick)) {
                        seleccion = i;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) Transformarse();
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();

        const bool haySeleccion = seleccion >= 0 && seleccion < static_cast<int>(Visibles());
        ImGui::BeginDisabled(!haySeleccion);
        if (ImGui::Button(pestana == Pestana::kAcciones ? "Ejecutar" : "Transformarme", { 200, 0 }) || (intro && haySeleccion)) Transformarse();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!transformado);
        if (ImGui::Button("Volver a mi aspecto", { 200, 0 })) {
            Volver();
            Cerrar();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Desatascar", { 140, 0 })) {
            Cerrar();
            SKSE::GetTaskInterface()->AddTask([]() { Desatascar(true); });
            PonerEstado("Desatascado. Si sigues sin moverte, el registro dice por qué.");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Si no te puedes mover: devuelve los controles y la postura de pie.");
        ImGui::SameLine();
        if (ImGui::Button("Cerrar", { 120, 0 })) Cerrar();

        std::string copia;
        {
            std::scoped_lock bloqueo{ mutexEstado };
            copia = estado;
        }
        ImGui::TextDisabled("%s", copia.empty() ? "F11 o Esc para cerrar. Doble clic o Intro para transformarte." : copia.c_str());

        ImGui::Separator();
        if (camaraPropia) {
            std::string raza;
            {
                std::scoped_lock bloqueo{ mutexCamara };
                raza = razaCamara;
            }
            ImGui::Text("Cámara de %s (se guarda para esta raza)", raza.c_str());
            float altura = alturaCamara.load();
            ImGui::SetNextItemWidth(-140);
            if (ImGui::SliderFloat("Altura", &altura, 0.0f, 800.0f, "%.0f")) alturaCamara = altura;
            if (ImGui::IsItemDeactivatedAfterEdit()) guardarCamara = true;
            float distancia = distanciaCamara.load();
            ImGui::SetNextItemWidth(-140);
            if (ImGui::SliderFloat("Distancia", &distancia, 0.0f, 1500.0f, distancia <= 0.0f ? "la del juego" : "%.0f")) {
                distanciaCamara = distancia;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) guardarCamara = true;
        } else {
            ImGui::TextDisabled("La altura y la distancia de la cámara se ajustan aquí al ser una criatura.");
        }
        ImGui::End();
    }

    // ---- DirectX --------------------------------------------------------------------------

    using Present_t = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
    Present_t            presentOriginal = nullptr;
    ID3D11Device*        dispositivo = nullptr;
    ID3D11DeviceContext* contexto = nullptr;
    bool                 iniciado = false;
    bool                 estabaAbierto = false;
    ImVec2               raton{ 0, 0 };
    LARGE_INTEGER        frecuencia{}, ultimo{};

    bool IniciarImGui(IDXGISwapChain* cadena)
    {
        if (FAILED(cadena->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dispositivo)))) return false;
        dispositivo->GetImmediateContext(&contexto);
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.MouseDrawCursor = true;
        if (std::filesystem::exists("C:\\Windows\\Fonts\\segoeui.ttf")) {
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 22.0f, nullptr, io.Fonts->GetGlyphRangesDefault());
        }
        ImGui::StyleColorsDark();
        ImGui::GetStyle().WindowRounding = 6.0f;
        ImGui_ImplDX11_Init(dispositivo, contexto);
        QueryPerformanceFrequency(&frecuencia);
        QueryPerformanceCounter(&ultimo);
        spdlog::info("ImGui listo");
        return true;
    }

    void AplicarEntrada()
    {
        auto&     io = ImGui::GetIO();
        Pendiente p;
        {
            std::scoped_lock bloqueo{ mutexPendiente };
            std::swap(p, pendiente);
        }
        raton.x = std::clamp(raton.x + p.dx, 0.0f, io.DisplaySize.x - 1);
        raton.y = std::clamp(raton.y + p.dy, 0.0f, io.DisplaySize.y - 1);
        io.AddMousePosEvent(raton.x, raton.y);
        for (const auto& [boton, abajo] : p.botones) io.AddMouseButtonEvent(boton, abajo);
        if (p.rueda != 0) io.AddMouseWheelEvent(0, p.rueda);
        for (const auto& [k, abajo] : p.teclas) io.AddKeyEvent(k, abajo);
        for (const auto c : p.letras) io.AddInputCharacter(static_cast<unsigned>(c));
    }

    void Dibujar(IDXGISwapChain* cadena)
    {
        if (!iniciado) {
            iniciado = IniciarImGui(cadena);
            if (!iniciado) return;
        }
        LARGE_INTEGER ahora;
        QueryPerformanceCounter(&ahora);
        const float delta = static_cast<float>(ahora.QuadPart - ultimo.QuadPart) / static_cast<float>(frecuencia.QuadPart);
        ultimo = ahora;

        // La cámara se revisa unas diez veces por segundo, esté o no abierto el menú.
        static LARGE_INTEGER ultimaRevision{};
        if (ahora.QuadPart - ultimaRevision.QuadPart > frecuencia.QuadPart / 10) {
            ultimaRevision = ahora;
            SKSE::GetTaskInterface()->AddTask(Vigilar);
        }

        if (!abierto) {
            if (estabaAbierto) {
                std::scoped_lock bloqueo{ mutexPendiente };
                pendiente = {};
            }
            estabaAbierto = false;
            return;
        }

        auto&                io = ImGui::GetIO();
        DXGI_SWAP_CHAIN_DESC desc{};
        cadena->GetDesc(&desc);
        io.DisplaySize = { static_cast<float>(desc.BufferDesc.Width), static_cast<float>(desc.BufferDesc.Height) };
        io.DeltaTime = delta > 0.0f ? delta : 1.0f / 60.0f;
        if (!estabaAbierto) raton = { io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f };
        estabaAbierto = true;
        AplicarEntrada();

        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        DibujarVentana();
        ImGui::Render();

        ID3D11Texture2D* fondo = nullptr;
        if (FAILED(cadena->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&fondo)))) return;
        ID3D11RenderTargetView* vista = nullptr;
        dispositivo->CreateRenderTargetView(fondo, nullptr, &vista);
        fondo->Release();
        if (!vista) return;

        ID3D11RenderTargetView* vistasPrevias[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* profundidadPrevia = nullptr;
        contexto->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, vistasPrevias, &profundidadPrevia);
        contexto->OMSetRenderTargets(1, &vista, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        contexto->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, vistasPrevias, profundidadPrevia);
        for (auto* v : vistasPrevias) {
            if (v) v->Release();
        }
        if (profundidadPrevia) profundidadPrevia->Release();
        vista->Release();
    }

    HRESULT WINAPI PresentGancho(IDXGISwapChain* cadena, UINT sincronia, UINT banderas)
    {
        Dibujar(cadena);
        return presentOriginal(cadena, sincronia, banderas);
    }

    // Se cambia la entrada 8 (Present) de la tabla virtual de la cadena de intercambio del juego.
    bool EngancharPresent()
    {
        auto* render = RE::BSRenderManager::GetSingleton();
        auto* cadena = render ? render->GetRuntimeData().swapChain : nullptr;
        if (!cadena) {
            spdlog::error("No encuentro la cadena de intercambio de DirectX");
            return false;
        }
        auto** tabla = *reinterpret_cast<void***>(cadena);
        DWORD  proteccion;
        if (!VirtualProtect(&tabla[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &proteccion)) return false;
        presentOriginal = reinterpret_cast<Present_t>(tabla[8]);
        tabla[8] = reinterpret_cast<void*>(&PresentGancho);
        VirtualProtect(&tabla[8], sizeof(void*), proteccion, &proteccion);
        spdlog::info("Present enganchado");
        return true;
    }

    // ---- Arranque -------------------------------------------------------------------------

    void LeerAjustes()
    {
        wchar_t ruta[MAX_PATH]{};
        HMODULE modulo = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&LeerAjustes), &modulo) &&
            GetModuleFileNameW(modulo, ruta, MAX_PATH)) {
            rutaIni = std::filesystem::path{ ruta }.replace_extension(L".ini");
            tecla = GetPrivateProfileIntW(L"General", L"Tecla", TECLA_POR_DEFECTO, rutaIni.c_str());
        }
        spdlog::info("Tecla del menu: DIK {:#x}", tecla);
    }

    void PrepararRegistro()
    {
        auto carpeta = SKSE::log::log_directory();
        if (!carpeta) return;
        auto registro = std::make_shared<spdlog::logger>(
            "registro", std::make_shared<spdlog::sinks::basic_file_sink_mt>((*carpeta / "Transformaciones.log").string(), true));
        registro->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(registro));
    }

    void AlRecibirMensaje(SKSE::MessagingInterface::Message* mensaje)
    {
        // Al cargar una partida en la que el jugador ya era una criatura.
        if (mensaje->type == SKSE::MessagingInterface::kPostLoadGame) {
            SKSE::GetTaskInterface()->AddTask(Vigilar);
            return;
        }
        if (mensaje->type != SKSE::MessagingInterface::kDataLoaded) return;
        CargarLista();
        if (auto* entrada = RE::BSInputDeviceManager::GetSingleton()) entrada->AddEventSink(Teclado::Instancia());
        if (auto* eventos = RE::ScriptEventSourceHolder::GetSingleton()) {
            eventos->AddEventSink<RE::TESSwitchRaceCompleteEvent>(FinDeCambioDeRaza::Instancia());
        }
        EngancharPresent();
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    PrepararRegistro();
    LeerAjustes();

    return SKSE::GetMessagingInterface()->RegisterListener(AlRecibirMensaje);
}
