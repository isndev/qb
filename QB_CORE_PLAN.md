# QB‑Core — Deep Review & Improvement Plan

> Scope : `qb/include/qb/core/**` + `qb/source/core/src/**`
> Standard cible : C++23 (`CMAKE_CXX_STANDARD 23`)
> Objectif : performance extrême, sûreté, élégance moderne.
> Date : 2026‑04‑19 — Auteur : review interne.
> Dernière mise à jour : 2026‑04‑19 (phases 0/2/3 terminées ; 2.1/2.3/2.4 HIGH résolus ; 2.17 C++23 terminé ; invariants centralisés doc 2.20 ; **suite de régression `test-core-improvements.cpp` ajoutée — 19 tests verts couvrant 1/3/4/9/11/12/13/14/16/17**).

---

## 0. TL;DR — Tableau récapitulatif

Légende : ☐ à faire — ☑ appliqué et validé (tests verts) — ⚠ partiellement appliqué.

| #  | État | Zone                          | Sévérité | Type            | Résumé                                                                                           |
|----|------|-------------------------------|----------|-----------------|--------------------------------------------------------------------------------------------------|
| 1  | ☑ | `Event::type_to_id` (Release)    | **HIGH** | Correctness     | `type_id<T>()` + `type_to_id<T>()` routent par un **compteur magic-static atomique** (dense, sans collision jusqu'à 65535 types). 16 bits conservés pour préserver la cacheline. |
| 2  | ⚠ | `base_pipe::allocate_back`       | **HIGH** | Correctness     | Conservé **par design** (cacheline). Contrat « trivially relocatable, pas de self-pointer » désormais documenté dans `readme/7_reference/core_invariants.md` §3.3. |
| 3  | ☑ | `ServiceActor<Tag>::ServiceIndex`| **HIGH** | Correctness / Race | `_nb_service` → `std::atomic<ServiceId>` + magic static par `Tag` dans `registerIndex`.     |
| 4  | ☑ | `__flush_all__` deadlock recovery| **HIGH** | Correctness     | Algorithme réécrit : backoff borné (spin → yield → bail + notify) ; `_event_safe_deadlock` supprimé. |
| 5  | ☑ | `VirtualCore::addReferencedActor`| **MED**  | Consistency     | Route via `qb::allocate_actor` + `ActorProxy::setTypeInfo`. Voir 2.5.                            |
| 6  | ☑ | `CallbackMap` snapshot           | **MED**  | Performance     | Flat `_callback_list` synchronisé au register/unregister ; snapshot = 1 `memcpy`.                |
| 7  | ☑ | `_router.memh` dispatch          | **MED**  | Performance     | `semh<_,void>` dispatch par function pointer + trampoline typé (aucune virtuelle/alloc).         |
| 8  | ☑ | `Actor::Actor()` w/o handler     | **MED**  | Safety          | `assert(VirtualCore::_handler)` ajouté dans les 3 constructeurs.                                 |
| 9  | ☑ | `Actor::_alive` (bool plain)     | **LOW**  | Clarity         | Invariant thread-ownership explicité en doc en-tête + §2.3 de `core_invariants.md`.              |
| 10 | ☑ | `Main::_instances`               | **LOW**  | Dead code       | `_instances` + `_instances_lock` supprimés (header + cpp).                                       |
| 11 | ☑ | `NoAffinity` constant            | **LOW**  | API             | Gardé (API utilisateur) : doc précisée ; `VirtualCore::__init__` filtre désormais les `CoreId >= MaxCores` → sentinel sûr. |
| 12 | ☑ | `Main::core()` range             | **LOW**  | Bug latent      | `index >= qb::MaxCores`, message dynamique `to_string(MaxCores - 1)`.                            |
| 13 | ☑ | `spawn_async` heap alloc         | **LOW**  | Performance     | Compteur `shared_ptr<atomic>` désormais init dans le ctor → hot path sans branche.               |
| 14 | ☑ | `TActorFactory::create_impl`     | **LOW**  | Performance     | Point d'extension `qb::allocate_actor<T>` → support PMR / pool au choix de l'app.                |
| 15 | ☑ | `std::set<ServiceId>` pour IDs   | **LOW**  | Performance     | Remplacé par `ServiceIdPool` (bitset 8 KiB, `countr_zero`, cursor) — O(1) acquire/release.       |
| 16 | ☑ | `_sleep_count` reset logic       | **LOW**  | Clarity         | Struct `Metrics` dédiée : `had_activity()`, `carry_over()`, `_spin_credit` (nom explicite).      |
| 17 | ☑ | `addRefActor` lifecycle          | **MED**  | Safety          | `qb::RefActorHandle<T>` + `Actor::addRefHandle<T>` : lookup sûr, pointeur auto‑invalide.         |
| 18 | ☑ | `Actor::Actor()` ctor logic      | **LOW**  | API             | Overload `Actor(qb::no_default_events_t)` pour actors légers sans 4 registrations par défaut.    |
| 19 | ☑ | C++23 modernisation              | **LOW**  | Style           | `std::jthread`+`std::stop_token` (Main/VirtualCore), `std::span` (__receive_events__), `[[assume]]` (router), `std::bit`, `[[nodiscard]]`. |
| 20 | ☑ | Doc — invariants coro/actor      | **LOW**  | Doc             | Nouveau `qb/readme/7_reference/core_invariants.md` — single source of truth.                     |

---

## 1. Architecture générale (état des lieux)

### 1.1 Forces
- **Modèle acteur strict** : un `VirtualCore` par thread, actors non‑partagés, communication par `Event`.
- **`thread_local VirtualCore* _handler`** → routage zéro‑coût côté expéditeur.
- **Deux chemins d'envoi** :
  - `push<T>` : ordonné, `allocate_back` → FIFO par pipe.
  - `send<T>` : non ordonné, `allocate` (peut réutiliser l'espace avant), QOS0 only.
- **Pipe mono‑core** (`_mono_pipe` / `_mono_pipe_swap`) : évite la MPSC pour les events `dest._core_id == _index` (double buffering, swap au début de `__receive__`).
- **Alignement cache‑line** sur `Event` et `allocator::pipe` → pas de false‑sharing inter‑core.
- **Concepts C++23** (`event_type`, `actor_type`, `service_type`, `callback_type`, `trivial_event`, `event_qos0_type`, `service_event_type`) bien utilisés dans `Actor.tpp`, `Pipe.tpp`, `VirtualCore.tpp`.
- **`std::bit_cast`** (C++20) dans `ActorId.cpp` → UB éliminé.

### 1.2 Faiblesses structurelles (vue d'ensemble)
1. **Couplage fort** `Actor ↔ VirtualCore::_handler` : tout passe par un pointeur thread‑local. Test/isolation difficile.
2. **Dispatch dynamique** : `router::memh` chaîne 2 virtuelles par event (résolveur d'event + résolveur de handler).
3. **Zones non trivialement thread‑safe** : ~~`VirtualCore::_nb_service` / `getServices()`~~ corrigés (2.3) ; `Main::_instances` subsiste.
4. **Lifecycle des actors référencés** : aucun contrat fort, pointeur cru.

---

## 2. Findings détaillés

### 2.1 HIGH — `Event::type_to_id` peut collisionner (Release) ☑ DONE

**Décision conservée** : `EventId` reste `uint16_t` — contrainte de design explicite du framework pour garder chaque `Event` en 1 cacheline.

**Fix retenu** : remplacer l'origine du nombre plutôt que sa largeur. `qb::type_id<T>()` et `Event::type_to_id<T>()` délèguent maintenant à un **compteur magic-static atomique** dans `qb::detail::type_id_for<T>` (`qb/include/qb/core/Event.h`) :

```cpp
namespace qb::detail {
inline std::atomic<TypeId> _type_id_counter{0};
template <typename T>
[[nodiscard]] inline TypeId type_id_for() noexcept {
    static const TypeId id = _type_id_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    return id;
}
}
```

Propriétés :
- **Dense** : numérotation `[1, NTypes]` sans trous.
- **Sans collision** : la seule possibilité d'en avoir est de dépasser 65535 types distincts par process (impossible en pratique).
- **Thread-safe cross-TU** : la barrière acquire des magic statics sérialise le `fetch_add` au premier appel.
- **Non-ASLR** : plus de narrowing d'adresse ; les IDs sont déterministes au sein d'un run.

Le tag `qb::type<T>` est conservé (vide) pour compat source si du code externe partialise dessus.

---

### 2.2 HIGH — `base_pipe` réalloue via `memcpy` des types non‑trivially‑copyable ⚠ KEPT BY DESIGN

**Décision** : la politique reste « events trivially relocatable ». La cacheline/latence gagnée par le `memcpy` vaut le contrat, et les seuls cas problématiques en pratique sont des events **avec self-pointer** (extrêmement rares).

**Action effectuée** : contrat rendu **explicite et central** dans `qb/readme/7_reference/core_invariants.md` §3.3 — « Events MUST be trivially relocatable : no self-pointers, no members registering themselves into an external registry in their ctor/dtor ». Les payloads usuels (POD, `std::string` avec SSO, `std::unique_ptr`, `std::vector`) satisfont déjà ce contrat.

À faire dans une Phase ultérieure (optionnel) : ajouter un `static_assert` guidé par concept sur `Actor::push<T>` dès que `std::is_trivially_relocatable` atterrira en standard (actuellement P1144 pas encore dans libc++/libstdc++).

---

### 2.3 HIGH — Init statique de `ServiceActor<Tag>::ServiceIndex` ☑ DONE

Deux correctifs combinés :
1. **`VirtualCore::_nb_service`** est désormais `std::atomic<ServiceId>{0}` (`qb/include/qb/core/VirtualCore.h`, `VirtualCore.cpp`). Toutes les lectures restent relaxed (publication garantie par l'*acquire edge* des magic statics).
2. **`Actor::registerIndex<Tag>()`** (`qb/include/qb/core/Actor.tpp`) utilise maintenant une magic static locale, garantissant par la norme C++ que le bloc d'enregistrement s'exécute **exactement une fois par `Tag`**, même en présence d'initialisations concurrentes depuis plusieurs TU / DSO :
   ```cpp
   static const ServiceId idx = [] {
       const auto new_id = VirtualCore::_nb_service.fetch_add(1, std::memory_order_relaxed) + 1;
       std::lock_guard lk(VirtualCore::servicesMutex());
       VirtualCore::getServices()[type_id<Tag>()] = new_id;
       return new_id;
   }();
   ```
3. Un nouveau mutex magic-static **`VirtualCore::servicesMutex()`** protège les mutations de la table `type_id<Tag>() → ServiceId`.
4. **`Actor::getServiceId<Tag>()`** (`Actor.tpp`) route désormais par `registerIndex<Tag>()` au lieu d'un accès `operator[]` direct — ce qui éliminait silencieusement la valeur `0` si l'index n'avait pas encore été initialisé dans l'ordre.

Tous les readers de `_nb_service` (`CoreInitializer`, `removeActor`, `ServiceIdPool::init`) utilisent `.load(std::memory_order_relaxed)`.

---

### 2.4 HIGH — `__flush_all__` : algorithme anti‑deadlock fragile ☑ DONE

Réécrit intégralement (`qb/source/core/src/VirtualCore.cpp`) autour d'un invariant explicite : **chaque appel à `__flush_all__` se termine en temps borné**. Le workflow peut alors reprendre la main, drainer son mailbox via `__receive__` et, symétriquement côté pair, libérer la pression qui bloquait le `try_send`.

Stratégie :
- **Plus de flag coopératif** : la table `SharedCoreCommunication::_event_safe_deadlock` est supprimée (Main.h + Main.cpp) — protocole ad hoc, race, livelock potentiel.
- **Backoff progressif, borné** (constantes locales à l'unité de compilation, tunables sans ABI break) :
  - `[0, 64)` : spin + `qb::spin_loop_pause()` (hint CPU, 0 appel OS).
  - `[64, 512)` : `std::this_thread::yield()` (le pair consommateur a un créneau garanti).
  - `>= 512` : *bail* propre — `pipe.reset(offset)` pour conserver le tail, `mail_box.notify()` sur le pair pour réveiller son éventuel `wait()`, puis on rend la main à `__workflow__`.
- **Événements QoS‑0** : sémantique best‑effort préservée (un seul `try_send`, drop en cas d'échec — identique à l'ancien comportement).
- **Plus de `goto`** : refactor en variables locales `cur/end/base` + `bool partial` ; extraction naturelle en deux blocs (`continue` pour avancer, `break` pour bailer).

Argumentation formelle de la terminaison : le budget de tentatives par événement QoS est borné par `kFlushYieldAttempts`. Les pipes ne sont parcourues qu'une fois. En sortie, soit la pipe est complètement drainée, soit son `_begin` est avancé de la portion envoyée. À l'itération suivante de `__workflow__`, `__receive__` draine le mailbox local → espace libéré côté mailbox pour tous les pairs → leur `try_send` aboutira. Pas de famine possible.

---

### 2.5 MED — Incohérence `addRefActor` vs. factory standard ☑ DONE

`VirtualCore::addReferencedActor` passe désormais par **le même point d'extension d'allocation** que `TActorFactory` (`qb::allocate_actor<_Actor>`) et utilise `ActorProxy::setTypeInfo<_Actor>` pour une métadonnée cohérente (nom démanglé + `id_type` typé). Voir `qb/include/qb/core/VirtualCore.tpp` et `qb/include/qb/core/Actor.h` (nouvelle méthode publique `ActorProxy::setTypeInfo`).

---

### 2.6 MED — `CallbackMap` snapshotée à chaque tour (hot‑path) ☑ DONE

Nouveau membre `VirtualCore::_callback_list` (`std::vector<ICallback*>`) tenu à jour par `registerCallback` / `__unregisterCallback` (swap‑pop). Le workflow remplace l'itération `unordered_map` + push_back par une copie flat (`cb_snapshot = _callback_list`) ; le snapshot reste nécessaire car `onCallback()` peut muter la liste (via `addRefActor`). Coût par itération : 1 `memcpy` contigu au lieu de N lookups hashmap.

---

### 2.7 MED — Dispatch double‑virtuel dans `memh` ☑ DONE (inner layer)

La spécialisation `semh<_RawEvent, void>` — utilisée par `memh` pour les dispatch hétérogènes — n'alloue plus de `std::unique_ptr<IHandlerResolver>` par abonné et supprime l'appel virtuel : chaque entrée stocke maintenant `{ void* handler, void(*trampoline)(void*, Event&) noexcept }`. Le trampoline est une fonction `static` typée par `_Handler` qui fait le `static_cast` et appelle directement `handler.on(event)` (inline‑friendly).

Gains mesurables : −1 alloc par `subscribe`, −1 indirect virtuel + −1 `vptr` par entrée sur le chemin `route`. L'API publique (`subscribe`/`unsubscribe`) reste identique ; la couche externe `memh::IEventResolver` (1 virtuelle par event type pour l'index `EventId → resolver`) est conservée pour préserver la compat binaire et le support ADL des event types.

---

### 2.8 MED — `Actor::Actor()` crashe silencieusement sans `_handler` ☑ DONE

Les trois constructeurs `Actor` (`Actor()`, `Actor(ActorId)`, `Actor(no_default_events_t)`) vérifient désormais `assert(VirtualCore::_handler != nullptr)` avec message explicite. En debug, toute instanciation hors d'un worker `VirtualCore` est immédiatement signalée plutôt que de provoquer un déréférencement nul silencieux.

---

### 2.9 MED — `addRefActor` : contrat de vie non exprimé ☑ DONE

Nouveau template `qb::RefActorHandle<T>` (`qb/include/qb/core/Actor.h`) :
- Capture `ActorId` + pointeur mis en cache lors de la création.
- `get()` vérifie la liveness via `VirtualCore::findActor<T>(id)` (lookup O(1) dans la map d'actors + check `is_alive()`) et met à jour le pointeur en cache.
- `operator->()` / `operator*()` avec assertion.
- `operator bool()` pour tests conditionnels.
- Helper **`Actor::addRefHandle<T>(args...)`** : crée un actor référencé et retourne directement un `RefActorHandle<T>`, prévenant tout stockage de pointeur cru.

L'API `addRefActor<T>` reste en place pour compat, mais la nouvelle façon recommandée est `addRefHandle<T>`.

---

### 2.10 LOW — Dead code : `Main::_instances` ☑ DONE

`Main::_instances` et `Main::_instances_lock` ont été supprimés (déclaration `Main.h` + définitions + maintenance `push_back/erase` dans `Main.cpp`). Aucune lecture externe n'y faisait référence, donc l'effacement est purement additif côté binaire (moins de membres, moins de lock, ABI source-compatible).

---

### 2.11 LOW — `NoAffinity` ☑ DONE (conservé comme API)

**Clarification** : `qb::NoAffinity` est une **constante publique** destinée à l'utilisateur pour exprimer « pas de pinning CPU » avec intention lisible dans le source. Deux améliorations :
1. Documentation enrichie (header `Main.h`) avec exemples d'usage et comportement runtime.
2. `VirtualCore::__init__` filtre désormais tout `CoreId >= qb::MaxCores` de la set d'affinité (utilisant `std::any_of` + `is_real_core` local) : `CoreIdSet{qb::NoAffinity}` n'appelle aucune API OS de pinning, évitant toute UB liée à un bit hors des bornes de `cpu_set_t` / `DWORD_PTR`.

---

### 2.12 LOW — `Main::core()` magic number 255 ☑ DONE

Remplacé par `index >= static_cast<CoreId>(qb::MaxCores)` + message d'exception dynamique (`std::to_string(qb::MaxCores - 1)`). Plus aucun magic number ; `MaxCores` (défini dans `qb/include/qb/core/ActorId.h`) reste la seule source de vérité.

---

### 2.20 LOW — Documentation centralisée des invariants ☑ DONE

Nouveau fichier **`qb/readme/7_reference/core_invariants.md`** (et lien ajouté dans le `README.md` de la section 7_reference) :
- §1 Modèle de thread (un `VirtualCore` ↔ un `std::jthread`).
- §2 Cycle de vie actor (construction / init / steady / destruction).
- §3 Système d'événements (identité, push/send/broadcast, layout cacheline, contrat de relocation 2.2, deadlock recovery 2.4).
- §4 Cheat-sheet de memory ordering (tableau data → access pattern → why safe).
- §5 Coroutines & async I/O.
- §6 Affinité CPU & shutdown (`NoAffinity`, `std::jthread` / `std::stop_source`).
- §7 Do / Don't.

Références croisées explicites avec `QB_CORE_PLAN.md` (findings) et la doc de référence existante.

---

### 2.13 LOW — `spawn_async` : heap alloc inutile ☑ DONE

Le compteur de coroutines (`active_coroutines_`) est désormais **eagerly alloué** via un `std::make_shared<std::atomic<std::size_t>>` dans l'initialiseur de membre de la classe `Actor`. Conséquences :
- `spawn_async` perd sa branche `if (!active_coroutines_)` : le hot path se résume à 1 atomic `fetch_add` + 1 `spawn()`.
- Le shared_ptr reste requis pour permettre aux guards RAII de coroutines orphelines de décrémenter en toute sûreté après destruction de l'actor.
- Coût : 1 alloc (`make_shared` — 1 bloc fusionné) par actor, compensé par la suppression d'une branche sur chaque `spawn_async`.

Un compteur **intrusive** (inline dans `Actor` + deux atomics combinés refcount/active) est envisageable pour 0 alloc, mais non retenu : la complexité supplémentaire ne paye que pour des applications très coroutine‑lourdes.

---

### 2.18 LOW — Pool d'actors / PMR ☑ DONE

Introduction d'un **point d'extension** au niveau API :
```cpp
template <typename _Actor, typename... _Args>
[[nodiscard]] inline _Actor* qb::allocate_actor(_Args&&... args);
```
Implémentation par défaut = `new _Actor(args...)`. Les utilisateurs peuvent le spécialiser pour un acteur donné pour brancher un `std::pmr::polymorphic_allocator`, un pool monotone ou une arena.
Les deux chemins internes (`TActorFactory::create_impl` **et** `VirtualCore::addReferencedActor`) routent maintenant par ce point, garantissant un comportement cohérent quelle que soit la voie de création.

---

### 2.14 LOW — `std::set<ServiceId>` pour les IDs disponibles ☑ DONE

Nouvelle classe interne **`VirtualCore::ServiceIdPool`** (`qb/include/qb/core/VirtualCore.h`) :
- Stockage `std::array<uint64_t, 1024>` (8 KiB) — bit = 1 ⇔ SID libre.
- `acquire()` : `std::countr_zero` sur le premier mot non nul à partir d'un curseur `_next_word` → O(1) amorti.
- `release(sid)` : flip d'un bit + update du curseur (on privilégie la réutilisation du plus petit SID).
- `empty()`/`size()` en temps constant via un compteur cached.

Remplace l'allocation de nœuds du red‑black tree et supprime les traversées logarithmiques. Zéro allocation dynamique sur tout le cycle de vie.

---

### 2.15 LOW — `_metrics.reset()` obscur ☑ DONE

Nouvelle `struct VirtualCore::Metrics` dotée :
- D'un nom de champ explicite : **`_spin_credit`** (ex‑`_sleep_count`) — « crédit de polls lock‑free avant de consentir à bloquer ».
- D'une méthode `had_activity()` claire et `[[nodiscard]]`.
- D'une méthode `carry_over()` qui *explique* la mécanique : total d'activité observée + crédit restant = crédit pour la prochaine itération, puis remise à zéro des compteurs (préservant `_nanotimer`).

La boucle `__workflow__` appelle `carry_over()` puis décrémente `_spin_credit` ; l'ancien idiome `*this = {sum};` est supprimé.

---

### 2.16 LOW — Registration systématique dans `Actor::Actor()` ☑ DONE

Ajout du **tag `qb::no_default_events_t`** (+ l'instance inline `qb::no_default_events`) et d'un nouveau constructeur protégé `explicit Actor(no_default_events_t)`. Les classes dérivées qui passent ce tag à `Actor(...)` héritent d'un actor **sans** subscriptions par défaut — idéal pour les pools d'actors éphémères. La documentation rappelle clairement qu'il reste recommandé de `registerEvent<KillEvent>(*this)` dans `onInit()` pour permettre un arrêt propre.

---

### 2.17 LOW — Modernisation C++23 suggérée ☑ DONE

| Feature              | Statut    | Détail                                                                            |
|----------------------|-----------|-----------------------------------------------------------------------------------|
| `std::jthread`       | ☑         | `Main::_cores` est maintenant `std::vector<std::jthread>` → RAII `request_stop()` + `join()` sur destruction. |
| `std::stop_token`    | ☑         | `Main::_stop_source` → token injecté dans chaque worker via `CoreSpawnerParameter::stop_token` et stocké dans `VirtualCore::_stop_token`. Polled dans `__workflow__` (fallback signal-free pour shutdown propre). |
| `std::span`          | ☑         | `VirtualCore::__receive_events__(std::span<EventBucket>)` remplace la paire `EventBucket*/size_t` — bounds-aware, zéro coût runtime. |
| `[[assume(...)]]`    | ☑         | Macro portable `QB_ASSUME(cond)` (`qb/utility/branch_hints.h`) — C++23 `[[assume]]` avec fallbacks `__builtin_assume` (Clang), `__assume` (MSVC), `__builtin_unreachable` (GCC). Appliquée dans `router::semh<_,void>::route()` et `router::memh<...>::route()` pour éliminer les checks redondants du function pointer / `unique_ptr` dans les hot paths. |
| `std::bit` / `countr_zero` | ☑ | Utilisé dans `ServiceIdPool::acquire()` — scan bit via intrinsèque CPU.          |
| `[[nodiscard]]`      | ☑ étendus | Nouvelles API (`RefActorHandle`, `ServiceIdPool`, `Metrics::had_activity`).      |
| `std::expected`      | ☐         | Refactor futur de `addActor`/`initActor` (casse l'API) — reporté (Phase 4).     |
| `std::flat_map`      | ☐         | Compatibilité libc++ en cours ; à instrumenter cas par cas.                      |
| `std::print` / `std::format` | ☐ | Migration logs conditionnée par libfmt ↔ libc++ uniforme (Phase 4).             |
| `deducing this`      | ☐ (exclu) | Exclu volontairement : compatibilité compilateurs anciens (Clang ≤ 16, gcc < 14).|

---

## 3. Plan d'action (priorisé)

### Phase 0 — Hygiène (≤ 1 jour)
- [x] Supprimer `Main::_instances` / `_instances_lock` (2.10).
- [x] `NoAffinity` conservé comme API publique : doc étendue + filtre `< MaxCores` dans `VirtualCore::__init__` (2.11).
- [x] Remplacer `index > 255` par `index >= qb::MaxCores` (2.12).
- [x] Invariant doc de `Actor::_alive` (2.9).
- [x] `ActorProxy::setTypeInfo` dans `addReferencedActor` (cf. 2.5).
- [x] Refactor `_metrics` (`Metrics::carry_over`, `_spin_credit`) (cf. 2.15).
- [x] Ajouter `assert(VirtualCore::_handler)` dans les ctors `Actor` (cf. 2.8).

### Phase 1 — Corrections de correction (1–3 jours)
- [x] **`type_id<T>` / `type_to_id<T>` collision-free** (2.1) — magic-static atomique, 16 bits conservés (cacheline).
- [x] **Doc formalisée** du contrat de relocation pour `Event` (2.2) — centralisée dans `core_invariants.md` §3.3.
- [x] **`ServiceIndex` via magic static + `std::atomic<ServiceId>`** (2.3).
- [x] Ajouter tests de régression — `qb/source/core/tests/system/test-core-improvements.cpp` (19 tests, 0,11 s) :
  - `TypeId.{IsStableAcrossCalls,IsCollisionFreeAcrossManyTypes,ConcurrentFirstInstantiationIsRaceFree,EventTypeToIdMatchesGlobalTypeId}` — couvre 2.1.
  - `ServiceIndex.IsUniqueAcrossTagsAndStableUnderConcurrency` — couvre 2.3.
  - `DeadlockRecovery.QoS1HighBackpressureNoLivelock` (4 sources × 200 K events) — couvre 2.4.
  - `RefActorHandle.ReportsNullptrAfterChildKill` — couvre 2.9 / 2.17.
  - `NoAffinity.{SetAffinityWithSentinelOnlyDoesNotCrash,SetAffinityWithMixedSentinelAndRealCoreIsSafe}` — couvre 2.11 ; impose `CoreIdSet` lenient (out-of-range CoreId silencieusement filtrés à la construction comme à l'usage).
  - `MainCoreRange.{RejectsExactlyAtMaxCores,RejectsAboveMaxCores,AcceptsLastValidCoreId}` — couvre 2.12.
  - `SpawnAsync.CounterIsEagerlyAllocatedAndZero` — couvre 2.13.
  - `AllocateActor.{IsRoutedFromStandardFactory,IsRoutedFromAddRefActor}` — couvre 2.5 + 2.14.
  - `NoDefaultEvents.{ActorWithoutKillRegistrationStillSelfKills,OptInKillEventEnablesExternalKill}` — couvre 2.16/2.18.
  - `StopSource.{ExplicitStopShutsDownPromptly,MainDestructorJoinsRunningWorkers}` — couvre 2.17 (jthread/stop_source).
- [ ] Stress de relocation d'events avec `std::string` > SSO (devrait être un trap documenté ; à ajouter si on retient un mode debug-only).

### Phase 2 — Performance (3–7 jours)
- [x] **Flat callback vector** (2.6).
- [x] **Dispatch par fonction‑pointeur** dans `router::semh<_,void>` (2.7).
- [x] **Bitset (`ServiceIdPool`) pour `_ids`** (2.14).
- [x] **`spawn_async` eager counter** (2.12).
- [x] **`qb::allocate_actor<T>` point d'extension PMR/pool** (2.13).
- [ ] Bench avant/après : throughput push/send, latence 99p, CPU idle.

### Phase 3 — Robustesse & API (1–2 semaines)
- [x] **Deadlock recovery redesign** (2.4) : backoff borné (spin → yield → bail+notify), flag coopératif supprimé, terminaison prouvée.
- [x] **`RefActorHandle<T>`** avec detection de mort (2.9) + `Actor::addRefHandle<T>`.
- [x] **Opt‑in default event registrations** via `qb::no_default_events_t` (2.16).
- [ ] **`std::expected<ActorId, ErrorCode>`** pour `addActor` et `initActor`.
- [x] **`std::jthread` + `std::stop_token`** côté `Main` (2.17) : `Main::_cores` RAII, token injecté vers `VirtualCore::_stop_token`, polled dans `__workflow__`.
- [x] **`std::span`** pour `VirtualCore::__receive_events__` (2.17).
- [x] **`QB_ASSUME`** (C++23 `[[assume]]`) dans hot paths router (2.17).

### Phase 4 — Modernisation C++23 continue
- [ ] Passer progressivement à `std::flat_map` pour les petites tables stables.
- [ ] Introduire `std::print` dans les chemins de log.
- [ ] `std::expected<ActorId, ErrorCode>` dans `addActor` / `initActor`.
- [ ] Explorer `std::execution` (Senders) comme alternative d'avenir à `ICallback`/`spawn_async`.

---

## 4. Risques & compatibilité

- **Changer `EventId` de 16→32 bits** casse l'ABI binaire de `Event`. Doit être fait ensemble avec une bump de version de la lib (peu impactant en monorepo).
- **Refactor `router::memh` vers fn pointers** : l'API publique (`subscribe<E>`, `unsubscribe<E>`) reste identique. Compatible source.
- **Flat callback vector** : purement interne, invisible.
- **Suppression `_instances`** : API inchangée.
- **`ServiceIndex` refactor** : pas de changement d'API ; comportement plus déterministe.

---

## 5. Couverture de tests à renforcer

| Test                                        | Justification                                    |
|---------------------------------------------|--------------------------------------------------|
| Collision simulée d'`EventId`               | Couvre 2.1.                                      |
| Event avec `std::string` ≥ SSO + resize pipe | Couvre 2.2 (doit échouer sans fix).             |
| `ServiceActor<A/B/C>` init concurrent multi‑TU | Couvre 2.3.                                   |
| Mailbox saturation deadlock pattern         | Couvre 2.4.                                      |
| Création/destruction massive d'actors (10⁶) | Couvre 2.14 + perf.                              |
| `spawn_async` intensif, actor tué pendant suspension | Couvre 2.12 + coroutine safety.         |
| `addRefActor` + child self‑kill avec parent non averti | Couvre 2.9.                           |

---

## 6. Conclusion

`qb-core` est une base **solide** et **bien pensée** pour l'ultra‑performance : choix d'un acteur par `VirtualCore`, pipe mono‑core double‑buffered, alignement cache, zéro contention hors mailbox MPSC. Les findings ci‑dessus sont soit des **correctness traps latents** (dont #2.1, #2.2, #2.3 sont les plus critiques), soit des **optimisations de second ordre** très rentables (flat cb vector, dispatch par fn‑pointer).

Priorité absolue : **Phase 1 (correctness)**. Phase 2 apporte un gain de latence mesurable en scénario de contention. Phase 3+ consolide l'ergonomie.

En appliquant ce plan, `qb-core` gagnerait à la fois en **sûreté** (bugs subtils éliminés), en **performance** (dispatch réduit, un minimum d'allocs) et en **élégance C++23** (expected, jthread, flat_map, deducing this), tout en préservant l'ABI et l'API existantes autant que possible.

