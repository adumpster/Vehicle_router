# Models Documentation

The repository contains **three standalone solver variants**. Each is a complete,
independent CMake project with its own `src/`, `include/`, `testcases/`,
`results/` and `CMakeLists.txt`. They share ~95% of their source; the differences
are deliberate and isolated so you can measure how much the ride-sharing
constraints cost in objective value.

```
optimization-Hybrid_final/      ← recommended, final refined model
optimization-All_constarint/    ← strict sharing caps (note: folder spelling)
optimization-No_Constraint/     ← relaxed baseline (sharing ignored)
```

All three run identically from the command line:

```bash
./build/velora ./testcases/TC04.json ./results/TC04_output.json
```

---

## 1. The single point of difference: `get_global_share_limit`

Every variant defines the same inline function in `include/config.h`. It maps an
employee's **sharing preference** to the **maximum number of passengers** allowed
on a route that carries that employee. This one function is the design fork.

### Hybrid (final)

```cpp
// HYBRID: Only Single is strict
int get_global_share_limit(SharingPref p) {
    switch (p) {
        case SINGLE: return 1;     // must ride alone
        case DOUBLE: return 999;   // willing to share with anyone
        case TRIPLE: return 999;   // willing to share with anyone
        default:     return 999;   // ANY_SHARE
    }
}
```

Interpretation: a `single` employee is guaranteed a private ride. `double` and
`triple` are treated as "I'm happy to share" — they impose **no numeric seat
cap** beyond the vehicle's own capacity. This maximises pooling (and thus
savings) while still honouring the one preference riders care about most.

### All-constraints

```cpp
// Strict for All-Constraints
int get_global_share_limit(SharingPref p) {
    switch (p) {
        case SINGLE: return 1;
        case DOUBLE: return 2;     // at most 1 co-rider
        case TRIPLE: return 3;     // at most 2 co-riders
        default:     return 999;
    }
}
```

Interpretation: the literal reading of the preference. A `double` employee may
share with at most one other person; a `triple` with at most two. This is the
most restrictive model and generally yields the highest cost.

### No-constraint

```cpp
// No Constraint: Everything is relaxed
int get_global_share_limit(SharingPref /*p*/) {
    return 999; // Explicitly relaxed for the unconstrained variant
}
```

Interpretation: sharing preference is ignored entirely. The only limit on a
route is the vehicle's physical `capacity`. This is the theoretical lower bound
on cost (loosest constraints) and is used as a comparison baseline.

> Because this cap flows through **every** module —
> `heuristic.cpp`, `alns.cpp` (`simulate_route`, `best_insert`,
> `recompute_max_capacity`), `infeasible_handler.cpp`, `dynamic_handler.cpp` —
> changing this one function consistently changes the entire solver's behaviour.

## 2. Secondary differences

### 2a. `check_compatibility` in `heuristic.cpp`

- **Hybrid & All:** only a `single` passenger shrinks a route's effective
  capacity to 1; the compatibility check consults `route.max_capacity` and the
  per-employee limit. (All-constraints additionally defines a
  `sharing_cap_val()` helper and a service time of 2 min — see below.)
- **No-constraint:** `check_compatibility` reduces to a pure vehicle-capacity
  check: `route.current_capacity + 1 > (int)v.capacity`. Sharing and even the
  premium/normal category interplay are stripped out of the shrink logic.

### 2b. Service time per pickup (`SERVICE_MIN`)

- **Hybrid** and **No-constraint:** `SERVICE_MIN = 0` — no dwell time is added
  at a pickup; a passenger's `departure_time = begin_service`.
- **All-constraints:** the constructive heuristic uses a local
  `SERVICE_PICKUP_MIN = 2` (2 minutes of boarding time per pickup) in
  `simulate_insertion_and_check`. `config.cpp` still defines the global
  `SERVICE_MIN = 0`, so other modules use 0; only the All-constraints
  constructive step charges the 2-minute dwell.

### 2c. Vehicle-category strictness

The Hybrid and All-constraints ALNS/handlers enforce **both** directions of the
category rule:

```cpp
if (e.veh_pref == PREMIUM && v.category != PREMIUM) return false; // wants premium → premium only
if (e.veh_pref == NORMAL  && v.category == PREMIUM) return false; // wants normal → no premium
```

The No-constraint variant removes the category checks from its `vehicle_ok` /
compatibility paths (consistent with its "relax everything" philosophy).

### 2d. ALNS restart count (`n_runs`)

Defined in each variant's `src/main.cpp`:

| Variant | `make_config_instant().n_runs` | `make_config_quality().n_runs` |
|---------|-------------------------------|-------------------------------|
| Hybrid | 4 | 4 |
| All-constraints | 4 | 4 |
| No-constraint | **8** | **10** |

The No-constraint model does more independent restarts (its search space is
larger/flatter because there are fewer constraints pruning it).

### 2e. `main.cpp` structural nit

Hybrid's `main.cpp` wraps stage 3 in an extra `if(true){ … }` block (lines
137/148). This is cosmetic and does not change behaviour versus the other two.

## 3. Which model should I use?

- **Use `optimization-Hybrid_final`** for the real deliverable. It is the
  "final, refined" model: it honours the preference riders care about most
  (`single` = private) while pooling aggressively everywhere else, giving the
  best realistic cost.
- **Use `optimization-All_constarint`** when you must honour `double`/`triple`
  literally (e.g. a stricter customer contract) or to report the "all rules
  strictly on" cost.
- **Use `optimization-No_Constraint`** to establish the lower-bound reference
  cost and to quantify how much the sharing rules cost you.

## 4. Choosing solver *depth* at runtime (all variants)

Independently of which variant you build, each `velora` binary supports two ALNS
effort levels, selected by an `alns_depth` key in the input JSON's `config`
array (read in `main.cpp`):

- `alns_depth = 2` (**default**) → **INSTANT** mode (`make_config_instant`,
  ~350 iterations, ~3 s).
- `alns_depth = 1` → **QUALITY** mode (`make_config_quality`, ~6000 iterations,
  ~5 min).

```json
{ "config": [ { "key": "alns_depth", "value": 1 } ], ... }
```

See [ALGORITHMS.md](ALGORITHMS.md#3-stage-3--alns) for what these parameters do.
