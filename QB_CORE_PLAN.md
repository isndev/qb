# QB‑Core — Deep Review & Improvement Plan

> Scope : `qb/include/qb/core/**` + `qb/source/core/src/**`
> Standard cible : C++23 (`CMAKE_CXX_STANDARD 23`)
> Objectif : performance extrême, sûreté, élégance moderne.
> Date : 2026‑04‑19 — Auteur : review interne.
> Dernière mise à jour : 2026‑04‑19 (phases 0, 2 & 3 appliquées ; 2.17 C++23 terminé).

---

## 0. TL;DR — Tableau récapitulatif

Légende : ☐ à faire — ☑ appliqué et validé (tests verts) — ⚠ partiellement appliqué.

| #  | État | Zone                          | Sévérité | Type            | Résumé                                                                                           |
|----|------|-------------------------------|----------|-----------------|--------------------------------------------------------------------------------------------------|
| 1  | ☐ | `Event::type_to_id` (Release)    | **HIGH** | Correctness     | Cast `reinterpret_cast<size_t>(&id) → uint16_t` — collisions possibles entre types.              |
| 2  | ☐ | `base_pipe::allocate_back`       | **HIGH** | Correctness     | Réallocation par `memcpy` alors que les `Event` peuvent être non‑trivially‑copyable.             |
| 3  | ☐ | `ServiceActor<Tag>::ServiceIndex`| **HIGH** | Correctness / Race | Init statique écrit `VirtualCore::_nb_service` sans synchro ni ordre garanti inter‑TU.      |
| 4  | ☐ | `__flush_all__` deadlock recovery| **HIGH** | Correctness     | Logique `_event_safe_deadlock` fragile, `goto`, lecture/écriture atomiques entrelacées.          |
| 5  | ☑ | `VirtualCore::addReferencedActor`| **MED**  | Consistency     | Route via `qb::allocate_actor` + `ActorProxy::setTypeInfo`. Voir 2.5.                            |
| 6  | ☑ | `CallbackMap` snapshot           | **MED**  | Performance     | Flat `_callback_list` synchronisé au register/unregister ; snapshot = 1 `memcpy`.                |
| 7  | ☑ | `_router.memh` dispatch          | **MED**  | Performance     | `semh<_,void>` dispatch par function pointer + trampoline typé (aucune virtuelle/alloc).         |
| 8  | ☑ | `Actor::Actor()` w/o handler     | **MED**  | Safety          | `assert(VirtualCore::_handler)` ajouté dans les 3 constructeurs.                                 |
| 9  | ☐ | `Actor::_alive` (bool plain)     | **LOW**  | Clarity         | Mutable, pas d'atomique ; OK mais mal documenté (ownership thread).                              |
| 10 | ☐ | `Main::_instances`               | **LOW**  | Dead code       | Registre global jamais consulté → suppression.                                                   |
| 11 | ☐ | `NoAffinity` constant            | **LOW**  | Dead code       | Déclaré, jamais consommé.                                                                        |
| 12 | ☐ | `Main::core()` range             | **LOW**  | Bug latent      | Magic number `255`, devrait être `MaxCores - 1` (= 255).                                         |
| 13 | ☑ | `spawn_async` heap alloc         | **LOW**  | Performance     | Compteur `shared_ptr<atomic>` désormais init dans le ctor → hot path sans branche.               |
| 14 | ☑ | `TActorFactory::create_impl`     | **LOW**  | Performance     | Point d'extension `qb::allocate_actor<T>` → support PMR / pool au choix de l'app.                |
| 15 | ☑ | `std::set<ServiceId>` pour IDs   | **LOW**  | Performance     | Remplacé par `ServiceIdPool` (bitset 8 KiB, `countr_zero`, cursor) — O(1) acquire/release.       |
| 16 | ☑ | `_sleep_count` reset logic       | **LOW**  | Clarity         | Struct `Metrics` dédiée : `had_activity()`, `carry_over()`, `_spin_credit` (nom explicite).      |
| 17 | ☑ | `addRefActor` lifecycle          | **MED**  | Safety          | `qb::RefActorHandle<T>` + `Actor::addRefHandle<T>` : lookup sûr, pointeur auto‑invalide.         |
| 18 | ☑ | `Actor::Actor()` ctor logic      | **LOW**  | API             | Overload `Actor(qb::no_default_events_t)` pour actors légers sans 4 registrations par défaut.    |
| 19 | ☑ | C++23 modernisation              | **LOW**  | Style           | `std::jthread`+`std::stop_token` (Main/VirtualCore), `std::span` (__receive_events__), `[[assume]]` (router), `std::bit`, `[[nodiscard]]`. |
| 20 | ☐ | Doc — invariants coro/actor      | **LOW**  | Doc             | Invariants thread‑safety/lifecycle dispersés ; un doc central serait précieux.                   |

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
3. **Zones non trivialement thread‑safe** : `VirtualCore::_nb_service`, `VirtualCore::getServices()`, `Main::_instances` — initialisations entre TU non ordonnées.
4. **Lifecycle des actors référencés** : aucun contrat fort, pointeur cru.

---

## 2. Findings détaillés

### 2.1 HIGH — `Event::type_to_id` peut collisionner (Release)

`qb/include/qb/core/Event.h`

```109:123:qb/include/qb/core/Event.h
#ifdef NDEBUG
    using id_type = EventId;
    template <typename T>
    [[nodiscard]] constexpr static id_type
    type_to_id() {
        return static_cast<id_type>(reinterpret_cast<std::size_t>(&qb::type<T>::id));
    }
#else
    using id_type = const char *;
```

`EventId = uint16_t`. On cast l'adresse d'un symbole statique → tronquée à 16 bits. Sur un binaire de taille réelle, la section `.rodata`/`.bss` qui contient les `qb::type<T>::id` peut dépasser 64 KB : **deux `type<T>::id` différents peuvent partager les mêmes 16 bits bas**.

Conséquence : collision de clé dans `router::memh::_registered_events` (`qb::unordered_map<uint16_t, …>`), un handler `SomeEvent` pourrait recevoir un `OtherEvent`. Comportement sournois, apparaissant seulement dans les binaires volumineux avec beaucoup de types d'events.

**Recommandé** : `EventId = uint32_t` (ou `uintptr_t` direct ; en Release, garder le `static_cast` non truncating). Pas d'impact mesurable — le header aligne sur cache‑line de toute façon.

```cpp
using EventId = std::uint32_t;   // ou uintptr_t
template <typename T>
[[nodiscard]] static constexpr id_type type_to_id() noexcept {
    return static_cast<id_type>(reinterpret_cast<std::uintptr_t>(&qb::type<T>::id));
}
```

---

### 2.2 HIGH — `base_pipe` réalloue via `memcpy` des types non‑trivially‑copyable

`qb/include/qb/system/allocator/pipe.h`

```369:377:qb/include/qb/system/allocator/pipe.h
            const auto new_data = base_type::allocate(new_capacity);
            std::memcpy(new_data, _data + _begin, nb_item * sizeof(T));
            if (_capacity)
                base_type::deallocate(_data, _capacity);
```

`T = EventBucket` (POD), OK pour le type du buffer. Mais les *events* stockés dans le buffer peuvent contenir des membres non‑trivially‑copyable (ex : `std::string` avec SSO pointeur → `_M_local_buf`). Après `memcpy + deallocate`, les self‑pointers pointent sur mémoire libérée.

La règle de workspace (`cpp.mdc`) recommande `qb::string<N>` pour éviter ça ; mais la **header docs de `Actor::push<T>`** affirme « Supports events with non‑trivially destructible members (e.g., std::string, std::vector) » — **fausse garantie**.

**Options** :
- A. **Documenter** formellement l'interdiction d'SSO/pointeurs self‑référentiels dans les events (préciser la promesse = « destructible non trivialement ≠ relocatable »).
- B. Ajouter un `static_assert(std::is_trivially_relocatable_v<T>)` (proposal C++26, disponible via `__is_trivially_relocatable` sur Clang).
- C. Implémenter une relocalisation *type‑aware* via un registre de `move_constructor` par `EventId` (coût : 1 fn pointer par event type dans un registre global; invoqué seulement au resize — rare).

Solution recommandée : **A** (documentation + `static_assert` guidé par concept) pour rester performant ; **B** dès que le compilateur supporte `std::is_trivially_relocatable` en standard.

```cpp
// dans Actor::push
template <typename _Event, typename... _Args>
_Event &push(ActorId const &dest, _Args &&...args) const noexcept {
    static_assert(std::is_trivially_copyable_v<_Event> ||
                  requires { typename _Event::qb_relocatable_tag; },
                  "Events must be trivially copyable or opt-in relocation via qb_relocatable_tag");
    …
}
```

---

### 2.3 HIGH — Init statique de `ServiceActor<Tag>::ServiceIndex`

`qb/include/qb/core/VirtualCore.tpp`

```130:131:qb/include/qb/core/VirtualCore.tpp
template <typename Tag>
inline const ServiceId ServiceActor<Tag>::ServiceIndex = Actor::registerIndex<Tag>();
```

`registerIndex` :
```cpp
return VirtualCore::getServices()[type_id<Tag>()] = ++VirtualCore::_nb_service;
```

Problèmes :
- `VirtualCore::_nb_service` est un `ServiceId` non atomique, incrémenté depuis potentiellement plusieurs initialiseurs dynamiques (ordre non spécifié entre TU).
- Si deux TU référencent `ServiceActor<A>` et `ServiceActor<B>` avec initialisation parallèle (sur certains linkers/loaders de plugins), race possible.
- `getServices()` renvoie une `unordered_map` locale statique — initialisation lazy protégée par *magic statics*, OK ; mais l'accès subséquent non synchronisé.

**Fix recommandé** :
```cpp
static std::atomic<ServiceId> _nb_service{0};

template <typename Tag>
static ServiceId registerIndex() noexcept {
    static const ServiceId idx = []{
        auto id = _nb_service.fetch_add(1, std::memory_order_relaxed) + 1;
        // lock + insert dans getServices()
        return id;
    }();
    return idx;
}
```

Utiliser un **magic static** par `Tag` garantit l'unicité et la thread‑safety.

---

### 2.4 HIGH — `__flush_all__` : algorithme anti‑deadlock fragile

`qb/source/core/src/VirtualCore.cpp`

```184:216:qb/source/core/src/VirtualCore.cpp
                if (!try_send(event) && event.state.qos) {
                    ++_metrics._nb_event_sent_try;
                    static thread_local auto &current_lock =
                        _engine._event_safe_deadlock[_resolved_index];
                    current_lock.store(true, std::memory_order_release);
                    while (!try_send(event)) {
                        ++_metrics._nb_event_sent_try;
                        if (current_lock.load(std::memory_order_acquire)) {
                            _engine
                                ._event_safe_deadlock[_engine._core_set.resolve(
                                    event.dest.index())]
                                .store(false, std::memory_order_release);
                        } else {
                            pipe.reset(i - pipe.data());
                            goto end;
                        }
                    }
                }
```

Problèmes :
1. **`goto end`** saute hors d'un `for`‑range — moderne C++ permet `break` + flag.
2. **Protocole ad hoc** : chaque core possède un flag `bool atomic`. Si A bloque sur B, A met son flag à `true`, et à chaque itération remet à `false` le flag de B. B, lors de son propre flush, verra son flag à `false` → sortira. Protocole dérivé d'un patron maison, non documenté formellement, testé empiriquement.
3. **Race** : deux cores peuvent voir leurs flags entrelacés — risque de livelock théorique.
4. **Coût CPU** : spin + remise à false chaque tour jusqu'à ce que la mailbox se vide. En charge pleine, pertes de cycles.

**Recommandation** :
- Documenter le protocole (diagramme + preuve d'absence de deadlock).
- Remplacer le spin par un **backoff exponentiel** (`std::this_thread::yield()` → `spin_loop_pause()`) pour éviter la famine.
- Considérer un **futex / event** : si `try_send` échoue N fois, s'endormir sur la mailbox destination jusqu'à notification.
- Ou : repenser la MPSC pour offrir un `try_reserve_batch` → produce‑batch atomique et échec propre (plutôt que retry unitaire).

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

### 2.10 LOW — Dead code : `Main::_instances`, `NoAffinity`

- `Main::_instances` + `_instances_lock` : maintenus (add/erase) mais **jamais consultés** (grepped exhaustivement).
- `constexpr const CoreId NoAffinity = std::numeric_limits<CoreId>::max();` : déclaré, jamais consommé.

**Action** : supprimer les deux ; ça allège le header + évite les suppressions incorrectes si un process crashe avec 2 `Main` simultanés.

---

### 2.11 LOW — `Main::core(index)` : magic number

```cpp
if (index > 255) throw std::range_error("Max core id managed by qb is 255");
```

Alors que `constexpr size_t MaxCores = 256;` existe dans `ActorId.h`. Remplacer par `index >= MaxCores`.

---

### 2.12 LOW — `spawn_async` : heap alloc inutile ☑ DONE

Le compteur de coroutines (`active_coroutines_`) est désormais **eagerly alloué** via un `std::make_shared<std::atomic<std::size_t>>` dans l'initialiseur de membre de la classe `Actor`. Conséquences :
- `spawn_async` perd sa branche `if (!active_coroutines_)` : le hot path se résume à 1 atomic `fetch_add` + 1 `spawn()`.
- Le shared_ptr reste requis pour permettre aux guards RAII de coroutines orphelines de décrémenter en toute sûreté après destruction de l'actor.
- Coût : 1 alloc (`make_shared` — 1 bloc fusionné) par actor, compensé par la suppression d'une branche sur chaque `spawn_async`.

Un compteur **intrusive** (inline dans `Actor` + deux atomics combinés refcount/active) est envisageable pour 0 alloc, mais non retenu : la complexité supplémentaire ne paye que pour des applications très coroutine‑lourdes.

---

### 2.13 LOW — Pool d'actors / PMR ☑ DONE

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
- [ ] Supprimer `Main::_instances` / `_instances_lock` / `NoAffinity` (dead code).
- [ ] Remplacer `index > 255` par `index >= qb::MaxCores`.
- [x] `ActorProxy::setTypeInfo` dans `addReferencedActor` (cf. 2.5).
- [x] Refactor `_metrics` (`Metrics::carry_over`, `_spin_credit`) (cf. 2.15).
- [x] Ajouter `assert(VirtualCore::_handler)` dans les ctors `Actor` (cf. 2.8).

### Phase 1 — Corrections de correction (1–3 jours)
- [ ] **`EventId = uint32_t`** (Release). Mise à jour dans tous les headers qui référencent `_EventId`.
- [ ] **Doc + `static_assert`** de non‑auto‑référence pour `Event` members ; ou, `qb_relocatable_tag`.
- [ ] **`ServiceIndex` via magic static + `std::atomic<ServiceId>`**.
- [ ] Ajouter tests de régression : collisions d'EventId (binaire synthétique), relocation d'events avec `std::string` (doit trap).

### Phase 2 — Performance (3–7 jours)
- [x] **Flat callback vector** (2.6).
- [x] **Dispatch par fonction‑pointeur** dans `router::semh<_,void>` (2.7).
- [x] **Bitset (`ServiceIdPool`) pour `_ids`** (2.14).
- [x] **`spawn_async` eager counter** (2.12).
- [x] **`qb::allocate_actor<T>` point d'extension PMR/pool** (2.13).
- [ ] Bench avant/après : throughput push/send, latence 99p, CPU idle.

### Phase 3 — Robustesse & API (1–2 semaines)
- [ ] **Deadlock recovery redesign** (2.4) : backoff + doc + test de stress multi‑core saturé.
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

