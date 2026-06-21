# QB‑Core — Actor Coroutine Scope : sécurité & ergonomie par construction

> Scope : `qb/include/qb/core/Actor.{h,tpp}`, `qb/source/core/src/{Actor,VirtualCore}.cpp`,
> `qb/include/qb/io/async/coroutine/**` (cancellation, combinators, sync, scope).
> Standard cible : C++23. Objectif : rendre les coroutines d'acteur **sûres et annulables
> par construction**, à **coût nul hors du chemin opt‑in**, sans toucher au scheduler optimisé.
> Date : 2026‑06‑20 — Auteur : design interne.
> Statut : **Couches 1 et 3 IMPLÉMENTÉES & validées** (ASan/UBSan + TSan verts, leak‑neutral vs baseline) ;
>          Couche 2 (combinateurs annulables) à faire.
> Connexe : `internal/plans/QB_COROUTINE_PLAN.md` (sous‑système coroutine), `readme/7_reference/core_invariants.md`.

## État d'implémentation (2026‑06‑20)

**Couche 1 — livrée.** `cancellation_token` gagne un état vide sans allocation (`null_token`,
`operator bool`, `cancel`/`is_cancelled`/`on_cancel`/`throw_if_cancelled` null‑safe ; `on_cancel`
const). `Actor` : membre lazy `_coro_scope`, `spawn`, `has_coro_scope`, helpers
`__resolve_coro_scheduler__` / `__ensure_coro_scope__` / `__cancel_coro_scope__` ; `spawn_detached`
refactoré (comportement identique). `ScopedCoroContext` : `sleep` / `until_cancelled` /
`cancellation_point` (yield+bail) / `cancellable` / `token` / `child_token`. `kill()` et
`VirtualCore::removeActor` annulent le scope (idempotent). Wrapper scopé qui avale `cancelled_error`.
Tests : `source/core/tests/system/test-actor-coroutine-scope.cpp` (10 cas) — **verts sous
`sanitize` (ASan+UBSan) et `sanitize-thread` (TSan)** ; régressions coroutine/acteur vertes ; fuites
ASan résiduelles confirmées **préexistantes** (Finding 2.B.8) par comparaison stash/baseline.

> Affinement appris du code : `check_cancelled` *parque jusqu'à annulation* (≠ poll). D'où deux ops
> distinctes — `cancellation_point()` = yield + bail (boucles), `until_cancelled()` = park jusqu'au kill
> (attente pure, sans timer détaché — réclamation exacte).

**Couche 3 (`ask`) — livrée.** `qb::AskEvent` (base à round-trip via `reply()`, champ `correlation_id`) ;
`ScopedCoroContext::ask<E>(target, req, timeout)` = **awaiter unique à 3 sources** (réponse / timeout
`ev_timer` / annulation de scope), **sans helper détaché** ; registre de corrélation `thread_local`
par worker (mono-thread, sans lock, slots dans la frame de l'awaiter) ; `Actor::resolve_ask<E>(E&)`
route les réponses depuis `on(E&)`. Tests : `test-actor-coroutine-ask.cpp` (11 cas — succès / timeout /
cancel-on-kill, variantes cross-core, corrélations concurrentes distinctes, réponse tardive post-timeout,
payload non-trivial) — **verts sous ASan/UBSan et TSan**, zéro fuite. Décision : type d'événement unique
(req == resp via `reply()`), corrélation préservée par `reply()` ; l'asker doit appeler `resolve_ask`
en tête de son `on(E&)` (contrat documenté ; un mixin `with_ask` reste un raffinement optionnel).

---

## 0. TL;DR — décisions de conception

Légende : ☐ à faire — ◇ décision figée — ✗ rejeté (avec preuve).

| #  | État | Sujet | Décision |
|----|------|-------|----------|
| D1 | ◇ | Mécanisme d'annulation | **Coopératif** (`cancellation_token` par acteur), **PAS** destruction forcée de frame. |
| D2 | ✗ | Destruction forcée « drop‑the‑future » | **Rejetée** : incomplète (helpers détachés des combinateurs s'échappent), change la sémantique testée « orphelin‑et‑termine », exige une chirurgie du scheduler. Voir §3. |
| D3 | ◇ | API | `spawn(fn)` **nouvelle** ; `spawn_detached` **inchangée** (zéro régression). Deux intentions distinctes. |
| D4 | ◇ | Contexte | `ScopedCoroContext` = sur‑ensemble de `CoroContext` (porte l'`ActorId` **et** le token de scope). |
| D5 | ◇ | Token | Membre `cancellation_token` **alloué paresseusement** au 1ᵉʳ `spawn` → coût nul sinon. |
| D6 | ◇ | Déclenchement | `kill()` ⇒ `token.cancel()` (idempotent, réveil <30 ms déjà testé) ; `~Actor` re‑signale par défense. |
| D7 | ◇ | Couche 2 | Rendre `when_all`/`when_any`/`coro_with_timeout` **propagateurs d'annulation** → annulation **totale**. |
| D8 | ◇ | `ask` | Awaiter **sur‑mesure inline à 3 sources** (réponse / timeout / cancel), **sans** `with_deadline`/`when_any`. |
| D9 | ◇ | Résidu `[this]` | Lint CI `spawn_*\(\s*\[[^\]]*(this|&)` — le token borne la durée de vie, pas la capture volontaire. |

---

## 1. État des lieux (preuves)

### 1.1 Ce qui existe et qu'on réutilise

- **`Actor::spawn_detached(fn)`** — `Actor.tpp:222‑259`. Revalide `coro_scheduler_` vs TLS (1 `cmp+jne`), incrémente `active_coroutines_`, construit `CoroContext`, `spawn(actor_coro_wrapper(...))`.
- **`detail::actor_coro_wrapper`** — `Actor.tpp:208‑218`. Prend `func` **par valeur** (frame), `Guard` RAII décrémente le compteur même si l'acteur est mort. `co_await func(ctx)`.
- **`CoroContext`** — `Actor.h:1110‑1154`. Ne porte que l'`ActorId` : `push`, `push_to`, `id`, `time`. Aucun `this`.
- **`active_coroutines_`** — `Actor.h:1097`, `shared_ptr<atomic<size_t>>` (survit à l'acteur).
- **`cancellation_token`** — `cancellation.h:78‑146`. `shared_ptr<{bool, vector<function>}>`. `cancel()` enfile les réveils (`schedule_via_current`), idempotent. Awaiters token‑aware : `cancellable_sleep` (`:519`), `make_cancellable` (`:447`), `check_cancelled` (`:205`), `with_deadline` (`:613`).
- **`coroutine_scope`** — `scope.h`. Nursery task‑local : `spawn`/`join_all`/`cancel_all`, `cleanup_policy{join_all, cancel_all(défaut), detach}`. Modèle conceptuel du scope.
- **Awaiters RAII‑propres** — `awaiter.h`. **Tout destructeur d'awaiter arrête le watcher libev ET `unregister_suspended`** : `timer_awaiter` `:342‑351`, `socket_awaiter` `:481‑487`, `async_awaiter` `:600‑605`. ⇒ détruire un frame parqué est propre.
- **`async_event`** — `sync.h`. `co_await ev.wait()` parque le handle ; `ev.set()` le ré‑enfile. **Pont événement→coroutine** idéal pour `ask`.
- **Scheduler appartient au listener** — `listener.h:226`. `listener::run()` → `_coro_scheduler->run_ready()` après libev (`:517‑528`). Le listener (thread‑local) est détruit **au sortie du thread**, donc **après** `__workflow__` ⇒ **le scheduler survit à tous les acteurs**.
- **`~listener` → `reset_coro_scheduler` → `destroy_all_suspended()`** — `listener.h:605‑613`. Le framework détruit **déjà** les frames suspendues en fin de vie (loop valide).

### 1.2 Le flow décisif — ordonnancement de `__workflow__`

`VirtualCore.cpp:372‑458`, une itération :

1. `listener::current.run(EVRUN_NOWAIT)` ⇒ **les coroutines reprennent ici** (`run_ready`).
2. `__flush_all__()` (sortant) ; `__receive__()` ⇒ `on(KillEvent)` ⇒ `kill()` ⇒ `_actor_to_remove.insert(id)`.
3. `removeActors` ⇒ `removeActor` ⇒ `~Actor` — **en fin d'itération, hors `run_ready()`**.

Conséquences exploitées :
- `kill()` et `~Actor` s'exécutent **scheduler au repos** (aucune frame coroutine sur la pile) ⇒ signaler/enfiler est sûr.
- `removeActor` **a déjà le hook** : `has_active_coroutines()` + `LOG_WARN` — `VirtualCore.cpp:497‑500`.
- Une coroutine réveillée par le token au temps N reprend au `listener.run` de **N+1**, *après* la mort de l'objet acteur — sûr car elle ne touche que des valeurs + `ctx` (l'`ActorId` route, le token `shared_ptr` survit).

### 1.3 La faiblesse à corriger

Le header le dit lui‑même (`Actor.h:1026‑1034`) : `spawn_detached([this]…)` **compile et est UB**. Le seul garde‑fou actuel est documentaire. Par ailleurs, une coroutine d'acteur tué **attend la fin de son IO indéfiniment** (orphelin) au lieu d'être annulée promptement.

---

## 2. Le design

### 2.1 D3/D4/D5 — `spawn` + `ScopedCoroContext`

```cpp
// Actor.h — membre additionnel, alloué paresseusement (null tant qu'inutilisé).
mutable qb::io::async::cancellation_token _coro_scope{ qb::io::async::null_token };

// ScopedCoroContext : sur-ensemble de CoroContext, porte aussi le token de scope.
class ScopedCoroContext : public CoroContext {
    qb::io::async::cancellation_token _scope;
public:
    [[nodiscard]] const cancellation_token& token() const noexcept { return _scope; }
    [[nodiscard]] cancellation_token child_token() const; // lié au scope (cancel parent ⇒ cancel enfant)

    // Ops token-aware : annulation gratuite, sans threader de token.
    auto sleep(qb::duration d) const     { return cancellable_sleep(d, _scope); }
    auto cancellation_point() const      { return check_cancelled(_scope); }
    template <typename T> auto cancellable(task<T>&& t) const { return make_cancellable(std::move(t), _scope); }
    // connect(uri,timeout) / read / write / ask<Resp>(...) : variantes token-aware (Couche 3 pour ask).
};

template <typename Func>
void Actor::spawn(Func&& func) const {
    ensure_coro_scope();                 // crée _coro_scope au 1er appel (1 shared_ptr)
    // réutilise STRICTEMENT le wrapper existant + le compteur RAII ; passe un ScopedCoroContext.
    ScopedCoroContext ctx(this, _coro_scope);
    active_coroutines_->fetch_add(1, std::memory_order_relaxed);
    coro_scheduler_->spawn(detail::actor_coro_wrapper(std::forward<Func>(func), ctx, active_coroutines_));
}
```

`spawn_detached` reste **identique** (chemin brut, détaché, « orphelin‑et‑termine »). `spawn` exprime l'intention « lié à l'acteur, annulé à sa mort ».

### 2.2 D6 — cancel‑on‑kill

```cpp
void Actor::kill() const noexcept {
    if (_coro_scope) _coro_scope.cancel();   // idempotent, enfile-seulement
    VirtualCore::_handler->killActor(_id);   // comportement existant
}
// ~Actor (ou removeActor) : if (_coro_scope) _coro_scope.cancel();  // défense pour les morts hors-kill
```

Une coroutine scopée parquée sur une op `ctx.*` est réveillée (<30 ms, cf. tests), lève `cancelled_error`, **se déroule par le chemin normal** (catch + RAII ; l'awaiter arrête son watcher en `await_resume`), le `Guard` décrémente le compteur. **Aucune destruction forcée. Aucun nettoyage de bookkeeping manuel.**

### 2.3 D7 — Couche 2 : combinateurs propagateurs d'annulation (le fix profond)

Aujourd'hui `when_all`/`when_any`/`race`/`coro_with_timeout` lancent leurs branches **détachées** (`combinators.h:114, 357, 557‑558`) ⇒ une branche perdante survit à l'annulation du parent. C'est le seul résidu réel du modèle coopératif. Faire que chaque combinateur **enregistre son `cancellation_token` et annule/arrête ses branches** lorsque le parent est annulé rend l'annulation **totale** — et bénéficie à **toute** la couche coroutine (scopes, timeouts, `ask`), pas seulement aux acteurs. À traiter sous sanitizers avec tests de propagation dédiés.

### 2.4 D8 — Couche 3 : `ask` natif

```cpp
template <event_type Resp, event_type Req>
task<Resp> ScopedCoroContext::ask(ActorId target, Req payload, qb::duration timeout) const;
```

Implémentation : **un seul awaiter inline à 3 sources** (pas `with_deadline`/`when_any` qui détachent) :
1. **réponse** — slot de corrélation (`uint64_t corr_id`) dans un registre **par acteur** ; `on(Resp&)` de l'acteur résout le slot et `schedule_via_current(handle)` (pattern `async_event`, `sync.h`).
2. **timeout** — `ev_timer` inline (arrêté en `await_resume`/dtor).
3. **cancel** — `token.on_cancel` (scope de l'acteur).
La première source gagne ; les autres sont arrêtées par RAII. **Le destructeur de l'awaiter désenregistre le slot de corrélation** ⇒ cancel‑on‑kill propre. Le plumbing `on(Resp&)`→registre est fourni par un mixin (`with_ask<Self>`), pour ne pas l'imposer à chaque acteur.

### 2.5 D9 — lint complémentaire

Le token borne la **durée de vie** mais pas un `[this]` capturé volontairement (la coroutine reprend après la mort pour lever ; toucher `this` dans un `catch` reste UB). Garde‑fou : lint CI `rg -n "spawn_(async|scoped)\s*\(\s*\[[^\]]*(this|&)"` + `ScopedCoroContext` rend `this` inutile. Sécurité totale = scope (durée de vie/annulation) + lint (`[this]`) + docs.

---

## 3. Pourquoi PAS la destruction forcée (D2, rejet argumenté)

J'ai conçu puis **rejeté** l'approche « drop‑the‑future » (détruire le frame racine de l'acteur à sa mort), bien qu'elle soit *techniquement propre* (les dtors d'awaiters arrêtent les watchers, §1.1 ; précédent `destroy_all_suspended`, `listener.h:609 ; task<T>::~task` cascade, `task.h:476`). Trois preuves la disqualifient :

1. **Incomplète.** Les combinateurs détachent leurs branches (`combinators.h:114/357/557`) ⇒ elles **s'échappent** d'un drop du frame racine et tournent jusqu'à leur fin. La « reclamation totale garantie » est donc fausse.
2. **Change une sémantique testée.** `ActorCleanupWithActiveCoroutines`, `ActorDiesWithPendingCoroutines`, `CoroutineOutlivesActor` assertent « orphelin‑et‑termine » (`EXPECT_FALSE(main.hasError())`). Le drop casserait ces contrats — et le cas d'usage *légitime* « travail de fond qui survit » disparaîtrait.
3. **Risque sur le cœur.** Le drop forcé exige un index de scope + destruction sélective **dans le scheduler ultra‑optimisé**. Le coopératif n'y touche **pas**.

Le coopératif **dégrade vers** ce comportement « orphelin‑et‑termine » déjà béni pour tout `co_await` non‑token‑aware : son pire cas = le statu quo sûr. Il gagne donc sur sécurité, alignement, non‑régression **et** performance.

---

## 4. Invariants à ajouter (`core_invariants.md`)

1. **`spawn` ⇒ annulation coopérative à `kill()`** : une coroutine n'awaitant que des ops `ScopedCoroContext` est annulée et déroulée dans la prochaine itération de boucle après `kill()`. Un `co_await` non‑token‑aware retombe sur « orphelin‑et‑termine » (sûr).
2. **`spawn_detached` reste détaché** : sa coroutine survit à l'acteur et se termine (inchangé).
3. **Jamais `[this]`** dans `spawn_detached`/`spawn` : capturer par valeur, dialoguer par `ctx`. Le scope borne la durée de vie ; il **n'autorise pas** l'accès à `this` après suspension.
4. **`token().cancel()` est idempotent** et n'est jamais appelé cross‑thread (mono‑thread : un acteur d'un autre core envoie un event, le handler annule sur son thread).

---

## 5. Matrice de tests (critères d'acceptation)

| Cas | Attendu |
|-----|---------|
| `spawn` + `ctx.sleep(10s)` puis `kill()` | Coroutine annulée < 1 itération ; `cancelled_error` ; `live_frames` revient à la base. |
| `spawn` + `ctx.connect(uri)` puis `kill()` pendant connect | Watcher arrêté, slot libéré, pas de fuite. |
| `spawn` + `ctx.ask(...)` puis `kill()` avant réponse | Slot de corrélation désenregistré ; réponse tardive no‑op ; pas de fuite. |
| `spawn_detached` (brut) + `kill()` | **Inchangé** : orphelin‑et‑termine, `EXPECT_FALSE(hasError())`. |
| `when_any(a, b)` scopé + `kill()` (après Couche 2) | **Les deux** branches annulées (test de propagation). |
| `cancel()` ×3 | Callbacks une seule fois (idempotence). |
| `cancellation_point()` après `kill()` | Lève `cancelled_error`. |
| Stress 50 `spawn` + `kill()` | Pas de crash, `live_frames` à la base, `active_coroutine_count()→0`. |
| Capture `[this]` dans `spawn` | Détectée par le lint CI. |

Tous sous **ASan/UBSan** ; les cas Couche 2 aussi sous **TSan**.

---

## 6. Bilan performance

| Chemin | Surcoût |
|--------|---------|
| Acteur sans coroutine | **0** (un `shared_ptr` null de plus). |
| `spawn_detached` existant | **0** (chemin et tests intacts). |
| `spawn` | 1 inc refcount (copie token) ; alloc token amortie 1×/acteur. |
| `co_await ctx.sleep/ask/...` | = awaiters cancellable existants. |
| `kill()` | O(awaiters scopés enregistrés), **uniquement au kill**. |
| Scheduler / boucle dispatch | **inchangés** — aucun lock, aucun atomic, aucune chirurgie. |

Optimisation ultérieure (optionnelle) : remplacer la `vector<std::function>` de `on_cancel` (`cancellation.h:84`) par une **liste intrusive** de nœuds‑awaiters → supprime les allocs heap sur le chemin d'annulation.

---

## 7. Résidus assumés

1. `co_await` non‑token‑aware (sleep/socket bruts) ⇒ orphelin‑et‑termine (sûr, existant). `ScopedCoroContext` + Couche 2 le réduisent à presque rien.
2. La Couche 2 touche du code éprouvé (combinateurs) ⇒ spec + tests de propagation dédiés, derrière sanitizers.
3. `ask` est un vrai chantier (registre de corrélation + awaiter 3‑sources + mixin) — pas une composition triviale.

---

## 8. Séquencement proposé

1. **L1** `spawn` + `ScopedCoroContext` + cancel‑on‑kill (faible risque, réutilise l'existant). + tests matrice §5 (hors Couche 2).
2. **L1.5** lint CI `[this]/[&]`.
3. **L2** combinateurs propagateurs d'annulation (sous sanitizers). Débloque l'annulation totale.
4. **L3** `ask` natif (mixin `with_ask<Self>` + awaiter 3‑sources).
5. Doc : `core_invariants.md` (§4) + « Coroutine Safety Cookbook ».
