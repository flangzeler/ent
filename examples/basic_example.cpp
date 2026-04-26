//basic_example.cpp | ent lib Release:1.0.1
//this code is provided under MIT license.

#include <iostream>
#include "ecs.h"

struct Position {
    float x, y, z;
};

struct Velocity {
    float x, y, z;
};

struct Health {
    int hp;
};

int main() {
    ent::World world;

    // Register components (important step in this ECS design)
    ent::RegisterComponent<Position>();
    ent::RegisterComponent<Velocity>();
    ent::RegisterComponent<Health>();

    // Create entities
    auto e1 = world.CreateEntity();
    auto e2 = world.CreateEntity();

    // Add components
    world.AddComponent<Position>(e1, {0, 0, 0});
    world.AddComponent<Velocity>(e1, {1, 0, 0});

    world.AddComponent<Position>(e2, {10, 5, 0});
    world.AddComponent<Health>(e2, {100});

    // Query system
    auto view = world.Query<Position, Velocity>();

    view.ForEach([](Position& p, Velocity& v) {
        p.x += v.x;
        p.y += v.y;
        p.z += v.z;

        std::cout << "Entity moving: "
                  << p.x << ", "
                  << p.y << ", "
                  << p.z << "\n";
    });

    // Debug stats
    world.PrintStats();

    return 0;
}
