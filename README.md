# Ent ECS

A high-performance **header-only Archetype ECS** written in modern C++20.

Designed for game engine development and real-time simulation systems.

---

## Features

- Archetype + Chunk-based SoA storage
- O(1) entity lookup system
- Component registry (runtime type-safe)
- Fast query system
- Entity migration (add/remove components)
- Cache-friendly memory layout
- Header-only (no dependencies)

---

## Example Usage

```cpp
ent::World world;

ent::RegisterComponent<Position>();
ent::RegisterComponent<Velocity>();

auto e = world.CreateEntity();

world.AddComponent<Position>(e, {0,0,0});
world.AddComponent<Velocity>(e, {1,0,0});

auto view = world.Query<Position, Velocity>();

view.ForEach([](Position& p, Velocity& v) {
    p.x += v.x;
});
