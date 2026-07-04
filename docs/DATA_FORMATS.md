# Data Formats

This document specifies the **input** and **output** JSON schemas, field by
field, as parsed by `io.cpp` and emitted by `output_json.cpp`. A bundled
header-only parser (`mini_json.h`) handles all JSON — there are no external
dependencies.

---

## 1. Input JSON (test case)

Top-level keys: `employees` (object), `vehicles` (array), `baseline` (array,
optional), `metadata` (array, optional), `config` (array, optional).

### 1.1 `employees` — object keyed by employee id

```json
"employees": {
  "E01": {
    "priority": 1,
    "pickup": { "lat": 12.936, "lng": 77.625 },
    "drop":   { "lat": 12.9716, "lng": 77.5946 },
    "earliest_pickup": "08:15",
    "latest_drop": "09:15",
    "vehicle_preference": "premium",
    "sharing_preference": "single",
    "distances": { "drop": 7162.4, "E02": 1305.8, "E03": 3576.6 }
  }
}
```

| Field | Type | Meaning |
|-------|------|---------|
| `priority` | int | 1 = highest. Default `999` if missing. |
| `pickup` | `{lat,lng}` | Home pickup location. |
| `drop` | `{lat,lng}` | Office. Assumed identical for all employees; the **first** employee's drop sets the global `OFFICE`. |
| `earliest_pickup` | string `"HH:MM"` **or** number | Earliest pickup. A number is a **day-fraction** (`0.3333… = 08:00`). |
| `latest_drop` | string `"HH:MM"` **or** number | Office-arrival deadline. Same dual format. |
| `vehicle_preference` | `"premium"`/`"normal"`/`"any"` | Category requirement. Default `"any"`. |
| `sharing_preference` | `"single"`/`"double"`/`"triple"`/`"any"` | Ride-sharing preference. Default `"any"`. |
| `distances` | object (optional) | Real road distances in **metres** from this employee to others / `"drop"`. Used only if `allow_external_maps` is true. |

### 1.2 `vehicles` — array

```json
"vehicles": [
  {
    "vehicle_id": "V01",
    "fuel_type": "electric",
    "vehicle_type": "4W",
    "capacity": 3,
    "cost_per_km": 10,
    "avg_speed_kmph": 30,
    "current_lat": 12.935,
    "current_lng": 77.62,
    "available_from": 0.3333333333333333,
    "category": "premium"
  }
]
```

| Field | Type | Meaning |
|-------|------|---------|
| `vehicle_id` | string | Unique id. |
| `capacity` | number | Physical seats. |
| `cost_per_km` | number | Monetary cost coefficient. |
| `avg_speed_kmph` | number | Used to convert distance → time. Default 30. |
| `current_lat`/`current_lng` | number | Vehicle's trip-#1 start location. |
| `available_from` | string `"HH:MM"` or number | When the vehicle can start. Day-fraction supported. |
| `category` | `"premium"`/`"normal"`/`"any"` | Vehicle category. |
| `fuel_type`, `vehicle_type` | string | Informational; not used by the solver. |

### 1.3 `baseline` — array (optional)

Naive per-employee cost, used only for the **savings** figures in reports/output.

```json
"baseline": [ { "employee_id": "E01", "baseline_cost": 430, "baseline_time_min": 45 } ]
```

### 1.4 `metadata` — array of `{key,value}` (optional)

| Key | Effect |
|-----|--------|
| `allow_external_maps` | `true` → use the per-employee `distances` (metres) instead of haversine. |
| `objective_cost_weight` | Sets `W_COST` (default 0.6). |
| `objective_time_weight` | Sets `W_TIME` (default 0.4). |
| `priority_<k>_max_delay_min` | Per-priority delay budget (minutes) for Stage 2. |
| `test_case_id`, `city`, `distance_method` | Informational. |

### 1.5 `config` — array of `{key,value}` (optional)

| Key | Effect |
|-----|--------|
| `alns_depth` | `2` = INSTANT (default, ~3 s), `1` = QUALITY (~5 min). |

```json
"config": [ { "key": "alns_depth", "value": 1 } ]
```

---

## 2. Output JSON (solved plan)

Written by `write_output_json`. Structure:

```json
{
  "input": { ...verbatim echo of the input JSON... },
  "summary": {
    "total_employees": 12,
    "employees_routed": 12,
    "employees_unrouted": 0,
    "total_baseline_cost": 4945.0,
    "total_optimized_cost": 674.43,
    "net_savings": 4270.57,
    "savings_percentage": 86.36
  },
  "unrouted_employees": [
    { "employee_id": "E07", "reason": "..." }
  ],
  "vehicles": [
    {
      "vehicle_id": "V02",
      "category": "normal",
      "capacity": 4.0,
      "speed_kmh": 35.0,
      "cost_per_km": 14.0,
      "total_cost": 207.86,
      "trips": [
        {
          "trip_number": 1,
          "load": 4,
          "capacity_limit": 4,
          "start_time": "08:00",
          "end_time": "09:37",
          "trip_distance_km": 19.11,
          "trip_cost": 207.86,
          "route": ["START", "E09", "E02", "E07", "E04", "END"],
          "passengers": [
            { "employee_id": "E09", "pickup_time": "08:30", "drop_time": "09:37" },
            { "employee_id": "E02", "pickup_time": "08:34", "drop_time": "09:37" }
          ]
        }
      ]
    }
  ]
}
```

### Field reference

| Path | Meaning |
|------|---------|
| `input` | The original input JSON, echoed verbatim (lets a downstream consumer see both problem and solution in one file). |
| `summary.employees_routed / _unrouted` | How many of the total were placed. |
| `summary.total_optimized_cost` | Σ of all vehicles' `total_cost` (the objective). |
| `summary.net_savings` / `savings_percentage` | vs the summed `baseline_cost` of routed employees. |
| `unrouted_employees[].reason` | Text from `g_unrouted_reason` explaining why (usually empty when Stage 2 succeeds). |
| `vehicles[].total_cost` | Sum of that vehicle's trip costs. |
| `trips[].load` | Passengers on the trip (`current_capacity`). |
| `trips[].capacity_limit` | Effective cap applied (`max_capacity`; 1 if a `single` rider is on board). |
| `trips[].start_time` / `end_time` | Trip start (departure from START) and office arrival. |
| `trips[].route` | Ordered node list, `START`/`END` plus employee ids. |
| `trips[].passengers[]` | Per-employee `pickup_time` (departure at their pickup) and shared `drop_time` (office arrival). |

> **Note on formatting:** committed `results/*.json` files may contain extra blank
> lines inside the echoed `input` block (a cosmetic artefact of how the raw input
> is embedded). The solved portion (`summary`, `vehicles`, …) is standard JSON.

---

## 3. New-employees JSON (for `dynamic_insert`)

Consumed by the dynamic tool. Top-level key `new_employees`, an object keyed by
id, same per-employee schema as input `employees` (times as `"HH:MM"` strings),
optionally with a `baseline_cost`:

```json
{
  "new_employees": {
    "E13": {
      "priority": 2,
      "pickup": { "lat": 12.93, "lng": 77.62 },
      "drop":   { "lat": 12.9716, "lng": 77.5946 },
      "earliest_pickup": "08:20",
      "latest_drop": "09:30",
      "vehicle_preference": "any",
      "sharing_preference": "double",
      "baseline_cost": 400
    }
  }
}
```

See [DYNAMIC_INSERTION.md](DYNAMIC_INSERTION.md).
