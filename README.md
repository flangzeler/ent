
 
-A high-performance **header-only Archetype ECS** written in modern C++20.
`ent` is a high-performance, header-only **archetype ECS** for C++20 focused on game engines and real-time simulation workloads.
 
-Designed for game engine development and real-time simulation systems.
- Single-header include (`include/ent/ent.h`)
- SoA chunk storage for cache-friendly iteration
- Runtime component registry + compile-time typed APIs
- Query, cached query, command buffer, observers, singletons, systems, serialization, and debugging tools
 
 ---
 
-## Features
## Table of Contents
 
-- Archetype + Chunk-based SoA storage
-- O(1) entity lookup system
-- Component registry (runtime type-safe)
-- Fast query system
-- Entity migration (add/remove components)
-- Cache-friendly memory layout
-- Header-only (no dependencies)
1. [Quick Start](#quick-start)
2. [Core Concepts](#core-concepts)
3. [Setup and Configuration](#setup-and-configuration)
4. [API Walkthrough](#api-walkthrough)
5. [Query and Iteration Patterns](#query-and-iteration-patterns)
6. [Structural Changes and CommandBuffer](#structural-changes-and-commandbuffer)
7. [Observers (OnAdd / OnRemove)](#observers-onadd--onremove)
8. [Singleton Components](#singleton-components)
9. [Serialization](#serialization)
10. [System Scheduler](#system-scheduler)
11. [EntityHandle and WorldBuilder](#entityhandle-and-worldbuilder)
12. [Debugging, Inspection, and Profiling](#debugging-inspection-and-profiling)
13. [Performance Notes and Best Practices](#performance-notes-and-best-practices)
14. [Complete Example](#complete-example)
 
 ---
 
-## Example Usage
+## Quick Start
 
 ```cpp

struct Position { float x, y, z; };
struct Velocity { float x, y, z; };
 

int main() {
    ent::World world;
 

    ent::RegisterComponent<Position>();
    ent::RegisterComponent<Velocity>();
 

    auto e = world.CreateEntity();
    world.AddComponent<Position>(e, {0, 0, 0});
    world.AddComponent<Velocity>(e, {1, 0, 0});
 

    world.Query<Position, Velocity>().ForEach<Position, Velocity>([](Position& p, const Velocity& v) {
        p.x += v.x;
        p.y += v.y;
        p.z += v.z;
    });
}
```

---

## Core Concepts

### 1) Entity
An `Entity` is a numeric ID (`uint32_t`) that points to component data in archetype chunks.

### 2) Component
A component is any C++ type you register via `RegisterComponent<T>()`. Registered components get a runtime `ComponentID` (bit index in signature mask).

### 3) Archetype
An archetype represents one exact component set/signature. Entities migrate between archetypes when adding/removing components.

### 4) Chunk SoA Storage
Each archetype stores entities in fixed-size chunks (`ENT_CHUNK_SIZE`, default 16KB). Data is laid out as:
- entity ID array
- aligned component arrays (one contiguous array per component)

This gives high cache locality for tight loops.

### 5) Queries
`World::Query<Ts...>()` selects all archetypes containing `Ts...`, then iterates all matching chunk rows.

---

## Setup and Configuration

### Include
- Main header: `include/ent/ent.h`

### Compile
- Requires **C++20**.
- No external dependencies.

### Optional compile-time config macros
Define before including `ent.h`:

- `ENT_CHUNK_SIZE` (default `16384`)
- `ENT_MAX_COMPONENTS` (default `64`)
- `ENT_INITIAL_ENTITY_CAPACITY` (default `4096`)
- `ENT_PARALLEL_MIN_CHUNKS` (default `2`)

### Optional profiling hooks
Define before include:

```cpp
#define ENT_PROFILE_BEGIN(name) MyProfilerBegin(name)
#define ENT_PROFILE_END(name)   MyProfilerEnd(name)
#include "ent/ent.h"
```

---

## API Walkthrough

## Component Registry

 `RegisterComponent<T>()` — register at startup.
`GetComponentID<T>()` — get runtime ID.

 Important: register components before gameplay/world mutation.

## World lifecycle

- `Entity CreateEntity()`
- `void DestroyEntity(Entity e)`
- `bool IsAlive(Entity e)`

## Component access

- `T& AddComponent<T>(Entity e, T value = {})`
- `void RemoveComponent<T>(Entity e)`
- `bool HasComponent<T>(Entity e)`
- `T& GetComponent<T>(Entity e)` / const overload

## Clone

- `Entity CloneEntity(Entity source)` — deep copy all source components.

## Stats and utility

- `WorldStats GetStats()`
- `void PrintStats()`
- `void PrintMemoryStats()`
- `void InspectEntity(Entity e)`
- `size_t ArchetypeCount()`
- `uint32_t LiveEntityCount()`

---

## Query and Iteration Patterns

`QueryResult` supports several iteration styles.

### Basic per-entity iteration

```cpp
world.Query<Position, Velocity>()
    .ForEach<Position, Velocity>([](Position& p, const Velocity& v) {
        p.x += v.x;
     });
```

### Iterate with entity IDs

```cpp
world.Query<Position>()
     .ForEachWithEntity<Position>([](ent::Entity e, Position& p) {
         (void)e;
         p.x += 1.0f;
     });
```

### Exclusion queries

```cpp
const ent::Signature excluded = ent::SignatureBit(ent::GetComponentID<Disabled>());
world.QueryExcluding<Position, Velocity>(excluded)
     .ForEach<Position, Velocity>([](Position& p, Velocity& v){ p.x += v.x; });
```

### Chunk-level iteration

- `ForEachChunk(...)`
- `ParallelForEachChunk(...)`
- `ParallelForEach<Ts...>(...)`

Use these for heavy workloads and better thread utilization.

### CachedQuery

`CachedQuery<Ts...>` caches matching archetypes and auto-refreshes when world structure changes (tracked by world version).

```cpp
ent::CachedQuery<Position, Velocity> q(world);
q.ForEach([](Position& p, Velocity& v) { p.x += v.x; });
```

---

## Structural Changes and CommandBuffer

Use `CommandBuffer` when iterating and you need deferred create/add/remove/destroy without invalidating in-flight iteration.

Typical pattern:
1. Iterate and queue structural ops in `CommandBuffer`.
2. Call `world.FlushCommands(cmd)` after iteration.

High-level operations include:
- `CreateEntity()` (phantom ID until flush)
- `DestroyEntity(e)`
- `AddComponent<T>(e, value)`
- `RemoveComponent<T>(e)`

---

## Observers (OnAdd / OnRemove)

+Register lifecycle hooks by component type:

```cpp
world.OnAdd<Health>([](ent::World& w, ent::Entity e) {
    auto& h = w.GetComponent<Health>(e);
    if (h.current <= 0) h.current = h.max;
 });

world.OnRemove<Health>([](ent::World&, ent::Entity e) {
    // called before component storage is removed/destroyed
    (void)e;
});
```

Semantics:
- `OnAdd<T>`: fires after successful add.
- `OnRemove<T>`: fires before removal/destroy (while still present).

---

## Singleton Components

World-level shared objects keyed by component ID:

- `SetSingleton<T>(value)`
- `GetSingleton<T>()`
- `HasSingleton<T>()`
- `RemoveSingleton<T>()`

Great for global configs, service pointers, timers, frame context, etc.

---

## Serialization

Binary save/load APIs:

- `bool Save(const std::string& path) const`
- `bool Load(const std::string& path)`

Format includes:
- file magic/version
- component registry snapshot (name+size)
- entity pool state
- archetypes, entities, and component bytes

Notes:
- Register compatible component types before loading.
- Components matched by `(name, size)` when loading.
- Unmatched stored components are skipped.

---

## System Scheduler

`SystemScheduler` executes systems in priority order.

System concept:

```cpp
struct MySystem {
    void Update(ent::World& world, float dt);
};
```

APIs:
- `Register(system, name, priority)`
- `RegisterFn(name, fn, priority)`
- `SetEnabled(name, bool)`
- `Update(world, dt)`
- `PrintOrder()`

Built-in sample systems in header:
- `MovementSystem`
- `HealthSystem`
- `LifetimeSystem`
- `PendingKillSystem`

---

## EntityHandle and WorldBuilder

## EntityHandle
RAII helper wrapping entity ownership.

```cpp
auto h = ent::EntityHandle::Create(world)
            .Add<Position>({1,2,3})
            .Add<Velocity>({0,1,0});

ent::Entity id = h.ID();
h.Release(); // prevent auto-destroy on handle dtor
```

If not released, owned entity is automatically destroyed when handle is destroyed.

## WorldBuilder
Fluent startup helper:

```cpp
ent::MovementSystem move;
ent::HealthSystem health;

auto built = ent::WorldBuilder{}
    .RegisterComponents<ent::Position, ent::Velocity, ent::Health>()
    .AddSystem(move, "Movement", 100)
    .AddSystem(health, "Health", 300)
    .Build();
```

Returns unique pointers to world and scheduler.

---

## Debugging, Inspection, and Profiling

- `InspectEntity(e)` prints archetype, location, and components.
- `PrintStats()` and `PrintMemoryStats()` dump world + archetype memory stats.
- `WorldStats` provides machine-readable counters.
- profiling scopes wrap important operations via `ENT_PROFILE_BEGIN/END`.

---

## Performance Notes and Best Practices

1. **Register all components early**; avoid runtime registration mid-frame.
2. **Prefer tight component sets** for hot loops to minimize bytes/entity.
3. **Batch structural changes** via `CommandBuffer`.
4. **Reuse CachedQuery** inside systems.
5. **Use chunk/parallel iteration** only for meaningful workloads.
6. **Minimize false sharing** by per-thread chunk partitioning patterns.
7. **Tune** `ENT_CHUNK_SIZE` and `ENT_PARALLEL_MIN_CHUNKS` for your target.

---

## Complete Example

See: `examples/basic_example.cpp`

Build example (adjust include path as needed):

```bash
g++ -std=c++20 -O2 -Iinclude examples/basic_example.cpp -o basic_example
./basic_example
```

---
+
+## License
+
+MIT. See `LICENSE`.
