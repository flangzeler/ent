// ENT
// ================================================================
//  ent.h — ECS library  By FLANGZELER  |Release 1.0.1--INITIAL-RELEASE
//  Standards: C++20  | Date: 04-26-2026
//  
//  LICENSE: This code is released under the MIT License. See LICENSE file for details.
// 
//
//  BASIC_FEATURES:
//  ─────────────────────────────────────────────────────────────
//  .1  CommandBuffer          Deferred structural mutation (safe during iteration)
//  .2  System Framework       Type-safe, cache-friendly system scheduling
//  .3  Versioned CachedQuery  Auto-refresh on archetype creation, zero overhead
//  .4  Parallel Iteration     Chunk-level std::thread parallelism, no false sharing
//  .5  Memory Hardening       Alignment guarantees, padding strategies, asserts
//  .6  Serialization          Binary world save/load, no external libs
//  .7  Debug & Profiling      Entity inspector, archetype dump, memory stats
//  .8  Observer System        OnAdd<T>/OnRemove<T> component lifecycle hooks
//  .9  EntityHandle           RAII owning wrapper around raw Entity
//  .10 Singleton Components   World-level shared-state components
//  .11 Entity Cloning         Full deep-copy of entity with all components
//  .12 WorldBuilder           Fluent startup API
//  .13 Profiling Hooks        User-injectable profile markers
// ================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <array>
#include <vector>
#include <algorithm>
#include <bit>
#include <span>
#include <functional>
#include <memory>
#include <typeindex>
#include <typeinfo>
#include <type_traits>
#include <limits>
#include <string_view>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <fstream>
#include <unordered_map>
#include <optional>

// ================================================================
//  PROFILING HOOKS
//  Define before including this header to plug in your profiler.
//  e.g. #define ENT_PROFILE_BEGIN(name) OPTICK_EVENT(name)
// ================================================================
#ifndef ENT_PROFILE_BEGIN
    #define ENT_PROFILE_BEGIN(name) ((void)0)
#endif
#ifndef ENT_PROFILE_END
    #define ENT_PROFILE_END(name)   ((void)0)
#endif

// ================================================================
//  CONFIG
// ================================================================

#ifndef ENT_CHUNK_SIZE
    #define ENT_CHUNK_SIZE  16384           // 16 KB per chunk
#endif
#ifndef ENT_MAX_COMPONENTS
    #define ENT_MAX_COMPONENTS 64
#endif
#ifndef ENT_INITIAL_ENTITY_CAPACITY
    #define ENT_INITIAL_ENTITY_CAPACITY 4096
#endif
#ifndef ENT_PARALLEL_MIN_CHUNKS
    #define ENT_PARALLEL_MIN_CHUNKS 2       // minimum chunks before spawning threads
#endif

#ifdef NDEBUG
    #define ENT_ASSERT(x)       ((void)0)
    #define ENT_ASSERT_MSG(x,m) ((void)0)
#else
    #define ENT_ASSERT(x)       assert(x)
    #define ENT_ASSERT_MSG(x,m) assert((x) && (m))
#endif

namespace ent
{

// ================================================================
//  FORWARD DECLARATIONS
// ================================================================

class  World;
class  Archetype;
struct Chunk;
struct ChunkView;
class  CommandBuffer;

// ================================================================
//  TYPES
// ================================================================

using Entity        = uint32_t;
using ComponentID   = uint16_t;
using ArchetypeID   = uint32_t;

static constexpr Entity      INVALID_ENTITY    = std::numeric_limits<Entity>::max();
static constexpr ComponentID INVALID_COMPONENT = std::numeric_limits<ComponentID>::max();
static constexpr ArchetypeID INVALID_ARCHETYPE = std::numeric_limits<ArchetypeID>::max();

// ================================================================
//  COMPONENT SIGNATURE
//  Bitmask over registered component IDs (up to 64).
// ================================================================

using Signature = uint64_t;

[[nodiscard]] inline constexpr Signature SignatureBit(ComponentID id) noexcept
{
    ENT_ASSERT(id < ENT_MAX_COMPONENTS);
    return Signature{1} << id;
}

// ================================================================
//  COMPONENT TYPE REGISTRY
//  Thread-unsafe: register all components at startup before any
//  World operations. Never call Register() during gameplay.
// ================================================================

struct ComponentMeta 
{
    ComponentID      id        = INVALID_COMPONENT;
    size_t           size      = 0;
    size_t           alignment = 0;
    std::string_view name;
    bool             triviallySerializable = false; // true if trivially copyable

    // Lifecycle hooks (null if trivial)
    void (*construct)(void* dest, size_t count)              = nullptr;
    void (*destruct) (void* src,  size_t count)              = nullptr;
    void (*move_ctor)(void* dest, void* src, size_t count)   = nullptr;
    void (*copy_ctor)(void* dest, const void* src, size_t count) = nullptr;
};

class ComponentRegistry 
{
public:
    static ComponentRegistry& Get() noexcept
    {
        static ComponentRegistry instance;
        return instance;
    }

    template<typename T>
    [[nodiscard]] ComponentID Register() 
    {
        const auto key = std::type_index(typeid(T));
        for (uint16_t i = 0; i < static_cast<uint16_t>(mMetas.size()); ++i)
            if (mTypeIndex[i] == key) return static_cast<ComponentID>(i);

        ENT_ASSERT_MSG(mMetas.size() < ENT_MAX_COMPONENTS,
            "Too many component types — increase ENT_MAX_COMPONENTS");

        ComponentID id = static_cast<ComponentID>(mMetas.size());
        ComponentMeta meta{};
        meta.id        = id;
        meta.size      = sizeof(T) > 0 ? sizeof(T) : 1; // guard zero-size types
        meta.alignment = alignof(T);
        meta.name      = typeid(T).name();
        meta.triviallySerializable = std::is_trivially_copyable_v<T>;

        if constexpr (!std::is_trivially_constructible_v<T>) 
        {
            meta.construct = [](void* dest, size_t count) 
                {
                T* ptr = static_cast<T*>(dest);
                for (size_t i = 0; i < count; ++i) ::new(ptr + i) T{};
            };
        }
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            meta.destruct = [](void* src, size_t count) 
                {
                T* ptr = static_cast<T*>(src);
                for (size_t i = 0; i < count; ++i) ptr[i].~T();
            };
        }
        if constexpr (!std::is_trivially_move_constructible_v<T>)
        {
            meta.move_ctor = [](void* dest, void* src, size_t count)
                {
                T* d = static_cast<T*>(dest);
                T* s = static_cast<T*>(src);
                for (size_t i = 0; i < count; ++i) ::new(d + i) T{std::move(s[i])};
            };
        }
        if constexpr (!std::is_trivially_copy_constructible_v<T>) 
        {
            meta.copy_ctor = [](void* dest, const void* src, size_t count) 
                {
                T* d       = static_cast<T*>(dest);
                const T* s = static_cast<const T*>(src);
                for (size_t i = 0; i < count; ++i) ::new(d + i) T{s[i]};
            };
        }

        mMetas.push_back(meta);
        mTypeIndex.push_back(key);
        return id;
    }

    template<typename T>
    [[nodiscard]] ComponentID GetID() const 
    {
        const auto key = std::type_index(typeid(T));
        for (uint16_t i = 0; i < static_cast<uint16_t>(mTypeIndex.size()); ++i)
            if (mTypeIndex[i] == key) return static_cast<ComponentID>(i);
        ENT_ASSERT_MSG(false, "Component not registered — call RegisterComponent<T>() first");
        return INVALID_COMPONENT;
    }

    [[nodiscard]] const ComponentMeta& GetMeta(ComponentID id) const noexcept 
    {
        ENT_ASSERT(id < mMetas.size());
        return mMetas[id];
    }

    [[nodiscard]] size_t Count() const noexcept { return mMetas.size(); }

private:
    ComponentRegistry() = default;
    std::vector<ComponentMeta>   mMetas;
    std::vector<std::type_index> mTypeIndex;
};

// ================================================================
//  COMPONENT REGISTRATION HELPERS
// ================================================================

template<typename T>
[[nodiscard]] inline ComponentID RegisterComponent()
{
    return ComponentRegistry::Get().Register<T>();
}

template<typename T>
[[nodiscard]] inline ComponentID GetComponentID() 
{
    return ComponentRegistry::Get().GetID<T>();
}

// ================================================================
//  Describes where each component array sits inside a Chunk.
//  Built once per archetype — immutable thereafter.
//
//  Memory layout per chunk (SoA):
//  ┌──────────────────────────────────────────────────┐
//  │  Entity IDs  [capacity × 4 bytes]                │  ← hot: swap-remove touches this
//  │  [align pad]                                      │
//  │  Component0  [capacity × sizeof(C0)]             │  ← SoA: C0 for all entities
//  │  [align pad]                                      │
//  │  Component1  [capacity × sizeof(C1)]             │
//  │  ...                                              │
//  └──────────────────────────────────────────────────┘
// ================================================================

struct ComponentSlot
{
    ComponentID id     = INVALID_COMPONENT;
    uint16_t    offset = 0;     // byte offset inside chunk data block
};

struct ChunkLayout 
{
    std::array<ComponentSlot, ENT_MAX_COMPONENTS> slots{};
    uint16_t slotCount = 0;
    uint16_t capacity  = 0;     // max entities per chunk

    [[nodiscard]] int32_t OffsetOf(ComponentID id) const noexcept 
    {
        for (uint16_t i = 0; i < slotCount; ++i)
            if (slots[i].id == id) return static_cast<int32_t>(slots[i].offset);
        return -1;
    }
};

[[nodiscard]] inline ChunkLayout BuildChunkLayout(
    const ComponentID* ids,
    uint16_t           count,
    size_t             chunkDataSize
) 
{
    ChunkLayout layout{};
    layout.slotCount = count;

    const auto& reg = ComponentRegistry::Get();

    // Sort by alignment descending — eliminates padding between arrays
    std::array<uint16_t, ENT_MAX_COMPONENTS> order{};
    for (uint16_t i = 0; i < count; ++i) order[i] = i;
    std::sort(order.begin(), order.begin() + count, [&](uint16_t a, uint16_t b) {
        return reg.GetMeta(ids[a]).alignment > reg.GetMeta(ids[b]).alignment;
    });

    // Capacity = how many entities fit given Entity array + all component arrays
    size_t bytesPerEntity = sizeof(Entity);
    for (uint16_t i = 0; i < count; ++i)
        bytesPerEntity += reg.GetMeta(ids[i]).size;

    uint16_t cap = static_cast<uint16_t>(chunkDataSize / bytesPerEntity);
    if (cap == 0) cap = 1;
    layout.capacity = cap;

    // Assign offsets: [Entity * cap | pad | C0 * cap | pad | C1 * cap | ...]
    size_t cursor = sizeof(Entity) * cap;

    for (uint16_t oi = 0; oi < count; ++oi)
    {
        uint16_t i        = order[oi];
        const auto& meta  = reg.GetMeta(ids[i]);

        // Natural alignment
        size_t aligned = (cursor + meta.alignment - 1) & ~(meta.alignment - 1);

        layout.slots[i].id     = ids[i];
        layout.slots[i].offset = static_cast<uint16_t>(aligned);

        cursor = aligned + meta.size * cap;
    }

    return layout;
}

// ================================================================
//  Fixed-size (16 KB) block owning SoA component arrays.
//
//  Why 16 KB?  Fits comfortably in L1/L2 cache (typically 32–256 KB).
//  Iterating a full chunk touches only contiguous memory — no pointer
//  chasing, no TLB thrashing.
//
//  alignas(64) ensures the chunk starts on a cache-line boundary,
//  preventing false sharing between the Chunk header and any
//  adjacent allocation. The data block is aligned to 16 to guarantee
//  SSE/NEON alignment for component arrays.
// ================================================================

struct alignas(64) Chunk
{
    // Reserve 64 bytes for the header — keeps it on its own cache line.
    // Iteration code touches data[], not the header, so these never share.
    static constexpr size_t HEADER_SIZE = 64;
    static constexpr size_t DATA_SIZE   = ENT_CHUNK_SIZE - HEADER_SIZE;

    // ── Header (one cache line) ──────────────────────────────────
    uint16_t count       = 0;   // live entity count
    uint16_t capacity    = 0;   // max entities (derived from layout)
    uint32_t archetypeIdx = 0;  // owning archetype ID
    uint32_t chunkIdx    = 0;   // index within archetype chunk list
    uint8_t  _pad[HEADER_SIZE - 4*sizeof(uint32_t) - 2*sizeof(uint16_t)] = {};

    // ── Data block (aligned for SIMD) ───────────────────────────
    alignas(16) std::byte data[DATA_SIZE];

    [[nodiscard]] Entity* Entities() noexcept
    {
        return reinterpret_cast<Entity*>(data);
    }
    [[nodiscard]] const Entity* Entities() const noexcept 
    {
        return reinterpret_cast<const Entity*>(data);
    }
    [[nodiscard]] void* ComponentArray(int32_t byteOffset) noexcept
    {
        ENT_ASSERT(byteOffset >= 0);
        return data + byteOffset;
    }
    [[nodiscard]] const void* ComponentArray(int32_t byteOffset) const noexcept
    {
        ENT_ASSERT(byteOffset >= 0);
        return data + byteOffset;
    }

    [[nodiscard]] bool IsFull()  const noexcept { return count >= capacity; }
    [[nodiscard]] bool IsEmpty() const noexcept { return count == 0; }
};

// Layout correctness guarantees
static_assert(sizeof(Chunk) <= ENT_CHUNK_SIZE + 64,
    "Chunk header overflow — reduce header fields or raise ENT_CHUNK_SIZE");
static_assert(alignof(Chunk) == 64,
    "Chunk must be 64-byte aligned to prevent false sharing");
static_assert(offsetof(Chunk, data) % 16 == 0,
    "Chunk::data must be 16-byte aligned for SIMD safety");

// ================================================================
//  CHUNK VIEW
//  Passed to user lambdas during iteration — thin, copyable.
// ================================================================

struct ChunkView
{
    Chunk*             chunk  = nullptr;
    const ChunkLayout* layout = nullptr;

    [[nodiscard]] uint16_t Count() const noexcept { return chunk->count; }

    template<typename T>
    [[nodiscard]] T* Get() const noexcept
    {
        const ComponentID id  = ComponentRegistry::Get().GetID<T>();
        const int32_t     off = layout->OffsetOf(id);
        ENT_ASSERT_MSG(off >= 0, "Component not present in this archetype");
        return reinterpret_cast<T*>(chunk->ComponentArray(off));
    }

    template<typename T>
    [[nodiscard]] bool Has() const noexcept
    {
        return layout->OffsetOf(ComponentRegistry::Get().GetID<T>()) >= 0;
    }

    [[nodiscard]] Entity* GetEntities() const noexcept { return chunk->Entities(); }

    // Raw access by ComponentID — used by serializer / inspector
    [[nodiscard]] void* GetRaw(ComponentID id) const noexcept
    {
        const int32_t off = layout->OffsetOf(id);
        if (off < 0) return nullptr;
        return chunk->ComponentArray(off);
    }
};

// ================================================================
//  ARCHETYPE
//  Owns a list of chunks. All entities share the same component set.
// ================================================================

class Archetype 
{
public:
    void Init(
        ArchetypeID                     id,
        const std::vector<ComponentID>& componentIDs,
        Signature                       sig
    ) 
    {
        mID           = id;
        mSignature    = sig;
        mComponentIDs = componentIDs;
        std::sort(mComponentIDs.begin(), mComponentIDs.end());

        mLayout = BuildChunkLayout(
            mComponentIDs.data(),
            static_cast<uint16_t>(mComponentIDs.size()),
            Chunk::DATA_SIZE
        );
        AllocateChunk();
    }

    // ── Chunk access ──────────────────────────────────────────────

    [[nodiscard]] Chunk* GetOrAllocateChunk() 
    {
        if (!mChunks.empty() && !mChunks.back()->IsFull())
            return mChunks.back().get();
        return AllocateChunk();
    }

    [[nodiscard]] Chunk* GetChunk(uint32_t idx) noexcept
    {
        ENT_ASSERT(idx < mChunks.size());
        return mChunks[idx].get();
    }
    [[nodiscard]] const Chunk* GetChunk(uint32_t idx) const noexcept 
    {
        ENT_ASSERT(idx < mChunks.size());
        return mChunks[idx].get();
    }

    [[nodiscard]] size_t ChunkCount() const noexcept { return mChunks.size(); }

    // ── Entity insertion ──────────────────────────────────────────

    struct InsertResult { uint32_t chunkIdx; uint16_t row; };

    InsertResult InsertEntity(Entity e) 
    {
        Chunk*   chunk = GetOrAllocateChunk();
        uint16_t row   = chunk->count++;
        chunk->Entities()[row] = e;

        const auto& reg = ComponentRegistry::Get();
        for (uint16_t i = 0; i < mLayout.slotCount; ++i)
        {
            const auto& slot = mLayout.slots[i];
            const auto& meta = reg.GetMeta(slot.id);
            if (meta.construct) {
                void* ptr = static_cast<std::byte*>(chunk->ComponentArray(slot.offset))
                          + row * meta.size;
                meta.construct(ptr, 1);
            }
        }
        return {chunk->chunkIdx, row};
    }

    // Insert without default-constructing — used by serializer
    InsertResult InsertEntityRaw(Entity e) 
    {
        Chunk*   chunk = GetOrAllocateChunk();
        uint16_t row   = chunk->count++;
        chunk->Entities()[row] = e;
        return {chunk->chunkIdx, row};
    }

    // ── Swap-remove ───────────────────────────────────────────────

    Entity RemoveEntity(uint32_t chunkIdx, uint16_t row)
    {
        Chunk* chunk = mChunks[chunkIdx].get();
        ENT_ASSERT(row < chunk->count);

        const uint16_t last = chunk->count - 1;
        const auto&    reg  = ComponentRegistry::Get();

        Entity swapped = INVALID_ENTITY;

        if (row != last) {
            swapped                   = chunk->Entities()[last];
            chunk->Entities()[row]    = swapped;

            for (uint16_t i = 0; i < mLayout.slotCount; ++i)
            {
                const auto& slot = mLayout.slots[i];
                const auto& meta = reg.GetMeta(slot.id);

                std::byte* base = static_cast<std::byte*>(chunk->ComponentArray(slot.offset));
                void*      dst  = base + row  * meta.size;
                void*      src  = base + last * meta.size;

                if (meta.move_ctor) 
                {
                    if (meta.destruct) meta.destruct(dst, 1);
                    meta.move_ctor(dst, src, 1);
                } else
                {
                    std::memmove(dst, src, meta.size);
                }
            }
        }

        for (uint16_t i = 0; i < mLayout.slotCount; ++i)
        {
            const auto& slot = mLayout.slots[i];
            const auto& meta = reg.GetMeta(slot.id);
            if (meta.destruct)
            {
                void* ptr = static_cast<std::byte*>(chunk->ComponentArray(slot.offset))
                          + last * meta.size;
                meta.destruct(ptr, 1);
            }
        }

        --chunk->count;
        return swapped;
    }

    // ── Cross-archetype component copy (used during migration) ────

    static void CopySharedComponents(
        Chunk*             srcChunk, const ChunkLayout& srcLayout,
        Chunk*             dstChunk, const ChunkLayout& dstLayout,
        uint16_t           srcRow,   uint16_t           dstRow
    )
    {
        const auto& reg = ComponentRegistry::Get();
        for (uint16_t di = 0; di < dstLayout.slotCount; ++di)
        {
            const ComponentID id  = dstLayout.slots[di].id;
            const int32_t     sOff = srcLayout.OffsetOf(id);
            if (sOff < 0) continue;

            const auto& meta = reg.GetMeta(id);
            void*       dst  = static_cast<std::byte*>(dstChunk->ComponentArray(dstLayout.slots[di].offset))
                             + dstRow * meta.size;
            const void* src  = static_cast<const std::byte*>(srcChunk->ComponentArray(sOff))
                             + srcRow * meta.size;

            if (meta.copy_ctor) meta.copy_ctor(dst, src, 1);
            else                std::memcpy(dst, src, meta.size);
        }
    }

    // ── Component get/set ─────────────────────────────────────────

    template<typename T>
    void SetComponent(uint32_t chunkIdx, uint16_t row, T&& value) 
    {
        const ComponentID id  = ComponentRegistry::Get().GetID<T>();
        const int32_t     off = mLayout.OffsetOf(id);
        ENT_ASSERT(off >= 0);
        T* ptr = reinterpret_cast<T*>(
            static_cast<std::byte*>(mChunks[chunkIdx]->ComponentArray(off)) + row * sizeof(T));
        *ptr = std::forward<T>(value);
    }

    template<typename T>
    [[nodiscard]] T* GetComponent(uint32_t chunkIdx, uint16_t row) noexcept
    {
        const ComponentID id  = ComponentRegistry::Get().GetID<T>();
        const int32_t     off = mLayout.OffsetOf(id);
        ENT_ASSERT(off >= 0);
        return reinterpret_cast<T*>(
            static_cast<std::byte*>(mChunks[chunkIdx]->ComponentArray(off)) + row * sizeof(T));
    }

    [[nodiscard]] void* GetComponentRaw(uint32_t chunkIdx, uint16_t row, ComponentID cid) noexcept
    {
        const int32_t off = mLayout.OffsetOf(cid);
        if (off < 0) return nullptr;
        const size_t sz = ComponentRegistry::Get().GetMeta(cid).size;
        return static_cast<std::byte*>(mChunks[chunkIdx]->ComponentArray(off)) + row * sz;
    }

    // ── Accessors ─────────────────────────────────────────────────

    [[nodiscard]] ArchetypeID         ID()           const noexcept { return mID; }
    [[nodiscard]] Signature           GetSignature() const noexcept { return mSignature; }
    [[nodiscard]] const ChunkLayout&  Layout()       const noexcept { return mLayout; }
    [[nodiscard]] uint32_t EntityCount() const noexcept
    {
        uint32_t n = 0;
        for (const auto& c : mChunks) n += c->count;
        return n;
    }
    [[nodiscard]] const std::vector<ComponentID>& ComponentIDs() const noexcept 
    {
        return mComponentIDs;
    }

    // ── Debug ─────────────────────────────────────────────────────

    void PrintLayout() const 
    {
        const auto& reg = ComponentRegistry::Get();
        std::printf("[Archetype %u] sig=0x%016llx  entities=%u  chunks=%zu  cap=%u/chunk\n",
            mID, (unsigned long long)mSignature, EntityCount(), mChunks.size(), mLayout.capacity);
        for (uint16_t i = 0; i < mLayout.slotCount; ++i) {
            const auto& s = mLayout.slots[i];
            const auto& m = reg.GetMeta(s.id);
            std::printf("  [%u] id=%-3u  sz=%-4zu  align=%-3zu  off=%-5u  %s\n",
                i, s.id, m.size, m.alignment, s.offset, m.name.data());
        }
    }

private:
    Chunk* AllocateChunk() 
    {
        void* raw = nullptr;
#if defined(_MSC_VER)
        raw = _aligned_malloc(ENT_CHUNK_SIZE, 64);
        ENT_ASSERT_MSG(raw, "Chunk allocation failed");
#else
        if (posix_memalign(&raw, 64, ENT_CHUNK_SIZE) != 0)
            ENT_ASSERT_MSG(false, "Chunk allocation failed");
#endif
        Chunk* chunk       = ::new(raw) Chunk{};
        chunk->capacity    = mLayout.capacity;
        chunk->archetypeIdx = mID;
        chunk->chunkIdx    = static_cast<uint32_t>(mChunks.size());

        mChunks.emplace_back(ChunkDeleter{}, chunk);
        return chunk;
    }

    struct ChunkDeleter 
    {
        void operator()(Chunk* c) const noexcept
        {
            c->~Chunk();
#if defined(_MSC_VER)
            _aligned_free(c);
#else
            free(c);
#endif
        }
    };

    using ChunkPtr = std::unique_ptr<Chunk, ChunkDeleter>;

    ArchetypeID              mID        = INVALID_ARCHETYPE;
    Signature                mSignature = 0;
    std::vector<ComponentID> mComponentIDs;
    ChunkLayout              mLayout{};
    std::vector<ChunkPtr>    mChunks;
};

// ================================================================
//  ENTITY RECORD
//  O(1) lookup of entity → (archetype, chunk, row)
// ================================================================

struct EntityRecord {
    Archetype* archetype = nullptr;
    uint32_t   chunkIdx  = 0;
    uint16_t   row       = 0;
    bool       alive     = false;
};

// ================================================================
//  QUERY RESULT
//  Snapshot of matching archetypes at query time.
//  Rebuilt lazily via CachedQuery when World version changes.
// ================================================================

class QueryResult 
{
public:
    QueryResult() = default;
    explicit QueryResult(Signature required, Signature excluded = 0)
        : mRequired(required), mExcluded(excluded) {}

    [[nodiscard]] Signature Required() const noexcept { return mRequired; }
    [[nodiscard]] Signature Excluded() const noexcept { return mExcluded; }

    void AddArchetype(Archetype* a) { mArchetypes.push_back(a); }
    void Clear()                    { mArchetypes.clear(); }

    [[nodiscard]] std::span<Archetype* const> Archetypes() const noexcept { return mArchetypes; }

    [[nodiscard]] uint32_t EntityCount() const noexcept {
        uint32_t n = 0;
        for (Archetype* a : mArchetypes) n += a->EntityCount();
        return n;
    }

    // ── Sequential iteration ──────────────────────────────────────

    template<typename Func>
    void ForEachChunk(Func&& func) const 
    {
        ENT_PROFILE_BEGIN("QueryResult::ForEachChunk");
        for (Archetype* arch : mArchetypes) 
        {
            const ChunkLayout& layout = arch->Layout();
            for (size_t ci = 0; ci < arch->ChunkCount(); ++ci) 
            {
                Chunk* chunk = arch->GetChunk(static_cast<uint32_t>(ci));
                if (chunk->count == 0) continue;
                func(ChunkView{chunk, &layout});
            }
        }
        ENT_PROFILE_END("QueryResult::ForEachChunk");
    }

    template<typename... Ts, typename Func>
    void ForEach(Func&& func) const 
    {
        ForEachChunk([&](ChunkView view) 
            {
            const uint16_t n = view.Count();
            auto arrays = std::tuple<Ts*...>{view.Get<Ts>()...};
            for (uint16_t i = 0; i < n; ++i) {
                std::apply([&](Ts*... ptrs) { func(ptrs[i]...); }, arrays);
            }
        });
    }

    template<typename... Ts, typename Func>
    void ForEachWithEntity(Func&& func) const 
    {
        ForEachChunk([&](ChunkView view) 
            {
            const uint16_t n       = view.Count();
            Entity*        entities = view.GetEntities();
            auto arrays = std::tuple<Ts*...>{view.Get<Ts>()...};
            for (uint16_t i = 0; i < n; ++i) {
                std::apply([&](Ts*... ptrs) { func(entities[i], ptrs[i]...); }, arrays);
            }
        });
    }

    // ── PARALLEL ITERATION ────────────────────────────────────
    //
    //  Why chunk-level, not entity-level?
    //  ───────────────────────────────────────────────────────────
    //  Entity-level granularity means N synchronization points for N
    //  entities. Chunk-level means at most C synchronization points
    //  where C << N (typically C = N/512).
    //
    //  False sharing avoidance:
    //  Each chunk is 16 KB and cache-line aligned (64 B). Threads
    //  work on disjoint chunks, so they never share a cache line.
    //  Two threads will never write to the same 64-byte region.
    
    template<typename Func>
    void ParallelForEachChunk(Func&& func, unsigned numThreads = 0) const
    {
        ENT_PROFILE_BEGIN("QueryResult::ParallelForEachChunk");

        // Collect non-empty (chunk, layout) pairs
        std::vector<std::pair<Chunk*, const ChunkLayout*>> work;
        work.reserve(64);
        for (Archetype* arch : mArchetypes) 
        {
            const ChunkLayout* layout = &arch->Layout();
            for (size_t ci = 0; ci < arch->ChunkCount(); ++ci) 
            {
                Chunk* c = arch->GetChunk(static_cast<uint32_t>(ci));
                if (c->count > 0) work.push_back({c, layout});
            }
        }

        if (work.empty()) { ENT_PROFILE_END("QueryResult::ParallelForEachChunk"); return; }

        if (numThreads == 0)
            numThreads = std::max(1u, std::thread::hardware_concurrency());

        const size_t n       = work.size();
        const size_t nActual = std::min(static_cast<size_t>(numThreads), n);

        // Fall back to serial if not worth parallelising
        if (nActual <= 1 || n < ENT_PARALLEL_MIN_CHUNKS) 
        {
            for (auto& [c, l] : work) func(ChunkView{c, l});
            ENT_PROFILE_END("QueryResult::ParallelForEachChunk");
            return;
        }

        // Divide chunks evenly among threads — no locks needed because
        // each thread exclusively owns its range of chunks.
        const size_t perThread = (n + nActual - 1) / nActual;

        std::vector<std::thread> threads;
        threads.reserve(nActual);

        for (size_t t = 0; t < nActual; ++t)
        {
            size_t start = t * perThread;
            size_t end   = std::min(start + perThread, n);
            if (start >= end) break;

            threads.emplace_back([&work, &func, start, end]() {
                for (size_t i = start; i < end; ++i) 
                {
                    auto& [c, l] = work[i];
                    func(ChunkView{c, l});
                }
            });
        }

        for (auto& t : threads) t.join();

        ENT_PROFILE_END("QueryResult::ParallelForEachChunk");
    }

    // Parallel ForEach convenience
    template<typename... Ts, typename Func>
    void ParallelForEach(Func&& func, unsigned numThreads = 0) const
    {
        ParallelForEachChunk([&](ChunkView view) {
            const uint16_t n = view.Count();
            auto arrays = std::tuple<Ts*...>{view.Get<Ts>()...};
            for (uint16_t i = 0; i < n; ++i)
            {
                std::apply([&](Ts*... ptrs) { func(ptrs[i]...); }, arrays);
            }
        }, numThreads);
    }

private:
    Signature               mRequired  = 0;
    Signature               mExcluded  = 0;
    std::vector<Archetype*> mArchetypes;
};

// ================================================================
//
//  WHY DEFERRED COMMANDS?
//  ─────────────────────────────────────────────────────────────
//  During ForEach / ForEachChunk iteration, the World's archetype
//  list and chunk memory are live. Calling AddComponent immediately:
//
//    1. May allocate a NEW archetype → extends mArchetypes vector
//       → iterator/pointer invalidation if reallocation occurs.
//    2. Moves the entity to a different chunk via swap-remove,
//       changing the entity at the current row mid-iteration.
//       The very entity you just processed is now gone; another
//       entity appears at that row and is silently skipped.
//    3. Destroys an entity that another iteration branch still
//       holds a pointer into — instant undefined behaviour.
//
//  PHANTOM ENTITY IDs
//  ─────────────────────────────────────────────────────────────
//  CommandBuffer::CreateEntity() returns a "phantom" ID (≥ 0xE0000000).
//  FlushCommands() first creates all real entities, builds a
//  phantom→ real map, then replays every other command substituting
//  real IDs. You can safely chain CreateEntity + AddComponent
//  inside the same buffer.
// ================================================================

class CommandBuffer 
{
public:
    static constexpr Entity PHANTOM_BASE = 0xE000'0000u;

    CommandBuffer() = default;

    // ── Record API ────────────────────────────────────────────────

    [[nodiscard]] Entity CreateEntity() 
    {
        Entity ph = mNextPhantom++;
        mOrder.push_back({CmdKind::Create, mCreates.size()});
        mCreates.push_back({ph});
        return ph;
    }

    void DestroyEntity(Entity e) 
    {
        mOrder.push_back({CmdKind::Destroy, mDestroys.size()});
        mDestroys.push_back({e});
    }

    template<typename T>
    void AddComponent(Entity e, T value = T{})
    {
        ComponentID cid = ComponentRegistry::Get().GetID<T>();
        mOrder.push_back({CmdKind::Add, mAdds.size()});

        AddCmd cmd;
        cmd.entity = e;
        cmd.compID = cid;
        // Store component data as raw bytes (safe for trivially copyable types)
        // For non-trivial types a copy-constructed object lives here
        cmd.data.resize(sizeof(T));
        ::new(cmd.data.data()) T(std::move(value));
        cmd.apply = [](World& w, Entity ent, const std::vector<std::byte>& data) {
            w.AddComponent<T>(ent, *reinterpret_cast<const T*>(data.data()));
        };
        mAdds.push_back(std::move(cmd));
    }

    template<typename T>
    void RemoveComponent(Entity e) 
    {
        mOrder.push_back({CmdKind::Remove, mRemoves.size()});
        mRemoves.push_back({e, ComponentRegistry::Get().GetID<T>(),
            [](World& w, Entity ent) { w.RemoveComponent<T>(ent); }});
    }

    void Clear()
    {
        mOrder.clear();
        mCreates.clear();
        mDestroys.clear();
        mAdds.clear();
        mRemoves.clear();
        mNextPhantom = PHANTOM_BASE;
    }

    [[nodiscard]] bool   IsEmpty()      const noexcept { return mOrder.empty(); }
    [[nodiscard]] size_t CommandCount() const noexcept { return mOrder.size(); }

    [[nodiscard]] bool IsPhantom(Entity e) const noexcept 
    {
        return e >= PHANTOM_BASE;
    }

private:
    friend class World;

    enum class CmdKind : uint8_t { Create, Destroy, Add, Remove };

    struct OrderEntry { CmdKind kind; size_t idx; };

    struct CreateCmd  { Entity phantom; };
    struct DestroyCmd { Entity entity; };

    struct AddCmd 
    {
        Entity                    entity;
        ComponentID               compID;
        std::vector<std::byte>    data;
        std::function<void(World&, Entity, const std::vector<std::byte>&)> apply;
    };
    struct RemoveCmd
    {
        Entity                            entity;
        ComponentID                       compID;
        std::function<void(World&, Entity)> apply;
    };

    std::vector<OrderEntry>  mOrder;
    std::vector<CreateCmd>   mCreates;
    std::vector<DestroyCmd>  mDestroys;
    std::vector<AddCmd>      mAdds;
    std::vector<RemoveCmd>   mRemoves;

    Entity mNextPhantom = PHANTOM_BASE;
};

// ================================================================
//  WORLD STATS  — snapshot of memory and entity counts
// ================================================================

struct WorldStats 
{
    uint32_t liveEntities         = 0;
    uint32_t freeEntitySlots      = 0;
    uint32_t archetypeCount       = 0;
    uint32_t totalChunks          = 0;
    size_t   chunkMemoryBytes     = 0;
    size_t   recordMemoryBytes    = 0;
    uint64_t worldVersion         = 0;

    void Print() const 
    {
        std::printf("=== WorldStats ===\n");
        std::printf("  Live entities    : %u\n",   liveEntities);
        std::printf("  Free slots       : %u\n",   freeEntitySlots);
        std::printf("  Archetypes       : %u\n",   archetypeCount);
        std::printf("  Total chunks     : %u\n",   totalChunks);
        std::printf("  Chunk memory     : %.2f KB\n", chunkMemoryBytes / 1024.0);
        std::printf("  Record memory    : %.2f KB\n", recordMemoryBytes / 1024.0);
        std::printf("  World version    : %llu\n", (unsigned long long)worldVersion);
    }
};

// ================================================================
//  WORLD
//  Central manager: owns archetypes, entity records, and all
// ================================================================

class World 
{
public:
    World()
    {
        mEntityRecords.resize(ENT_INITIAL_ENTITY_CAPACITY);
        mNextEntity = 1; // entity 0 is INVALID sentinel
    }

    ~World() = default;
    World(const World&)            = delete;
    World& operator=(const World&) = delete;

    // ── Entity lifecycle ──────────────────────────────────────────

    [[nodiscard]] Entity CreateEntity() 
    {
        Entity e;
        if (!mFreeList.empty())
        {
            e = mFreeList.back();
            mFreeList.pop_back();
        } else 
        {
            e = mNextEntity++;
            if (e >= static_cast<Entity>(mEntityRecords.size()))
                mEntityRecords.resize(mEntityRecords.size() * 2);
        }
        mEntityRecords[e]         = EntityRecord{};
        mEntityRecords[e].alive   = true;
        auto* arch                = GetOrCreateArchetype({});
        auto [ci, row]            = arch->InsertEntity(e);
        mEntityRecords[e].archetype = arch;
        mEntityRecords[e].chunkIdx  = ci;
        mEntityRecords[e].row       = row;
        return e;
    }

    void DestroyEntity(Entity e) 
    {
        ENT_ASSERT_MSG(IsAlive(e), "Destroying dead/invalid entity");
        auto& rec = mEntityRecords[e];

        // Fire OnRemove for every component this entity has
        for (ComponentID cid : rec.archetype->ComponentIDs())
            FireOnRemove(cid, e);

        Entity swapped = rec.archetype->RemoveEntity(rec.chunkIdx, rec.row);
        if (swapped != INVALID_ENTITY) 
        {
            mEntityRecords[swapped].chunkIdx = rec.chunkIdx;
            mEntityRecords[swapped].row      = rec.row;
        }
        rec = EntityRecord{};
        mFreeList.push_back(e);
    }

    [[nodiscard]] bool IsAlive(Entity e) const noexcept
    {
        return e < static_cast<Entity>(mEntityRecords.size()) && mEntityRecords[e].alive;
    }

    // ── Component operations ──────────────────────────────────────

    template<typename T>
    T& AddComponent(Entity e, T value = T{}) 
    {
        ENT_ASSERT(IsAlive(e));
        const ComponentID cid = ComponentRegistry::Get().GetID<T>();
        auto&             rec = mEntityRecords[e];
        Archetype*        old = rec.archetype;

        if ((old->GetSignature() & SignatureBit(cid)) != 0)
        {
            // Already has component — just update value in place
            old->SetComponent<T>(rec.chunkIdx, rec.row, std::move(value));
            return *old->GetComponent<T>(rec.chunkIdx, rec.row);
        }

        std::vector<ComponentID> newIDs = old->ComponentIDs();
        newIDs.push_back(cid);
        std::sort(newIDs.begin(), newIDs.end());

        Archetype* next      = GetOrCreateArchetype(newIDs);
        auto [newCI, newRow] = next->InsertEntity(e);

        Archetype::CopySharedComponents(
            old->GetChunk(rec.chunkIdx),  old->Layout(),
            next->GetChunk(newCI),        next->Layout(),
            rec.row, newRow
        );
        next->SetComponent<T>(newCI, newRow, std::move(value));

        Entity swapped = old->RemoveEntity(rec.chunkIdx, rec.row);
        if (swapped != INVALID_ENTITY)
        {
            mEntityRecords[swapped].chunkIdx = rec.chunkIdx;
            mEntityRecords[swapped].row      = rec.row;
        }

        rec.archetype = next;
        rec.chunkIdx  = newCI;
        rec.row       = newRow;

        FireOnAdd(cid, e);
        return *next->GetComponent<T>(newCI, newRow);
    }

    template<typename T>
    void RemoveComponent(Entity e) 
    {
        ENT_ASSERT(IsAlive(e));
        const ComponentID cid = ComponentRegistry::Get().GetID<T>();
        auto&             rec = mEntityRecords[e];
        Archetype*        old = rec.archetype;

        ENT_ASSERT_MSG((old->GetSignature() & SignatureBit(cid)) != 0,
            "Entity does not have this component");

        FireOnRemove(cid, e);

        std::vector<ComponentID> newIDs;
        newIDs.reserve(old->ComponentIDs().size());
        for (ComponentID id : old->ComponentIDs())
            if (id != cid) newIDs.push_back(id);

        Archetype* next      = GetOrCreateArchetype(newIDs);
        auto [newCI, newRow] = next->InsertEntity(e);

        Archetype::CopySharedComponents(
            old->GetChunk(rec.chunkIdx), old->Layout(),
            next->GetChunk(newCI),       next->Layout(),
            rec.row, newRow
        );

        Entity swapped = old->RemoveEntity(rec.chunkIdx, rec.row);
        if (swapped != INVALID_ENTITY)
        {
            mEntityRecords[swapped].chunkIdx = rec.chunkIdx;
            mEntityRecords[swapped].row      = rec.row;
        }

        rec.archetype = next;
        rec.chunkIdx  = newCI;
        rec.row       = newRow;
    }

    template<typename T>
    [[nodiscard]] bool HasComponent(Entity e) const noexcept 
    {
        ENT_ASSERT(IsAlive(e));
        return (mEntityRecords[e].archetype->GetSignature()
                & SignatureBit(ComponentRegistry::Get().GetID<T>())) != 0;
    }

    template<typename T>
    [[nodiscard]] T& GetComponent(Entity e) 
    {
        ENT_ASSERT(IsAlive(e) && HasComponent<T>(e));
        auto& rec = mEntityRecords[e];
        return *rec.archetype->GetComponent<T>(rec.chunkIdx, rec.row);
    }

    template<typename T>
    [[nodiscard]] const T& GetComponent(Entity e) const
    {
        ENT_ASSERT(IsAlive(e) && HasComponent<T>(e));
        const auto& rec = mEntityRecords[e];
        return *rec.archetype->GetComponent<T>(rec.chunkIdx, rec.row);
    }

    // ── Entity Cloning ────────────────────────────────────────
    // Deep-copy all components from source to a new entity.

    [[nodiscard]] Entity CloneEntity(Entity source)
    {
        ENT_ASSERT(IsAlive(source));
        const auto& srcRec  = mEntityRecords[source];
        Archetype*  srcArch = srcRec.archetype;

        Entity clone = CreateEntity();
        if (srcArch->ComponentIDs().empty()) return clone;

        // Migrate clone directly into source's archetype
        auto& cloneRec  = mEntityRecords[clone];
        Archetype* empty = cloneRec.archetype;

        Archetype* dstArch   = GetOrCreateArchetype(srcArch->ComponentIDs());
        auto [newCI, newRow] = dstArch->InsertEntity(clone);

        // Copy all components
        Archetype::CopySharedComponents(
            srcArch->GetChunk(srcRec.chunkIdx),  srcArch->Layout(),
            dstArch->GetChunk(newCI),             dstArch->Layout(),
            srcRec.row, newRow
        );

        Entity swapped = empty->RemoveEntity(cloneRec.chunkIdx, cloneRec.row);
        if (swapped != INVALID_ENTITY)
        {
            mEntityRecords[swapped].chunkIdx = cloneRec.chunkIdx;
            mEntityRecords[swapped].row      = cloneRec.row;
        }

        cloneRec.archetype = dstArch;
        cloneRec.chunkIdx  = newCI;
        cloneRec.row       = newRow;

        return clone;
    }

    // ── Command Buffer flushing ────────────────────────────────

    void FlushCommands(CommandBuffer& buf)
    {
        ENT_PROFILE_BEGIN("World::FlushCommands");
        if (buf.IsEmpty()) { ENT_PROFILE_END("World::FlushCommands"); return; }

        // Pass 1: create real entities for all phantoms
        // Doing this first guarantees subsequent Add/Remove commands
        // targeting phantom IDs resolve correctly regardless of order.
        std::unordered_map<Entity, Entity> phantomMap;
        phantomMap.reserve(buf.mCreates.size());
        for (const auto& cmd : buf.mCreates)
        {
            Entity real = CreateEntity();
            phantomMap[cmd.phantom] = real;
        }

        auto resolve = [&](Entity e) -> Entity
            {
            if (e >= CommandBuffer::PHANTOM_BASE)
            {
                auto it = phantomMap.find(e);
                ENT_ASSERT_MSG(it != phantomMap.end(), "Unknown phantom entity in CommandBuffer");
                return it->second;
            }
            return e;
        };

        // Pass 2: replay in recorded order, substituting real IDs
        for (const auto& entry : buf.mOrder)
        {
            switch (entry.kind)
            {
            case CommandBuffer::CmdKind::Create:
                // Already handled in pass 1
                break;
            case CommandBuffer::CmdKind::Destroy:
            {
                Entity e = resolve(buf.mDestroys[entry.idx].entity);
                if (IsAlive(e)) DestroyEntity(e);
                break;
            }
            case CommandBuffer::CmdKind::Add: 
            {
                auto& cmd = buf.mAdds[entry.idx];
                Entity e  = resolve(cmd.entity);
                if (IsAlive(e)) cmd.apply(*this, e, cmd.data);
                break;
            }
            case CommandBuffer::CmdKind::Remove:
            {
                auto& cmd = buf.mRemoves[entry.idx];
                Entity e  = resolve(cmd.entity);
                if (IsAlive(e)) cmd.apply(*this, e);
                break;
            }
            }
        }

        buf.Clear();
        ENT_PROFILE_END("World::FlushCommands");
    }

    // ── Query system ──────────────────────────────────────────────

    template<typename... Ts>
    [[nodiscard]] QueryResult Query() 
    {
        return QueryInternal(BuildSignature<Ts...>(), 0);
    }

    template<typename... Required>
    [[nodiscard]] QueryResult QueryExcluding(Signature excluded)
    {
        return QueryInternal(BuildSignature<Required...>(), excluded);
    }

    [[nodiscard]] uint64_t GetVersion() const noexcept { return mQueryVersion; }

    // ── Observer / lifecycle hooks ─────────────────────────────
    //
    //  Callbacks fire AFTER AddComponent (component is accessible)
    //  and BEFORE DestroyEntity removes the component from storage.

    template<typename T>
    void OnAdd(std::function<void(World&, Entity)> callback)
    {
        ComponentID cid = ComponentRegistry::Get().GetID<T>();
        mOnAddObservers[cid].push_back(std::move(callback));
    }

    template<typename T>
    void OnRemove(std::function<void(World&, Entity)> callback) 
    {
        ComponentID cid = ComponentRegistry::Get().GetID<T>();
        mOnRemoveObservers[cid].push_back(std::move(callback));
    }

    // ── Singleton components ──────────────────────────────────
    //  World-level (not per-entity) shared state — configuration,
    //  resource handles, global timers, etc.

    template<typename T>
    void SetSingleton(T value = T{})
    {
        ComponentID cid = ComponentRegistry::Get().GetID<T>();
        auto buf        = std::make_unique<std::byte[]>(sizeof(T));
        ::new(buf.get()) T(std::move(value));
        mSingletons[cid] = std::move(buf);
    }

    template<typename T>
    [[nodiscard]] T& GetSingleton() 
    {
        ComponentID cid = ComponentRegistry::Get().GetID<T>();
        auto it         = mSingletons.find(cid);
        ENT_ASSERT_MSG(it != mSingletons.end(), "Singleton not set — call SetSingleton<T>() first");
        return *reinterpret_cast<T*>(it->second.get());
    }

    template<typename T>
    [[nodiscard]] const T& GetSingleton() 
    {
        ComponentID cid = ComponentRegistry::Get().GetID<T>();
        auto it         = mSingletons.find(cid);
        ENT_ASSERT_MSG(it != mSingletons.end(), "Singleton not set");
        return *reinterpret_cast<const T*>(it->second.get());
    }

    template<typename T>
    [[nodiscard]] bool HasSingleton() const noexcept 
    {
        ComponentID cid = ComponentRegistry::Get().GetID<T>();
        return mSingletons.count(cid) > 0;
    }

    template<typename T>
    void RemoveSingleton()
    {
        ComponentID cid = ComponentRegistry::Get().GetID<T>();
        mSingletons.erase(cid);
    }

    // ──  SERIALIZATION ─────────────────────────────────────────
    //
    //  Binary format:
    //  ┌───────────────────────────────────────────────┐
    //  │ Magic[4]        "ECS2"                        │
    //  │ Version[4]      = 1                           │
    //  │ NumComponents[4]                              │
    //  │ For each component:                           │
    //  │   NameLen[2]  Name[NameLen]  Size[4]          │
    //  │ NextEntityID[4]                               │
    //  │ FreeListSize[4]  FreeList[4*N]                │
    //  │ NumArchetypes[4]                              │
    //  │ For each archetype:                           │
    //  │   Signature[8]  NumCompIDs[2]  CompIDs[2*M]  │
    //  │   EntityCount[4]                              │
    //  │   For each entity:                            │
    //  │     EntityID[4]  ComponentData[...]           │
    //  └───────────────────────────────────────────────┘
    //
    
    [[nodiscard]] bool Save(const std::string& path) const
    {
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;

        auto write = [&](const void* data, size_t sz) {
            f.write(static_cast<const char*>(data), static_cast<std::streamsize>(sz));
        };
        auto writeU16 = [&](uint16_t v) { write(&v, 2); };
        auto writeU32 = [&](uint32_t v) { write(&v, 4); };
        auto writeU64 = [&](uint64_t v) { write(&v, 8); };

        // Header
        write("ECS2", 4);
        writeU32(1); // version

        // Component registry snapshot
        const auto& reg = ComponentRegistry::Get();
        writeU32(static_cast<uint32_t>(reg.Count()));
        for (uint32_t i = 0; i < static_cast<uint32_t>(reg.Count()); ++i)
        {
            const auto& meta = reg.GetMeta(static_cast<ComponentID>(i));
            uint16_t nameLen = static_cast<uint16_t>(meta.name.size());
            writeU16(nameLen);
            write(meta.name.data(), nameLen);
            writeU32(static_cast<uint32_t>(meta.size));
        }

        // Entity pool state
        writeU32(mNextEntity);
        writeU32(static_cast<uint32_t>(mFreeList.size()));
        for (Entity e : mFreeList) writeU32(e);

        // Archetypes
        writeU32(static_cast<uint32_t>(mArchetypes.size()));
        for (const auto& arch : mArchetypes) 
        {
            writeU64(arch->GetSignature());

            const auto& ids = arch->ComponentIDs();
            writeU16(static_cast<uint16_t>(ids.size()));
            for (ComponentID cid : ids) writeU16(cid);

            writeU32(arch->EntityCount());
            const ChunkLayout& layout = arch->Layout();

            for (size_t ci = 0; ci < arch->ChunkCount(); ++ci) {
                const Chunk* chunk = arch->GetChunk(static_cast<uint32_t>(ci));
                if (chunk->count == 0) continue;

                for (uint16_t row = 0; row < chunk->count; ++row)
                {
                    writeU32(chunk->Entities()[row]);
                    // Write each component in layout slot order
                    for (uint16_t si = 0; si < layout.slotCount; ++si) 
                    {
                        const auto& slot = layout.slots[si];
                        const auto& meta = reg.GetMeta(slot.id);
                        const std::byte* base =
                            static_cast<const std::byte*>(
                                const_cast<Chunk*>(chunk)->ComponentArray(slot.offset));
                        write(base + row * meta.size, meta.size);
                    }
                }
            }
        }

        return f.good();
    }

    [[nodiscard]] bool Load(const std::string& path) 
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;

        auto readU16 = [&]() -> uint16_t { uint16_t v = 0; f.read(reinterpret_cast<char*>(&v), 2); return v; };
        auto readU32 = [&]() -> uint32_t { uint32_t v = 0; f.read(reinterpret_cast<char*>(&v), 4); return v; };
        auto readU64 = [&]() -> uint64_t { uint64_t v = 0; f.read(reinterpret_cast<char*>(&v), 8); return v; };

        // Verify magic
        char magic[5] = {};
        f.read(magic, 4);
        if (std::strncmp(magic, "ECS2", 4) != 0) return false;

        uint32_t version = readU32();
        if (version != 1) return false;

        // Read component registry, build saved-name → current-ID map
        std::unordered_map<uint32_t, ComponentID> savedToCurrent; // savedIdx → currentID
        const auto& reg = ComponentRegistry::Get();

        uint32_t numSavedComps = readU32();
        for (uint32_t i = 0; i < numSavedComps; ++i) 
        {
            uint16_t nameLen = readU16();
            std::string name(nameLen, '\0');
            f.read(name.data(), nameLen);
            uint32_t savedSize = readU32();

            // Match by name
            bool found = false;
            for (uint32_t ci = 0; ci < static_cast<uint32_t>(reg.Count()); ++ci)
            {
                const auto& meta = reg.GetMeta(static_cast<ComponentID>(ci));
                if (meta.name == name && meta.size == savedSize) {
                    savedToCurrent[i] = static_cast<ComponentID>(ci);
                    found = true;
                    break;
                }
            }
            (void)found; // unmatched components are skipped during load
        }

        // Clear existing world state
        mEntityRecords.clear();
        mArchetypes.clear();
        mFreeList.clear();
        mOnAddObservers.clear();
        mOnRemoveObservers.clear();
        mSingletons.clear();
        mQueryVersion = 0;

        // Restore entity pool
        mNextEntity = readU32();
        mEntityRecords.resize(std::max(static_cast<Entity>(ENT_INITIAL_ENTITY_CAPACITY), mNextEntity + 1));
        uint32_t freeCount = readU32();
        mFreeList.resize(freeCount);
        for (Entity& e : mFreeList) e = readU32();

        // Restore archetypes
        uint32_t numArchs = readU32();
        for (uint32_t ai = 0; ai < numArchs; ++ai) 
        {
            uint64_t  sig         = readU64();
            uint16_t  numCompIDs  = readU16();
            std::vector<ComponentID> savedIDs(numCompIDs);
            for (uint16_t ci = 0; ci < numCompIDs; ++ci)
                savedIDs[ci] = readU16();

            // Remap saved IDs to current IDs
            std::vector<ComponentID> currentIDs;
            currentIDs.reserve(numCompIDs);
            for (ComponentID sid : savedIDs) {
                auto it = savedToCurrent.find(sid);
                if (it != savedToCurrent.end())
                    currentIDs.push_back(it->second);
            }

            Archetype* arch = GetOrCreateArchetype(currentIDs);

            uint32_t entityCount = readU32();
            for (uint32_t ei = 0; ei < entityCount; ++ei)
            {
                Entity e = readU32();

                // Ensure records vector is large enough
                if (e >= static_cast<Entity>(mEntityRecords.size()))
                    mEntityRecords.resize(e + 1);

                auto [ci, row] = arch->InsertEntityRaw(e);

                const ChunkLayout& layout = arch->Layout();
                Chunk* chunk = arch->GetChunk(ci);

                for (uint16_t si = 0; si < layout.slotCount; ++si) 
                {
                    const auto& slot = layout.slots[si];
                    const auto& meta = reg.GetMeta(slot.id);
                    std::byte* dst =
                        static_cast<std::byte*>(chunk->ComponentArray(slot.offset))
                        + row * meta.size;
                    f.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(meta.size));
                }

                mEntityRecords[e].archetype = arch;
                mEntityRecords[e].chunkIdx  = ci;
                mEntityRecords[e].row       = row;
                mEntityRecords[e].alive     = true;
            }
        }

        (void)sig; // silence unused warning for last sig read above
        return f.good();
    }

    // ── Debug & Inspector ──────────────────────────────────────

    void InspectEntity(Entity e) const 
    {
        if (!IsAlive(e))
        {
            std::printf("Entity %u: DEAD or INVALID\n", e);
            return;
        }
        const auto& rec = mEntityRecords[e];
        std::printf("──── Entity %u ────\n", e);
        std::printf("  Archetype : %u (sig=0x%016llx)\n",
            rec.archetype->ID(), (unsigned long long)rec.archetype->GetSignature());
        std::printf("  Location  : chunk=%u  row=%u\n", rec.chunkIdx, rec.row);
        std::printf("  Components:\n");
        const auto& reg = ComponentRegistry::Get();
        const auto& layout = rec.archetype->Layout();
        for (uint16_t i = 0; i < layout.slotCount; ++i)
        {
            const auto& slot = layout.slots[i];
            const auto& meta = reg.GetMeta(slot.id);
            std::printf("    [%u] %-40s  sz=%-4zu  align=%zu\n",
                slot.id, meta.name.data(), meta.size, meta.alignment);
        }
    }

    void PrintStats() const 
    {
        GetStats().Print();
        std::printf("──── Archetypes ────\n");
        for (const auto& arch : mArchetypes)
            arch->PrintLayout();
    }

    [[nodiscard]] WorldStats GetStats() const noexcept
    {
        WorldStats s{};
        s.liveEntities      = LiveEntityCount();
        s.freeEntitySlots   = static_cast<uint32_t>(mFreeList.size());
        s.archetypeCount    = static_cast<uint32_t>(mArchetypes.size());
        s.worldVersion      = mQueryVersion;
        s.recordMemoryBytes = mEntityRecords.size() * sizeof(EntityRecord);
        for (const auto& arch : mArchetypes) {
            s.totalChunks    += static_cast<uint32_t>(arch->ChunkCount());
            s.chunkMemoryBytes += arch->ChunkCount() * ENT_CHUNK_SIZE;
        }
        return s;
    }

    void PrintMemoryStats() const { GetStats().Print(); }

    [[nodiscard]] size_t ArchetypeCount()   const noexcept { return mArchetypes.size(); }
    [[nodiscard]] uint32_t LiveEntityCount() const noexcept 
    {
        uint32_t n = 0;
        for (const auto& arch : mArchetypes) n += arch->EntityCount();
        return n;
    }

private:
    // ── Internal archetype management ─────────────────────────────

    [[nodiscard]] Archetype* GetOrCreateArchetype(const std::vector<ComponentID>& ids) 
    {
        std::vector<ComponentID> sorted = ids;
        std::sort(sorted.begin(), sorted.end());

        Signature sig = 0;
        for (ComponentID id : sorted) sig |= SignatureBit(id);

        for (auto& arch : mArchetypes)
            if (arch->GetSignature() == sig) return arch.get();

        ArchetypeID newID = static_cast<ArchetypeID>(mArchetypes.size());
        auto arch = std::make_unique<Archetype>();
        arch->Init(newID, sorted, sig);
        Archetype* ptr = arch.get();
        mArchetypes.push_back(std::move(arch));
        mQueryVersion++; // notify CachedQuery instances
        return ptr;
    }

    [[nodiscard]] QueryResult QueryInternal(Signature required, Signature excluded) const
    {
        QueryResult result(required, excluded);
        for (const auto& arch : mArchetypes) {
            const Signature sig = arch->GetSignature();
            if ((sig & required) == required && (sig & excluded) == 0)
                result.AddArchetype(arch.get());
        }
        return result;
    }

    template<typename... Ts>
    [[nodiscard]] static Signature BuildSignature() 
    {
        Signature sig = 0;
        ((sig |= SignatureBit(ComponentRegistry::Get().GetID<Ts>())), ...);
        return sig;
    }

    void FireOnAdd(ComponentID cid, Entity e) 
    {
        auto it = mOnAddObservers.find(cid);
        if (it != mOnAddObservers.end())
            for (auto& cb : it->second) cb(*this, e);
    }

    void FireOnRemove(ComponentID cid, Entity e)
    {
        auto it = mOnRemoveObservers.find(cid);
        if (it != mOnRemoveObservers.end())
            for (auto& cb : it->second) cb(*this, e);
    }

    // ── Members ───────────────────────────────────────────────────

    std::vector<EntityRecord>              mEntityRecords;
    std::vector<std::unique_ptr<Archetype>> mArchetypes;
    std::vector<Entity>                    mFreeList;
    Entity                                 mNextEntity   = 1;
    uint64_t                               mQueryVersion = 0;

    // §8 Observers
    std::unordered_map<ComponentID, std::vector<std::function<void(World&,Entity)>>> mOnAddObservers;
    std::unordered_map<ComponentID, std::vector<std::function<void(World&,Entity)>>> mOnRemoveObservers;

    // §10 Singleton components
    std::unordered_map<ComponentID, std::unique_ptr<std::byte[]>> mSingletons;
};

// ================================================================
//   VERSIONED CACHED QUERY
// ================================================================
template<typename... Ts>
class CachedQuery {
public:
    explicit CachedQuery(World& world)
        : mWorld(&world), mCachedVersion(~uint64_t{0})
    {
        Refresh(); // force initial build
    }

    void Refresh() 
    {
        mResult        = mWorld->Query<Ts...>();
        mCachedVersion = mWorld->GetVersion();
    }

    // O(1) check — only rebuilds if world version has advanced
    void MaybeRefresh() 
    {
        if (mWorld->GetVersion() != mCachedVersion) Refresh();
    }

    template<typename Func>
    void ForEach(Func&& func) 
    {
        MaybeRefresh();
        mResult.template ForEach<Ts...>(std::forward<Func>(func));
    }

    template<typename Func>
    void ForEachWithEntity(Func&& func)
    {
        MaybeRefresh();
        mResult.template ForEachWithEntity<Ts...>(std::forward<Func>(func));
    }

    template<typename Func>
    void ForEachChunk(Func&& func)
    {
        MaybeRefresh();
        mResult.ForEachChunk(std::forward<Func>(func));
    }

    template<typename Func>
    void ParallelForEachChunk(Func&& func, unsigned threads = 0) 
    {
        MaybeRefresh();
        mResult.ParallelForEachChunk(std::forward<Func>(func), threads);
    }

    template<typename Func>
    void ParallelForEach(Func&& func, unsigned threads = 0) 
    {
        MaybeRefresh();
        mResult.template ParallelForEach<Ts...>(std::forward<Func>(func), threads);
    }

    [[nodiscard]] uint32_t EntityCount() { MaybeRefresh(); return mResult.EntityCount(); }
    [[nodiscard]] const QueryResult& Result() { MaybeRefresh(); return mResult; }

private:
    World*      mWorld;
    QueryResult mResult;
    uint64_t    mCachedVersion;
};

// ================================================================
//  ENTITY HANDLE
//  
//  Usage:
//    auto h = EntityHandle::Create(world)
//               .Add<Position>({1,2,3})
//               .Add<Velocity>({0,1,0});
//    Entity id = h.ID();
//    h.Release(); // hand off ownership to ECS
// ================================================================

class EntityHandle
{
public:
    EntityHandle() = default;

    static EntityHandle Create(World& world)
    {
        return EntityHandle(world, world.CreateEntity());
    }

    EntityHandle(World& world, Entity e) : mWorld(&world), mEntity(e) {}

    ~EntityHandle() 
    {
        if (mOwns && mWorld && mWorld->IsAlive(mEntity))
            mWorld->DestroyEntity(mEntity);
    }

    EntityHandle(EntityHandle&& o) noexcept
        : mWorld(o.mWorld), mEntity(o.mEntity), mOwns(o.mOwns) { o.mOwns = false; }

    EntityHandle& operator=(EntityHandle&& o) noexcept
    {
        if (this != &o) {
            if (mOwns && mWorld && mWorld->IsAlive(mEntity))
                mWorld->DestroyEntity(mEntity);
            mWorld  = o.mWorld;
            mEntity = o.mEntity;
            mOwns   = o.mOwns;
            o.mOwns = false;
        }
        return *this;
    }

    EntityHandle(const EntityHandle&)            = delete;
    EntityHandle& operator=(const EntityHandle&) = delete;

    template<typename T>
    EntityHandle& Add(T value = T{}) 
    {
        mWorld->AddComponent<T>(mEntity, std::move(value));
        return *this;
    }

    template<typename T>
    EntityHandle& Remove()
    {
        mWorld->RemoveComponent<T>(mEntity);
        return *this;
    }

    template<typename T>
    [[nodiscard]] T& Get() { return mWorld->GetComponent<T>(mEntity); }

    template<typename T>
    [[nodiscard]] const T& Get() const { return mWorld->GetComponent<T>(mEntity); }

    template<typename T>
    [[nodiscard]] bool Has() const { return mWorld->HasComponent<T>(mEntity); }

    void Destroy() {
        if (mOwns && mWorld && mWorld->IsAlive(mEntity)) {
            mWorld->DestroyEntity(mEntity);
            mOwns = false;
        }
    }

    // Hand off ownership to the ECS — handle no longer auto-destroys
    void Release() noexcept { mOwns = false; }

    [[nodiscard]] Entity ID()      const noexcept { return mEntity; }
    [[nodiscard]] bool   IsAlive() const noexcept { return mWorld && mWorld->IsAlive(mEntity); }
    [[nodiscard]] bool   Valid()   const noexcept { return mWorld && mEntity != INVALID_ENTITY; }

    explicit operator Entity() const noexcept { return mEntity; }

private:
    World*  mWorld  = nullptr;
    Entity  mEntity = INVALID_ENTITY;
    bool    mOwns   = true;
};

// ================================================================
//  SYSTEM FRAMEWORK
//
//  Design philosophy:
//  ─────────────────────────────────────────────────────────────
//  Systems are plain classes with an Update(World&, float) method.
//  No base class, no virtual dispatch, no RTTI required.
//  The concept check enforces the interface at compile time.
//
//  SystemScheduler stores type-erased std::function wrappers.
//  The scheduler overhead (one std::function call per system
//  per frame) is negligible — systems spend 99% of time iterating
//  chunks.
//
//  Cache-friendliness: each system operates on a CachedQuery
//  that returns contiguous SoA chunk data. All entities processed
//  by one system touch the same memory ranges — no pointer chasing.
//
//  Execution order: systems execute in ascending priority order.
//  Assign priorities at registration time (lower = earlier).
//  Typical assignment:
//    0–99   : input / pre-physics
//    100–199: physics / movement
//    200–299: game logic
//    300–399: health / damage
//    400–499: rendering prep
//    500+   : cleanup / lifetime
// ================================================================

template<typename T>
concept SystemType = requires(T sys, World& world, float dt) {
    { sys.Update(world, dt) } -> std::same_as<void>;
};

struct SystemEntry {
    std::string                          name;
    int                                  priority = 0;
    bool                                 enabled  = true;
    std::function<void(World&, float)>   update;
};

class SystemScheduler {
public:
    template<SystemType T>
    void Register(T& sys, std::string_view name, int priority = 0) 
    {
        mSystems.push_back({
            std::string(name),
            priority,
            true,
            [&sys](World& w, float dt) { sys.Update(w, dt); }
        });
        mDirty = true;
    }

    // Register a free-function or lambda system
    void RegisterFn(std::string_view name, std::function<void(World&, float)> fn, int priority = 0) 
    {
        mSystems.push_back({std::string(name), priority, true, std::move(fn)});
        mDirty = true;
    }

    void SetEnabled(std::string_view name, bool enabled)
    {
        for (auto& s : mSystems)
            if (s.name == name) { s.enabled = enabled; return; }
    }

    void Update(World& world, float dt) {
        ENT_PROFILE_BEGIN("SystemScheduler::Update");
        if (mDirty) {
            std::stable_sort(mSystems.begin(), mSystems.end(),
                [](const SystemEntry& a, const SystemEntry& b)
                {
                    return a.priority < b.priority; });
            mDirty = false;
        }
        for (auto& sys : mSystems) 
        {
            if (!sys.enabled) continue;
            ENT_PROFILE_BEGIN(sys.name.c_str());
            sys.update(world, dt);
            ENT_PROFILE_END(sys.name.c_str());
        }
        ENT_PROFILE_END("SystemScheduler::Update");
    }

    void PrintOrder() const {
        std::printf("=== System Execution Order ===\n");
        // Print sorted order
        auto sorted = mSystems;
        std::stable_sort(sorted.begin(), sorted.end(),
            [](const SystemEntry& a, const SystemEntry& b) { return a.priority < b.priority; });
        for (size_t i = 0; i < sorted.size(); ++i)
        {
            std::printf("  [%zu] priority=%-5d  %-30s  %s\n",
                i, sorted[i].priority,
                sorted[i].name.c_str(),
                sorted[i].enabled ? "ENABLED" : "DISABLED");
        }
    }

    [[nodiscard]] size_t SystemCount() const noexcept { return mSystems.size(); }

private:
    std::vector<SystemEntry> mSystems;
    bool                     mDirty = false;
};

// ================================================================
//  BUILT-IN EXAMPLE SYSTEMS
//  These demonstrate the system pattern and are also genuinely
//  useful as starting points.
// ================================================================

// ── Common game components (declare before using in systems) ────

struct Position  { float x = 0, y = 0, z = 0; };
struct Velocity  { float x = 0, y = 0, z = 0; };
struct Rotation  { float x = 0, y = 0, z = 0, w = 1; };

struct Health {
    float current = 100.f;
    float max     = 100.f;
    [[nodiscard]] bool IsAlive() const noexcept { return current > 0.f; }
    [[nodiscard]] float Pct()    const noexcept { return current / max; }
    void Damage(float amount)    noexcept { current = std::max(0.f, current - amount); }
    void Heal  (float amount)    noexcept { current = std::min(max,  current + amount); }
};

struct Lifetime {
    float remaining = 1.f;
    [[nodiscard]] bool IsExpired() const noexcept { return remaining <= 0.f; }
};

// Tag components — zero-overhead type markers (ECS uses signature bit only)
struct Disabled     {};   // entity exists but systems skip it
struct PendingKill  {};   // scheduled for deletion this frame

// ── Movement System ─────────────────────────────────────────────

class MovementSystem {
public:
    // Priority 100: runs after input, before rendering
    void Update(World& world, float dt) 
    {
        ENT_PROFILE_BEGIN("MovementSystem");
        mQuery.ForEach([dt](Position& pos, const Velocity& vel) 
            {
            pos.x += vel.x * dt;
            pos.y += vel.y * dt;
            pos.z += vel.z * dt;
        });
        ENT_PROFILE_END("MovementSystem");
    }

    void Init(World& world) { mQuery = CachedQuery<Position, Velocity>(world); }

private:
    CachedQuery<Position, Velocity> mQuery{*reinterpret_cast<World*>(1)}; // placeholder
    // NOTE: Call Init(world) before first Update
};

// ── Health System ────────────────────────────────────────────────
// Marks dead entities for removal via CommandBuffer (safe during iteration)

class HealthSystem {
public:
    void Update(World& world, float dt)
    {
        ENT_PROFILE_BEGIN("HealthSystem");
        (void)dt;
        world.Query<Health>().ForEachWithEntity<Health>(
            [this](Entity e, const Health& h) {
                if (!h.IsAlive()) mCmds.DestroyEntity(e);
            }
        );
        world.FlushCommands(mCmds);
        ENT_PROFILE_END("HealthSystem");
    }

private:
    CommandBuffer mCmds;
};

// ── Lifetime System ──────────────────────────────────────────────
// Counts down timers, destroys expired entities safely

class LifetimeSystem {
public:
    void Update(World& world, float dt)
    {
        ENT_PROFILE_BEGIN("LifetimeSystem");

        // Step 1: tick all timers (structural-change-free, safe inline)
        world.Query<Lifetime>().ForEach<Lifetime>(
            [dt](Lifetime& lt) { lt.remaining -= dt; }
        );

        // Step 2: queue expired entities for removal
        world.Query<Lifetime>().ForEachWithEntity<Lifetime>(
            [this](Entity e, const Lifetime& lt) 
            {
                if (lt.IsExpired()) mCmds.DestroyEntity(e);
            }
        );
        world.FlushCommands(mCmds);
        ENT_PROFILE_END("LifetimeSystem");
    }

private:
    CommandBuffer mCmds;
};

// ── PendingKill System ───────────────────────────────────────────
// Flushes any entity tagged PendingKill

class PendingKillSystem {
public:
    void Update(World& world, float dt)
    {
        (void)dt;
        world.Query<PendingKill>().ForEachWithEntity<PendingKill>(
            [this](Entity e, const PendingKill&) { mCmds.DestroyEntity(e); }
        );
        world.FlushCommands(mCmds);
    }
private:
    CommandBuffer mCmds;
};

// ================================================================
//  WORLD BUILDER
//  Fluent startup API — reduces boilerplate for common setup.
//
//  Usage:
//    auto [world, scheduler] =
//        WorldBuilder{}
//            .RegisterComponents<Position, Velocity, Health>()
//            .AddSystem(moveSys,   "Movement", 100)
//            .AddSystem(healthSys, "Health",   300)
//            .Build();
// ================================================================

class WorldBuilder 
{
public:
    template<typename... Ts>
    WorldBuilder& RegisterComponents()
    {
        (ComponentRegistry::Get().Register<Ts>(), ...);
        return *this;
    }

    template<SystemType T>
    WorldBuilder& AddSystem(T& sys, std::string_view name, int priority = 0)
    {
        mSystemAdders.push_back([&sys, n = std::string(name), priority](SystemScheduler& sched) 
            {
            sched.Register(sys, n, priority);
        });
        return *this;
    }

    struct BuildResult 
    {
        std::unique_ptr<World>          world;
        std::unique_ptr<SystemScheduler> scheduler;
    };

    [[nodiscard]] BuildResult Build() 
    {
        auto w = std::make_unique<World>();
        auto s = std::make_unique<SystemScheduler>();
        for (auto& adder : mSystemAdders) adder(*s);
        return {std::move(w), std::move(s)};
    }

private:
    std::vector<std::function<void(SystemScheduler&)>> mSystemAdders;
};

// ================================================================
//  END
// ================================================================
} 
