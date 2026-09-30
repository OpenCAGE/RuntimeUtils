#include "LIVE_LINK.h"
#include "LIVE_LINK_SERVER.h"
#include "LIVE_CAMERA.h"
#include "DEBUG_MARKER.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <DirectXMath.h>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace LIVE_LINK;

/*
	Addresses are RVAs in the Steam retail AI.exe. Calling conventions follow what the game's own call
	sites do: a class returned by value comes back through a hidden pointer passed first, and a counted pointer passed by
	value is an allocation the callee releases.
*/
namespace
{
	uintptr_t Address(uintptr_t rva) { return DEVTOOLS_RELATIVE_ADDRESS(rva); }

	// ---- Game globals ----
	uint8_t* EntityManager() { return *reinterpret_cast<uint8_t**>(Address(0x0134ef40)); }  // the entity manager
	uintptr_t PackFileBase() { return *reinterpret_cast<uintptr_t*>(Address(0x0134ef44)); } // the loaded COMMANDS.PAK, which packed offsets count from
	void* StringTable() { return *reinterpret_cast<void**>(Address(0x0134ef78)); }          // the string table

	// The entity manager's fields
	constexpr uint32_t kManagerRootGuid = 0x0C;     // the running level's root composite id
	constexpr uint32_t kManagerTemplates = 0x18;    // the level's composite templates (an array of packed pointers)
	constexpr uint32_t kManagerRoot = 0x44;         // the root entity (the level's top instance)
	constexpr uint32_t kManagerInitialised = 0x105; // set once the entity manager has initialised
	constexpr uint32_t kManagerShuttingDown = 0x106;
	constexpr uint32_t kManagerOffline = 0x108;     // when set, an instance that is building its entities keeps every one
	constexpr uint32_t kManagerTransport = 0xBC;   // the script transport: 1 paused, 2 pause menu, 5 running
	constexpr uint32_t kTransportPaused = 1, kTransportPauseMenu = 2, kTransportRunning = 5;
	constexpr ULONGLONG kTransportSettleMs = 5000; // how long the transport must stay running before edits are taken
	// The transport as last seen on the entity thread, since when, and the last value it had that was not running (what
	// the level's scripts are coming back from)
	uint32_t g_transport = ~0u;
	ULONGLONG g_transportSince = 0;
	uint32_t g_transportStopped = kTransportPaused;

	// An entity's fields: its vtable, its state flags, its owner, then its id and its type
	constexpr uint32_t kEntityState = 0x04;
	constexpr uint32_t kEntityGuid = 0x0C;
	constexpr uint32_t kEntityType = 0x10;
	// An instance's fields
	constexpr uintptr_t kCompositeInstanceVTable = 0x00fe1d2c; // the vtable an instance object starts with (its primary one)
	constexpr uint32_t kInstanceEntities = 0x2C;    // the instance's entities (an array)
	constexpr uint32_t kInstanceTemplate = 0x30;    // the composite template the instance was made from (a packed pointer)

	// An entity's virtual slots (byte offsets into its primary vtable)
	constexpr uint32_t kVFindEntity = 0x0C;      // an instance's lookup of an entity by id: its children, then its aliases
	constexpr uint32_t kVRelease = 0x28;         // releases the entity
	constexpr uint32_t kVRejected = 0x48;        // when this says yes, an instance building its entities releases the entity
	constexpr uint32_t kVGetInterface = 0x70;    // gives the entity's interface
	constexpr uint32_t kVAddEntity = 0x94;       // adds a child to an instance (given the instance, then the child)
	constexpr uint32_t kVRemoveEntity = 0x98;    // removes a child from an instance (given the instance, then the child)
	constexpr uint32_t kVPrimaryZone = 0xD0;     // gives the entity's primary zone
	constexpr uint32_t kVSecondaryZone = 0xD4;   // gives the entity's secondary zone
	constexpr uint32_t kVIsTemplated = 0x128;    // whether the entity was made from a template
	constexpr uint32_t kVMarkedForDelete = 0x12C;// whether the entity is marked for delete
	constexpr uint32_t kVRequiresScript = 0x130; // whether the entity needs scripting
	// The entity interface's virtual slots
	constexpr uint32_t kVFlushCache = 0x2FC;     // drops the entity's cached parameters and links

	// Flags for the record the game makes to go with a call into an entity, as its own callers set them
	constexpr uint32_t kInfoLifecycle = 0x04000000; // an instance initialising / shutting down its entities
	constexpr uint32_t kInfoLiveEdit = 0x20000000;  // a composite template being edited live (as the dev build does)

	// The game's counted allocation: for a single item, data is the object; for an array, count and data are the array.
	struct Allocation
	{
		void** vtable;
		volatile LONG references;
		uint32_t count;
		void* data;
	};

	// ---- Game functions ----
	typedef void(__thiscall* t_memory_ptr_dtor)(Allocation** self);
	auto entity_ptr_dtor = reinterpret_cast<t_memory_ptr_dtor>(Address(0x00198230)); // releases a counted reference to an entity
	auto zone_ptr_dtor = reinterpret_cast<t_memory_ptr_dtor>(Address(0x00005770));   // releases a counted reference to a zone

	typedef Allocation** (__thiscall* t_construct_entity)(void* manager, Allocation** result, Allocation* owner, uint32_t guid, uint32_t type);
	auto construct_entity = reinterpret_cast<t_construct_entity>(Address(0x00567b30));
	typedef Allocation** (__thiscall* t_construct_proxy)(void* manager, Allocation** result, Allocation* owner, const void* proxy);
	auto construct_proxy = reinterpret_cast<t_construct_proxy>(Address(0x00522fc0));

	typedef void** (__thiscall* t_create_info)(void* manager, void** result, Allocation** entity, uint32_t flags, uint32_t unused, const void* data);
	auto create_info = reinterpret_cast<t_create_info>(Address(0x00526ff0));
	typedef void(__thiscall* t_remove_reference)(void* info);
	auto remove_reference = reinterpret_cast<t_remove_reference>(Address(0x005252f0));

	typedef void(__thiscall* t_pause_context)(void* manager, Allocation** entity);
	auto requires_push_pause_context = reinterpret_cast<t_pause_context>(Address(0x0052d310));
	auto requires_pop_pause_context = reinterpret_cast<t_pause_context>(Address(0x0052f0e0));

	typedef bool(__thiscall* t_state_call)(void* state, Allocation** entity, void** info);
	auto state_initialise = reinterpret_cast<t_state_call>(Address(0x0053ac50));
	auto state_validate = reinterpret_cast<t_state_call>(Address(0x00536850));
	auto state_revert = reinterpret_cast<t_state_call>(Address(0x0053a9d0));
	auto state_shutdown = reinterpret_cast<t_state_call>(Address(0x0053aad0));
	auto state_live_edit = reinterpret_cast<t_state_call>(Address(0x0053b0c0));

	typedef void(__thiscall* t_zone_entity)(void* zone, Allocation** zonePtr, Allocation** entity);
	auto add_entity_to_zone = reinterpret_cast<t_zone_entity>(Address(0x005478a0));
	auto remove_entity_from_zone = reinterpret_cast<t_zone_entity>(Address(0x00548330));

	// Calls a method on an entity through a temporary entity: queues the call by adding a trigger to the entity manager
	typedef void(__thiscall* t_call_custom_method)(void* temporaryEntity, Allocation** entity, const uint32_t* method, uint32_t flags);
	auto call_custom_method = reinterpret_cast<t_call_custom_method>(Address(0x005425a0));
	constexpr uint32_t kTemporaryEntityTarget = 0x24; // the entity pointer the queued call builds its trigger from

	typedef void(__thiscall* t_access)(void* access);
	auto access_lock = reinterpret_cast<t_access>(Address(0x0051e1b0));   // takes the entity manager's access lock
	auto access_unlock = reinterpret_cast<t_access>(Address(0x0051e1d0)); // gives the entity manager's access lock back

	typedef void(__thiscall* t_add_to_string_table)(const uint32_t* string);
	auto add_to_string_table = reinterpret_cast<t_add_to_string_table>(Address(0x0051df70));
	typedef const char* (__thiscall* t_string_from_offset)(void* table, uint32_t offset);
	auto string_from_offset = reinterpret_cast<t_string_from_offset>(Address(0x00532160));

	// Variable types, as the entity manager turns a parameter's type guid into its vtable when it loads COMMANDS.PAK
	enum class VariableKind { Bool, Int, Float, String, FilePath, SplineData, Direction, Position, Enum, ShortGuid, Unknown };
	struct VariableType { VariableKind kind; uintptr_t guidRva; uintptr_t vtableRva; const char* name; };
	const VariableType kVariableTypes[] = {
		{ VariableKind::Bool,       0x0134ba70, 0x00fc451c, "bool" },
		{ VariableKind::Int,        0x0134ba74, 0x00fc45bc, "int" },
		{ VariableKind::Float,      0x0134ba78, 0x00fc465c, "float" },
		{ VariableKind::String,     0x0134ba7c, 0x00fc46fc, "String" },
		{ VariableKind::FilePath,   0x0134ba80, 0x00fc479c, "FilePath" },
		{ VariableKind::SplineData, 0x0134ba84, 0x00fc4da4, "SplineData" },
		{ VariableKind::Direction,  0x0134ba88, 0x00fc497c, "Direction" },
		{ VariableKind::Position,   0x0134ba8c, 0x00fc4a1c, "Position" },
		{ VariableKind::Enum,       0x0134ba90, 0x00fc483c, "Enum" },
		{ VariableKind::ShortGuid,  0x0134ba94, 0x00fc48dc, "ShortGuid" },
	};

	// ---- Helpers ----
	template<typename T> T* Packed(uint32_t offset)
	{
		if (offset == 0xFFFFFFFF)
			return nullptr;
		return reinterpret_cast<T*>(static_cast<uintptr_t>(PackFileBase() + offset * 4u));
	}

	void* Object(Allocation* allocation) { return allocation ? allocation->data : nullptr; }

	template<typename F> F Virtual(void* object, uint32_t byteOffset)
	{
		return reinterpret_cast<F>((*reinterpret_cast<void***>(object))[byteOffset / 4]);
	}

	uint32_t EntityGuid(Allocation* entity) { return *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(Object(entity)) + kEntityGuid); }
	uint32_t EntityType(Allocation* entity) { return *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(Object(entity)) + kEntityType); }
	void* EntityState(Allocation* entity) { return static_cast<uint8_t*>(Object(entity)) + kEntityState; }

	// A counted reference held by this code; released on scope exit the way the game releases its own
	struct EntityRef
	{
		Allocation* ptr = nullptr;
		EntityRef() = default;
		explicit EntityRef(Allocation* allocation) : ptr(allocation) { if (ptr) InterlockedIncrement(&ptr->references); }
		// Takes over a reference the game has already counted (a counted pointer it returned)
		static void Adopt(EntityRef& ref, Allocation* allocation) { ref.ptr = allocation; }
		EntityRef(const EntityRef&) = delete;
		EntityRef& operator=(const EntityRef&) = delete;
		~EntityRef() { if (ptr) entity_ptr_dtor(&ptr); }
	};
	struct ZoneRef
	{
		Allocation* ptr = nullptr;
		~ZoneRef() { if (ptr) zone_ptr_dtor(&ptr); }
	};
	struct InfoRef
	{
		void* ptr = nullptr;
		~InfoRef() { if (ptr) remove_reference(ptr); }
	};
	// The entity manager's pause for an entity, set before a live edit and cleared after it, as the dev build does when it
	// edits a composite template live
	struct PauseContext
	{
		uint8_t* manager;
		Allocation* entity;
		PauseContext(uint8_t* m, Allocation* e) : manager(m), entity(e) { requires_push_pause_context(manager, &entity); }
		~PauseContext() { requires_pop_pause_context(manager, &entity); }
	};
	// Held while touching the entity manager's data from outside its own calls
	struct ManagerAccess
	{
		uint8_t storage[8] = {};
		ManagerAccess() { access_lock(storage); }
		~ManagerAccess() { access_unlock(storage); }
	};

	std::string Hex(uint32_t value)
	{
		char buffer[16];
		snprintf(buffer, sizeof(buffer), "%02X-%02X-%02X-%02X", value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF, value >> 24);
		return buffer;
	}

	std::vector<Allocation*> ArrayItems(void* arrayPtr)
	{
		std::vector<Allocation*> items;
		Allocation* array = static_cast<Allocation*>(arrayPtr);
		if (!array || !array->data)
			return items;
		Allocation** data = static_cast<Allocation**>(array->data);
		for (uint32_t i = 0; i < array->count; i++)
			if (data[i])
				items.push_back(data[i]);
		return items;
	}

	CompositeTemplate* FindTemplate(uint32_t guid)
	{
		const PackedArray& templates = *reinterpret_cast<PackedArray*>(EntityManager() + kManagerTemplates);
		const uint32_t* offsets = Packed<uint32_t>(templates.offset);
		if (!offsets)
			return nullptr;
		for (uint32_t i = 0; i < templates.count; i++)
		{
			CompositeTemplate* candidate = Packed<CompositeTemplate>(offsets[i]);
			if (candidate && candidate->reference == guid)
				return candidate;
		}
		return nullptr;
	}

	// A direct child of an instance (not through aliases)
	Allocation* FindChild(Allocation* instance, uint32_t guid)
	{
		void* entities = *reinterpret_cast<void**>(static_cast<uint8_t*>(Object(instance)) + kInstanceEntities);
		for (Allocation* child : ArrayItems(entities))
			if (Object(child) && EntityGuid(child) == guid)
				return child;
		return nullptr;
	}

	// ---- Structured exception guard: a fault inside game code is reported instead of taking the game down ----
	int Filter(unsigned int code, const char* what)
	{
		DevTools::Log("LiveLink: exception 0x%08X during %s", code, what);
		return EXCEPTION_EXECUTE_HANDLER;
	}
	bool Guarded(const char* what, void(*body)(void*), void* context)
	{
		__try
		{
			body(context);
			return true;
		}
		__except (Filter(GetExceptionCode(), what))
		{
			return false;
		}
	}

	// ---- Variables ----
	const VariableType* TypeFromGuid(uint32_t guid)
	{
		for (const VariableType& type : kVariableTypes)
			if (*reinterpret_cast<const uint32_t*>(Address(type.guidRva)) == guid)
				return &type;
		return nullptr;
	}
	const VariableType* TypeFromVTable(uint32_t vtable)
	{
		for (const VariableType& type : kVariableTypes)
			if (Address(type.vtableRva) == vtable)
				return &type;
		return nullptr;
	}

	std::string VariableString(const uint32_t* variable)
	{
		void* table = StringTable();
		const char* text = table ? string_from_offset(table, variable[1]) : nullptr;
		return text ? text : "";
	}

	bool VariablesEqual(const uint32_t* a, const uint32_t* b)
	{
		if (!a || !b)
			return a == b;
		if (a[0] != b[0])
			return false;
		const VariableType* type = TypeFromVTable(a[0]);
		switch (type ? type->kind : VariableKind::Unknown)
		{
		case VariableKind::String:
		case VariableKind::FilePath:
			return VariableString(a) == VariableString(b);
		case VariableKind::Enum:
			return memcmp(a + 1, b + 1, 8) == 0;
		case VariableKind::Direction:
			return memcmp(a + 1, b + 1, 12) == 0;
		case VariableKind::Position:
			return memcmp(a + 1, b + 1, 24) == 0;
		case VariableKind::SplineData:
		{
			if (a[2] != b[2])
				return false;
			const void* pointsA = Packed<void>(a[1]);
			const void* pointsB = Packed<void>(b[1]);
			return a[2] == 0 || (pointsA && pointsB && memcmp(pointsA, pointsB, a[2] * 24) == 0);
		}
		default:
			return a[1] == b[1];
		}
	}

	struct EntityParameter { uint32_t param; uint32_t variable; };
	struct EntityParameterPack { uint32_t path; PackedArray params; };
	struct EntityInitialiserData { uint32_t guid; uint32_t type; };

	std::unordered_map<uint32_t, const EntityParameterPack*> ParameterPacks(const CompositeTemplate* composite)
	{
		std::unordered_map<uint32_t, const EntityParameterPack*> packs;
		const EntityParameterPack* data = Packed<EntityParameterPack>(composite->params.offset);
		for (uint32_t i = 0; data && i < composite->params.count; i++)
			packs[data[i].path] = &data[i];
		return packs;
	}

	// A proxy record (from a composite template's proxy list): its id, the path to what it stands for, and that thing's type
	struct EntityProxy { uint32_t guid; PackedArray path; uint32_t guid2; uint32_t function; };
	static_assert(sizeof(EntityProxy) == 0x14, "EntityProxy layout");

	std::vector<uint32_t> Words(const PackedArray& array)
	{
		const uint32_t* data = Packed<uint32_t>(array.offset);
		return data ? std::vector<uint32_t>(data, data + array.count) : std::vector<uint32_t>();
	}

	std::unordered_map<uint32_t, const EntityProxy*> Proxies(const CompositeTemplate* composite)
	{
		std::unordered_map<uint32_t, const EntityProxy*> proxies;
		const EntityProxy* data = Packed<EntityProxy>(composite->proxies.offset);
		for (uint32_t i = 0; data && i < composite->proxies.count; i++)
			proxies[data[i].guid] = &data[i];
		return proxies;
	}

	bool ProxiesEqual(const EntityProxy* a, const EntityProxy* b)
	{
		return a->guid2 == b->guid2 && a->function == b->function && Words(a->path) == Words(b->path);
	}

	// Each alias's id and the path of entity ids it overrides, in order
	struct AliasRecord { uint32_t guid; PackedArray path; };
	std::map<uint32_t, std::vector<uint32_t>> AliasPaths(const CompositeTemplate* composite)
	{
		std::map<uint32_t, std::vector<uint32_t>> paths;
		const AliasRecord* data = Packed<AliasRecord>(composite->aliases.offset);
		for (uint32_t i = 0; data && i < composite->aliases.count; i++)
			paths[data[i].guid] = Words(data[i].path);
		return paths;
	}

	// The composite's variables (a list in its composite template, which an instance walks to find them): a
	// variable's default is a parameter pack under its id, read through the instance itself - not an entity of it
	struct ConnectorRecord { uint32_t guid; uint32_t type; uint32_t name; };
	static_assert(sizeof(ConnectorRecord) == 12, "ConnectorRecord layout");
	void ConnectorIds(const CompositeTemplate* composite, std::unordered_set<uint32_t>& ids)
	{
		const ConnectorRecord* data = Packed<ConnectorRecord>(composite->connectors.offset);
		for (uint32_t i = 0; data && i < composite->connectors.count; i++)
			ids.insert(data[i].guid);
	}

	// Parameters that do not change what an entity does, and that OpenCAGE adds or retypes when it first opens a level it
	// did not write (so a game running such a PAK would otherwise see them change on every entity at the first push, and
	// every entity would be reset): 'name' (from its name tables) and 'mapping' (a string parameter turned into a material mapping)
	constexpr uint32_t kIgnoredParameters[] = { 0x58EBAC79 /* name */, 0x0EE7FAC6 /* mapping */ };

	std::map<uint32_t, const uint32_t*> PackVariables(const EntityParameterPack* pack)
	{
		std::map<uint32_t, const uint32_t*> variables;
		const EntityParameter* params = pack ? Packed<EntityParameter>(pack->params.offset) : nullptr;
		for (uint32_t i = 0; params && i < pack->params.count; i++)
		{
			bool ignored = false;
			for (uint32_t id : kIgnoredParameters)
				ignored |= params[i].param == id;
			if (!ignored)
				variables[params[i].param] = Packed<uint32_t>(params[i].variable);
		}
		return variables;
	}

	// ---- CAGEAnimation / TriggerSequence records and resource references (layouts as CathodeLib writes them to COMMANDS.PAK) ----
	struct Hasher
	{
		uint64_t value = 14695981039346656037ull; // FNV-1a
		void Word(uint32_t word)
		{
			for (int i = 0; i < 4; i++)
				value = (value ^ ((word >> (8 * i)) & 0xFF)) * 1099511628211ull;
		}
		void Words(const uint32_t* words, uint32_t count)
		{
			for (uint32_t i = 0; i < count; i++)
				Word(words[i]);
		}
	};
	constexpr uint32_t kMaxRecordWords = 1u << 22;

	// count words at a packed offset, or false (nothing there / implausibly many)
	bool WordsAt(uint32_t offset, uint32_t count, const uint32_t*& words)
	{
		words = nullptr;
		if (count == 0)
			return true;
		if (count > kMaxRecordWords)
			return false;
		words = Packed<uint32_t>(offset);
		return words != nullptr;
	}

	// A list of track offsets, each to { min, max, id, keyframes offset, keyframe count }
	void HashTracks(Hasher& hash, uint32_t listOffset, uint32_t count, uint32_t keyframeWords)
	{
		const uint32_t* list;
		hash.Word(count);
		if (!WordsAt(listOffset, count, list))
			return;
		for (uint32_t i = 0; i < count; i++)
		{
			const uint32_t* track = Packed<uint32_t>(list[i]);
			const uint32_t* keys;
			if (!track || !WordsAt(track[3], track[4] * keyframeWords, keys))
				continue;
			hash.Words(track, 3);
			hash.Word(track[4]);
			hash.Words(keys, track[4] * keyframeWords);
		}
	}

	// Entity id -> content hash of its CAGEAnimation record: { id, headers, count, float tracks, count, event tracks, count }
	std::unordered_map<uint32_t, uint64_t> AnimationHashes(const CompositeTemplate* composite)
	{
		std::unordered_map<uint32_t, uint64_t> hashes;
		const uint32_t* list;
		if (!WordsAt(composite->animations.offset, composite->animations.count, list))
			return hashes;
		for (uint32_t i = 0; i < composite->animations.count; i++)
		{
			const uint32_t* record = Packed<uint32_t>(list[i]);
			if (!record)
				continue;
			Hasher hash;
			const uint32_t* headers;
			hash.Word(record[2]);
			if (WordsAt(record[1], record[2] * 8, headers))
			{
				for (uint32_t h = 0; h < record[2]; h++)
				{
					const uint32_t* header = headers + h * 8; // binding, type, track, param, param type, sub param, path, length
					const uint32_t* path;
					hash.Words(header, 6);
					hash.Word(header[7]);
					if (WordsAt(header[6], header[7], path))
						hash.Words(path, header[7]);
				}
			}
			HashTracks(hash, record[3], record[4], 8); // float keys: mode, time, value, tangent in, tangent out
			HashTracks(hash, record[5], record[6], 6); // event keys: mode, time, forward, reverse, track type, duration
			hashes[record[0]] = hash.value;
		}
		return hashes;
	}

	// Entity id -> content hash of its TriggerSequence record: { id, entries, count, methods, count }
	std::unordered_map<uint32_t, uint64_t> SequenceHashes(const CompositeTemplate* composite)
	{
		std::unordered_map<uint32_t, uint64_t> hashes;
		const uint32_t* list;
		if (!WordsAt(composite->sequences.offset, composite->sequences.count, list))
			return hashes;
		for (uint32_t i = 0; i < composite->sequences.count; i++)
		{
			const uint32_t* record = Packed<uint32_t>(list[i]);
			if (!record)
				continue;
			Hasher hash;
			const uint32_t* entries;
			hash.Word(record[2]);
			if (WordsAt(record[1], record[2] * 3, entries))
			{
				for (uint32_t e = 0; e < record[2]; e++)
				{
					const uint32_t* entry = entries + e * 3; // path, length, timing
					const uint32_t* path;
					hash.Word(entry[1]);
					hash.Word(entry[2]);
					if (WordsAt(entry[0], entry[1], path))
						hash.Words(path, entry[1]);
				}
			}
			const uint32_t* methods;
			hash.Word(record[4]);
			if (WordsAt(record[3], record[4] * 3, methods))
				hash.Words(methods, record[4] * 3);
			hashes[record[0]] = hash.value;
		}
		return hashes;
	}

	struct RecordDiff
	{
		const CompositeTemplate* current = nullptr;
		const CompositeTemplate* incoming = nullptr;
		std::vector<uint32_t> changed;  // entities whose animation or sequence record differs (or appeared/went)
		bool resourcesChanged = false;
	};

	void RecordDiffBody(void* context)
	{
		RecordDiff& diff = *static_cast<RecordDiff*>(context);
		for (int kind = 0; kind < 2; kind++)
		{
			const auto before = kind == 0 ? AnimationHashes(diff.current) : SequenceHashes(diff.current);
			const auto after = kind == 0 ? AnimationHashes(diff.incoming) : SequenceHashes(diff.incoming);
			for (const auto& entry : after)
			{
				auto match = before.find(entry.first);
				if (match == before.end() || match->second != entry.second)
					diff.changed.push_back(entry.first);
			}
			for (const auto& entry : before)
				if (!after.count(entry.first))
					diff.changed.push_back(entry.first);
		}

		// Resource references are 10 words each, with no offsets in them
		const PackedArray& a = diff.current->resources;
		const PackedArray& b = diff.incoming->resources;
		const uint32_t* wordsA;
		const uint32_t* wordsB;
		diff.resourcesChanged = a.count != b.count || !WordsAt(a.offset, a.count * 10, wordsA) || !WordsAt(b.offset, b.count * 10, wordsB) ||
			(a.count && memcmp(wordsA, wordsB, a.count * 40) != 0);
	}

	bool PacksEqual(const EntityParameterPack* a, const EntityParameterPack* b)
	{
		const std::map<uint32_t, const uint32_t*> variablesA = PackVariables(a);
		const std::map<uint32_t, const uint32_t*> variablesB = PackVariables(b);
		if (variablesA.size() != variablesB.size())
			return false;
		for (const auto& entry : variablesB)
		{
			auto match = variablesA.find(entry.first);
			if (match == variablesA.end() || !VariablesEqual(match->second, entry.second))
				return false;
		}
		return true;
	}

	// ---- Per-entity operations (each run inside the exception guard) ----
	struct EntityOperation
	{
		Allocation* instance = nullptr;
		uint32_t guid = 0;
		uint32_t type = 0;
		const void* proxy = nullptr; // for a proxy being added: its proxy record in the new template
		Allocation* entity = nullptr; // for a live edit: the entity, when it was found beforehand (the caller holds a reference)
		std::string outcome;
	};

	// Bit 28 of an entity's state flags: set once initialised, clear again after shutdown (what the entity manager tests
	// before calling a method on an entity, and what the game tests when it checks that the record made to go with a call
	// into an entity is still valid)
	bool Initialised(Allocation* entity)
	{
		return ((*reinterpret_cast<const uint32_t*>(EntityState(entity)) >> 28) & 1) != 0;
	}

	// By its vtable: an entity's type being a composite's id is not enough - a proxy to an instance carries that type too
	bool IsCompositeInstance(Allocation* entity)
	{
		void* object = Object(entity);
		return object && *reinterpret_cast<uintptr_t*>(object) == Address(kCompositeInstanceVTable);
	}

	// An entity of the instance by id: a child, or what an alias of the composite points at (the instance's own lookup).
	// Returned with a reference held for the caller.
	Allocation* ResolveEntity(Allocation* instance, uint32_t guid)
	{
		Allocation* child = FindChild(instance, guid);
		if (child)
		{
			InterlockedIncrement(&child->references);
			return child;
		}
		void* instanceObject = Object(instance);
		Allocation* found = nullptr;
		Virtual<Allocation** (__thiscall*)(void*, Allocation**, const uint32_t*)>(instanceObject, kVFindEntity)(instanceObject, &found, &guid);
		return found;
	}

	void FlushEntity(Allocation* entity)
	{
		void* object = Object(entity);
		if (!object)
			return;
		void* entityInterface = Virtual<void* (__thiscall*)(void*)>(object, kVGetInterface)(object);
		if (entityInterface)
			Virtual<void(__thiscall*)(void*, Allocation**)>(entityInterface, kVFlushCache)(entityInterface, &entity);
	}

	// Flushes an entity's cache, reporting (rather than stopping at) an entity it faults on
	bool TryFlushEntity(Allocation* entity)
	{
		__try
		{
			FlushEntity(entity);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
	void FlushOne(Allocation* entity)
	{
		if (!TryFlushEntity(entity))
			DevTools::Log("LiveLink:   could not flush the cache of %s (type %s)", Hex(EntityGuid(entity)).c_str(), Hex(EntityType(entity)).c_str());
	}

	void AddZones(Allocation* instance, Allocation* entity, bool add)
	{
		void* instanceObject = Object(instance);
		auto zoneFn = add ? add_entity_to_zone : remove_entity_from_zone;
		for (uint32_t slot : { kVPrimaryZone, kVSecondaryZone })
		{
			ZoneRef zone;
			Virtual<Allocation** (__thiscall*)(void*, Allocation**, Allocation**)>(instanceObject, slot)(instanceObject, &zone.ptr, &instance);
			if (zone.ptr && Object(zone.ptr))
				zoneFn(Object(zone.ptr), &zone.ptr, &entity);
		}
	}

	// An instance made from a template that has not been spawned yet has no entities: it builds them all
	// from the template when it spawns, so an entity added to it now would be made twice
	bool IsUnspawnedTemplate(Allocation* instance)
	{
		void* object = Object(instance);
		if (!Virtual<bool(__thiscall*)(void*, Allocation**)>(object, kVIsTemplated)(object, &instance))
			return false;
		return ArrayItems(*reinterpret_cast<void**>(static_cast<uint8_t*>(object) + kInstanceEntities)).empty();
	}

	// An instance building its entities, for one entity or proxy: constructed and added to the instance. Initialising
	// comes after every new entity is in (a later step), as an instance builds all its entities, then initialises
	// them.
	void ConstructBody(void* context)
	{
		EntityOperation& op = *static_cast<EntityOperation*>(context);
		uint8_t* manager = EntityManager();
		EntityRef instance(op.instance);
		if (IsUnspawnedTemplate(instance.ptr))
		{
			op.outcome = "skipped (a template instance not spawned yet - it will be built from the new data)";
			return;
		}
		// A retry of one that failed before may have worked in some instances already
		if (FindChild(instance.ptr, op.guid))
		{
			op.outcome = "skipped (already in the instance)";
			return;
		}

		EntityRef entity;
		InterlockedIncrement(&instance.ptr->references); // constructing an entity or proxy takes the owner by value and releases it
		if (op.proxy)
			construct_proxy(manager, &entity.ptr, instance.ptr, op.proxy);
		else
			construct_entity(manager, &entity.ptr, instance.ptr, op.guid, op.type);
		if (!entity.ptr || !Object(entity.ptr))
		{
			op.outcome = op.proxy ? "could not be constructed (does its target exist?)" : "could not be constructed (unknown entity type?)";
			return;
		}
		void* object = Object(entity.ptr);
		if (!op.proxy)
		{
			auto release = Virtual<void(__thiscall*)(void*, Allocation**)>(object, kVRelease);
			// These two are dropped just as a level load drops them (an instance building its entities makes the same
			// checks): not failures
			if (Virtual<bool(__thiscall*)(void*, Allocation**)>(object, kVMarkedForDelete)(object, &entity.ptr))
			{
				release(object, &entity.ptr);
				op.outcome = "skipped as on a level load (marked for delete on construction)";
				return;
			}
			bool keep = manager[kManagerOffline] != 0;
			if (!keep && !Virtual<bool(__thiscall*)(void*)>(object, kVRejected)(object))
				keep = Virtual<bool(__thiscall*)(void*, Allocation**)>(object, kVRequiresScript)(object, &entity.ptr);
			if (!keep)
			{
				release(object, &entity.ptr);
				op.outcome = "skipped as on a level load (not scripted in this build of the game)";
				return;
			}
		}

		void* instanceObject = Object(instance.ptr);
		Virtual<void(__thiscall*)(void*, Allocation**, Allocation**)>(instanceObject, kVAddEntity)(instanceObject, &instance.ptr, &entity.ptr);
		op.entity = entity.ptr; // the reference passes to the caller, which initialises the entity next
		entity.ptr = nullptr;
		op.outcome = "constructed";
	}

	// An instance initialising its entities, for one entity just constructed and added (above)
	void InitialiseAddedBody(void* context)
	{
		EntityOperation& op = *static_cast<EntityOperation*>(context);
		uint8_t* manager = EntityManager();
		EntityRef instance(op.instance);
		EntityRef entity(op.entity);
		PauseContext pause(manager, entity.ptr);
		InfoRef info;
		create_info(manager, &info.ptr, &instance.ptr, kInfoLifecycle, 0, nullptr);
		state_initialise(EntityState(entity.ptr), &entity.ptr, &info.ptr);
		state_validate(EntityState(entity.ptr), &entity.ptr, &info.ptr);
		AddZones(instance.ptr, entity.ptr, true);
		op.outcome = "added";
	}

	// An instance shutting down its entities, for one entity, then its removal from the instance (which releases it)
	void RemoveEntityBody(void* context)
	{
		EntityOperation& op = *static_cast<EntityOperation*>(context);
		uint8_t* manager = EntityManager();
		EntityRef instance(op.instance);
		EntityRef entity(FindChild(instance.ptr, op.guid));
		if (!entity.ptr)
		{
			op.outcome = "was not in the instance";
			return;
		}

		{
			PauseContext pause(manager, entity.ptr);
			InfoRef info;
			create_info(manager, &info.ptr, &instance.ptr, kInfoLifecycle, 0, nullptr);
			AddZones(instance.ptr, entity.ptr, false);
			state_revert(EntityState(entity.ptr), &entity.ptr, &info.ptr);
			state_shutdown(EntityState(entity.ptr), &entity.ptr, &info.ptr);
		}
		void* instanceObject = Object(instance.ptr);
		Virtual<void(__thiscall*)(void*, Allocation**, Allocation**)>(instanceObject, kVRemoveEntity)(instanceObject, &instance.ptr, &entity.ptr);
		op.outcome = "removed";
	}

	// What the dev build does to one entity when a composite template is edited live: flush its cache and live edit it. The
	// id can be an alias's (an override of something inside a nested instance), which edits what it points at.
	void LiveEditBody(void* context)
	{
		EntityOperation& op = *static_cast<EntityOperation*>(context);
		uint8_t* manager = EntityManager();
		EntityRef instance(op.instance);
		EntityRef entity;
		if (op.entity)
			entity.ptr = (InterlockedIncrement(&op.entity->references), op.entity); // resolved before the template changed
		else
			EntityRef::Adopt(entity, ResolveEntity(instance.ptr, op.guid));
		// Not (or no longer) initialised - e.g. inside an instance this same edit took out - is left alone
		if (!entity.ptr || !Object(entity.ptr) || !Initialised(entity.ptr))
		{
			op.outcome = "was not in the instance";
			return;
		}
		PauseContext pause(manager, entity.ptr);
		// For an instance this flushes everything in it too (an instance's cache flush reaches its entities): they re-read the
		// parameters that reach them through its variables
		FlushEntity(entity.ptr);
		InfoRef info;
		create_info(manager, &info.ptr, &entity.ptr, kInfoLiveEdit, 0, nullptr);
		state_live_edit(EntityState(entity.ptr), &entity.ptr, &info.ptr);
		op.outcome = "live edited";
	}

	// Drops the cached parameters and links of the instance - its own (its variables' links) and, through the
	// instance's cache flush, those of every entity in it, nested instances included - which may point at the
	// replaced arrays
	void FlushInstanceBody(void* context)
	{
		EntityOperation& op = *static_cast<EntityOperation*>(context);
		FlushOne(op.instance);
		op.outcome = "flushed";
	}

	struct MethodOperation
	{
		Allocation* entity = nullptr;
		uint32_t method = 0;
	};
	void CallMethodBody(void* context)
	{
		MethodOperation& op = *static_cast<MethodOperation*>(context);
		EntityRef entity(op.entity);
		// The game's call of a method on an entity only reads the entity pointer at +0x24 of the temporary record it is given (copying
		// it, with a reference, into the trigger); a zeroed record with the entity itself there stands in for it
		alignas(8) uint8_t temporaryEntity[0x40] = {};
		*reinterpret_cast<Allocation**>(temporaryEntity + kTemporaryEntityTarget) = entity.ptr;
		call_custom_method(temporaryEntity, &entity.ptr, &op.method, 0);
	}

	// ---- Applying an image ----
	struct ApplyState
	{
		uint32_t compositeGuid = 0;
		const uint8_t* image = nullptr;
		uint32_t imageSize = 0;
		const uint32_t* relocations = nullptr;
		uint32_t relocationCount = 0;

		CompositeTemplate* current = nullptr;
		CompositeTemplate* incoming = nullptr;
		std::string error;
		uint32_t parameters = 0;
	};

	bool InBlock(const void* pointer, uint32_t size, const uint8_t* block, uint32_t blockSize)
	{
		const uint8_t* p = static_cast<const uint8_t*>(pointer);
		return p >= block && p + size <= block + blockSize && p + size >= p;
	}

	// Where images live: a heap of their own, so a small image does not cost the 64 KB a direct allocation from the OS would
	HANDLE ImageHeap()
	{
		static HANDLE heap = HeapCreate(0, 0, 0);
		return heap;
	}

	// Rebases the image's offsets onto the pack file and checks it holds the composite; nothing outside the block is
	// touched, so a block that fails here can be freed
	bool RelocateAndCheck(ApplyState& state, uint8_t* block)
	{
		const uint32_t size = state.imageSize;
		const uint32_t delta = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block) - PackFileBase());
		if (delta % 4 != 0)
		{
			state.error = "the pack file is not word-aligned";
			return false;
		}
		const uint32_t base = delta / 4;
		uint32_t* words = reinterpret_cast<uint32_t*>(block);
		const uint32_t wordCount = size / 4;
		for (uint32_t i = 0; i < state.relocationCount; i++)
		{
			const uint32_t index = state.relocations[i];
			if (index >= wordCount)
			{
				state.error = "a relocation lies outside the image";
				return false;
			}
			uint32_t& word = words[index];
			// A string's offset keeps its high bit ("in the pack file", as the string table reads an offset)
			word = (word & 0x80000000) ? (0x80000000 | ((word + base) & 0x7FFFFFFF)) : word + base;
		}

		// Header: entry points (3 words), then the parameter and composite offset tables
		const uint32_t parameterCount = words[4];
		const uint32_t compositeCount = words[6];
		const uint32_t* parameterTable = Packed<uint32_t>(words[3]);
		const uint32_t* compositeTable = Packed<uint32_t>(words[5]);
		if (compositeCount != 1 || !InBlock(compositeTable, 4, block, size) || parameterCount > wordCount || (parameterCount && !InBlock(parameterTable, parameterCount * 4, block, size)))
		{
			state.error = "the image header is not a single composite";
			return false;
		}
		CompositeTemplate* incoming = Packed<CompositeTemplate>(compositeTable[0]);
		if (!InBlock(incoming, sizeof(CompositeTemplate), block, size) || incoming->reference != state.compositeGuid)
		{
			state.error = "the image does not hold composite " + Hex(state.compositeGuid);
			return false;
		}
		// Every parameter must be one the game can turn into a variable: one left with its type id where the game
		// expects a vtable would crash the first entity to read it
		for (uint32_t i = 0; i < parameterCount; i++)
		{
			const uint32_t* variable = Packed<uint32_t>(parameterTable[i]);
			if (!InBlock(variable, 8, block, size))
			{
				state.error = "a parameter lies outside the image";
				return false;
			}
			if (!TypeFromGuid(variable[0]))
			{
				state.error = "parameter " + std::to_string(i) + " has a type the game does not have (" + Hex(variable[0]) + ")";
				return false;
			}
		}
		state.incoming = incoming;
		state.parameters = parameterCount;
		return true;
	}

	// Copies the image into memory that is never freed once used (entities may keep offsets into it), rebases it, and
	// turns its parameters into variables the way the entity manager does when it loads COMMANDS.PAK
	void LoadImageBody(void* context)
	{
		ApplyState& state = *static_cast<ApplyState*>(context);
		const uint32_t size = state.imageSize;
		if (size < 7 * 4 || size % 4 != 0)
		{
			state.error = "the image is too small or not word-aligned";
			return;
		}
		uint8_t* block = ImageHeap() ? static_cast<uint8_t*>(HeapAlloc(ImageHeap(), 0, size)) : nullptr;
		if (!block)
		{
			state.error = "out of memory";
			return;
		}
		memcpy(block, state.image, size);
		if (!RelocateAndCheck(state, block))
		{
			state.incoming = nullptr;
			HeapFree(ImageHeap(), 0, block);
			return;
		}

		// From here the block is in use: strings in it go into the game's string table
		const uint32_t* words = reinterpret_cast<const uint32_t*>(block);
		const uint32_t* parameterTable = Packed<uint32_t>(words[3]);
		for (uint32_t i = 0; i < state.parameters; i++)
		{
			uint32_t* variable = Packed<uint32_t>(parameterTable[i]);
			const VariableType* type = TypeFromGuid(variable[0]);
			variable[0] = static_cast<uint32_t>(Address(type->vtableRva));
			if (type->kind == VariableKind::String || type->kind == VariableKind::FilePath)
				add_to_string_table(variable + 1);
		}
		state.incoming->debug_name = string_from_offset(StringTable(), state.incoming->template_name);
	}

	// Points the live template at the incoming arrays; its runtime members (instances, shared instance) stay
	void SwapTemplateBody(void* context)
	{
		ApplyState& state = *static_cast<ApplyState*>(context);
		CompositeTemplate* current = state.current;
		const CompositeTemplate* incoming = state.incoming;
		current->template_name = incoming->template_name;
		current->debug_name = incoming->debug_name;
		current->shared_path = incoming->shared_path;
		current->links = incoming->links;
		current->params = incoming->params;
		current->aliases = incoming->aliases;
		current->mappings = incoming->mappings;
		current->connectors = incoming->connectors;
		current->proxies = incoming->proxies;
		current->entities = incoming->entities;
		current->resources = incoming->resources;
		current->animations = incoming->animations;
		current->sequences = incoming->sequences;
		current->begin_xref = incoming->begin_xref;
		current->end_xref = incoming->end_xref;
	}

	void RunOnInstances(const char* what, void(*body)(void*), const std::vector<Allocation*>& instances, uint32_t guid, uint32_t type, const void* proxy, int& done, int& failed)
	{
		for (Allocation* instance : instances)
		{
			EntityOperation op;
			op.instance = instance;
			op.guid = guid;
			op.type = type;
			op.proxy = proxy;
			if (!Guarded(what, body, &op))
			{
				failed++;
				continue;
			}
			if (guid != 0)
				DevTools::Log("LiveLink:   %s %s", Hex(guid).c_str(), op.outcome.c_str());
			if (op.outcome == "added" || op.outcome == "removed" || op.outcome == "live edited" || op.outcome == "flushed")
				done++;
			else if (op.outcome != "was not in the instance") // e.g. an entity this build does not script: nothing to do
				failed++;
		}
	}

	// Finds an entity by id (a child, or an alias's target) and keeps a reference to it in op.entity
	void ResolveBody(void* context)
	{
		EntityOperation& op = *static_cast<EntityOperation*>(context);
		op.entity = ResolveEntity(op.instance, op.guid);
	}

	// The last image applied to each composite, and where it went: the same image pushed again is then skipped
	struct AppliedImage
	{
		uint64_t hash;
		const CompositeTemplate* composite;
		uint32_t entitiesOffset;
		uint32_t paramsOffset;
	};
	std::unordered_map<uint32_t, AppliedImage> g_lastApplied;

	// Entities and proxies of a composite that could not be constructed in some instance (a proxy whose target arrives in
	// a later push, say): the template already has them, so no later diff would - they are tried again on the next apply
	struct FailedConstructs
	{
		const CompositeTemplate* composite;
		std::unordered_set<uint32_t> ids;
	};
	std::unordered_map<uint32_t, FailedConstructs> g_failedConstructs;

	uint64_t ImageHash(const uint8_t* image, uint32_t imageSize, const uint32_t* relocations, uint32_t relocationCount)
	{
		uint64_t hash = 14695981039346656037ull; // FNV-1a
		auto add = [&hash](const uint8_t* bytes, size_t count)
		{
			for (size_t i = 0; i < count; i++)
				hash = (hash ^ bytes[i]) * 1099511628211ull;
		};
		add(image, imageSize);
		add(reinterpret_cast<const uint8_t*>(relocations), relocationCount * sizeof(uint32_t));
		return hash;
	}

	struct LiveEditTarget
	{
		Allocation* instance;
		uint32_t guid;
		Allocation* entity; // referenced, or null when it did not resolve before the change
	};
}

__declspec(noinline)
void __fastcall LIVE_LINK::h_process(void* _this, void* /*_EDX*/)
{
	TrackTransport();
	LIVE_LINK_SERVER::ProcessEntityRequests();
	process(_this);
}

bool LIVE_LINK::RunningLevelIs(uint32_t root)
{
	return root == 0 || *reinterpret_cast<uint32_t*>(EntityManager() + kManagerRootGuid) == root;
}

bool LIVE_LINK::LevelRunning()
{
	uint8_t* manager = EntityManager();
	return manager && manager[kManagerInitialised] && !manager[kManagerShuttingDown] && *reinterpret_cast<Allocation**>(manager + kManagerRoot) && PackFileBase();
}

namespace
{
	// The game's globals: a pointer to them, whose +0x3C points at the level manager and +0x48 at what tracks the game's
	// state (the state itself at +0x08 there: 0 startup ... 4 gameplay ... 7 unloading, named below)
	constexpr uintptr_t kGameGlobals = 0x012F0C88;
	constexpr uint32_t kGlobalsLevelManager = 0x3C;
	constexpr uint32_t kGlobalsGameFlow = 0x48;
	constexpr uint32_t kGameFlowState = 0x08;
	constexpr int kGameStateGameplay = 4;
	const char* const kGameStateNames[] = { "STARTUP", "WSTARTUP", "MENU", "STARTING_GAMEPLAY", "GAMEPLAY", "GAME_OVER", "RESTARTING_GAMEPLAY", "UNLOADING_GAMEPLAY" };

	uint8_t* GameGlobalsMember(uint32_t offset)
	{
		__try
		{
			uint8_t* globals = *reinterpret_cast<uint8_t**>(Address(kGameGlobals));
			return globals ? *reinterpret_cast<uint8_t**>(globals + offset) : nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}
}

int LIVE_LINK::GameState()
{
	uint8_t* flow = GameGlobalsMember(kGlobalsGameFlow);
	if (!flow)
		return -1;
	__try
	{
		return *reinterpret_cast<int*>(flow + kGameFlowState);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return -1;
	}
}

const char* LIVE_LINK::GameStateName(int state)
{
	return state >= 0 && state < static_cast<int>(sizeof(kGameStateNames) / sizeof(kGameStateNames[0])) ? kGameStateNames[state] : "UNKNOWN";
}

void* LIVE_LINK::LevelManager()
{
	return GameGlobalsMember(kGlobalsLevelManager);
}

// Edits and method calls need a level being played. No level, or a level unloading, refuses them at once; a level still
// loading (or over, and about to restart from a checkpoint) holds them instead (see the wait check below).
const char* LIVE_LINK::NotReadyForEdits()
{
	if (!LevelRunning())
		return "No level is running";
	const int state = GameState();
	if (state == 7 /* unloading the level */)
		return "The game is unloading the level - try again once a level is being played";
	return nullptr;
}

// Every entity frame, before the requests: notes when the level's script transport changes, and logs it (so each run's
// log shows the level start's pauses).
void LIVE_LINK::TrackTransport()
{
	if (!LevelRunning())
	{
		g_transport = ~0u; // the next level starts afresh
		g_transportStopped = kTransportPaused;
		return;
	}
	const uint32_t transport = *reinterpret_cast<uint32_t*>(EntityManager() + kManagerTransport);
	if (transport == g_transport)
		return;
	if (g_transport != ~0u && g_transport != kTransportRunning)
		g_transportStopped = g_transport;
	DevTools::Log("LiveLink: level scripts %s (transport %u)", transport == kTransportRunning ? "running" : "not running", transport);
	g_transport = transport;
	g_transportSince = GetTickCount64();
}

// Edits and calls wait while the level's scripts are not running: the entity manager's script transport is paused (1)
// from a level's load through its intro and opening cutscene (the game queues its own triggers then), pause menu (2)
// while the game is paused, and at a breakpoint or single stepping under a debugger - only running (5) takes them, once
// it has lasted kTransportSettleMs (a level start can let its scripts run for a moment between its pauses: seen ~85 s
// into a boot of BSP_TORRENS). A method call or an added entity pushed during a level start was found to be able to
// leave the game on a black loading screen for good; in hours of edits and calls while running, never.
const char* LIVE_LINK::EditsMustWait()
{
	if (!LevelRunning())
		return nullptr; // refused outright by the not-ready check above
	const int state = GameState();
	if (state == 3 || state == 6)
		return "The level is still starting";
	if (state == 5 /* game over: the level stays loaded, and restarts from a checkpoint */)
		return "The game is over (it restarts from a checkpoint) - the level is still starting";
	switch (*reinterpret_cast<uint32_t*>(EntityManager() + kManagerTransport))
	{
	case kTransportRunning:
		if (g_transport == kTransportRunning && GetTickCount64() - g_transportSince >= kTransportSettleMs)
			return nullptr;
		switch (g_transportStopped) // OpenCAGE recognises this wording and sends the edit again later
		{
		case kTransportPaused:
			return "The level is still starting (its scripts only just started running)";
		case kTransportPauseMenu:
			return "The game is paused (pause menu) - it only just closed";
		default:
			return "The level's scripts are stopped (they only just resumed)";
		}
	case kTransportPauseMenu:
		return "The game is paused (pause menu)";
	case kTransportPaused:
		return "The level is still starting (its scripts are paused for the loading, intro and opening cutscene)";
	default:
		return "The level's scripts are stopped (breakpoint or single stepping)";
	}
}

Result LIVE_LINK::ApplyComposite(uint32_t root, uint32_t compositeGuid, const uint8_t* image, uint32_t imageSize, const uint32_t* relocations, uint32_t relocationCount)
{
	Result result;
	if (const char* notReady = NotReadyForEdits())
	{
		result.message = notReady;
		return result;
	}
	if (!RunningLevelIs(root))
	{
		result.message = "The game is running a different level - save, and load this one in the game";
		return result;
	}

	ApplyState state;
	state.compositeGuid = compositeGuid;
	state.image = image;
	state.imageSize = imageSize;
	state.relocations = relocations;
	state.relocationCount = relocationCount;
	state.current = FindTemplate(compositeGuid);
	if (!state.current)
	{
		result.message = "Composite " + Hex(compositeGuid) + " is not in the running level - save and reload the level to add new composites";
		return result;
	}

	ManagerAccess access;

	// The same image again, onto the template it was last applied to and which still holds it: nothing to do (and no
	// memory to spend on it). A level reload gives the template other arrays, so it is applied afresh then.
	const uint64_t hash = ImageHash(image, imageSize, relocations, relocationCount);
	auto last = g_lastApplied.find(compositeGuid);
	if (last != g_lastApplied.end() && last->second.hash == hash && last->second.composite == state.current &&
		last->second.entitiesOffset == state.current->entities.offset && last->second.paramsOffset == state.current->params.offset)
	{
		result.ok = true;
		result.message = "Unchanged since it was last applied";
		return result;
	}

	if (!Guarded("loading the image", LoadImageBody, &state) || !state.incoming)
	{
		result.message = "Could not load the composite: " + (state.error.empty() ? std::string("an exception was raised") : state.error);
		return result;
	}

	// What changed, against what the game is running
	std::unordered_map<uint32_t, uint32_t> oldTypes, newTypes;
	const EntityInitialiserData* oldEntities = Packed<EntityInitialiserData>(state.current->entities.offset);
	for (uint32_t i = 0; oldEntities && i < state.current->entities.count; i++)
		oldTypes[oldEntities[i].guid] = oldEntities[i].type;
	const EntityInitialiserData* newEntities = Packed<EntityInitialiserData>(state.incoming->entities.offset);
	for (uint32_t i = 0; newEntities && i < state.incoming->entities.count; i++)
		newTypes[newEntities[i].guid] = newEntities[i].type;

	std::vector<std::pair<uint32_t, uint32_t>> added, removed;
	for (const auto& entry : oldTypes)
	{
		auto match = newTypes.find(entry.first);
		if (match == newTypes.end() || match->second != entry.second)
			removed.push_back(entry);
	}
	for (const auto& entry : newTypes)
	{
		auto match = oldTypes.find(entry.first);
		if (match == oldTypes.end() || match->second != entry.second)
			added.push_back(entry);
	}

	// Proxies are entities too, built from their own records: one that is new or changed is (re)made
	const auto oldProxies = Proxies(state.current);
	const auto newProxies = Proxies(state.incoming);
	std::vector<uint32_t> removedProxies;
	std::vector<std::pair<uint32_t, const EntityProxy*>> addedProxies;
	for (const auto& entry : oldProxies)
	{
		auto match = newProxies.find(entry.first);
		if (match == newProxies.end() || !ProxiesEqual(entry.second, match->second))
			removedProxies.push_back(entry.first);
	}
	for (const auto& entry : newProxies)
	{
		auto match = oldProxies.find(entry.first);
		if (match == oldProxies.end() || !ProxiesEqual(entry.second, match->second))
			addedProxies.push_back(entry);
	}

	// What could not be constructed last time, if the composite still has it: tried again (in the instances that lack it)
	size_t retried = 0;
	auto failedBefore = g_failedConstructs.find(compositeGuid);
	if (failedBefore != g_failedConstructs.end() && failedBefore->second.composite == state.current)
	{
		for (uint32_t id : failedBefore->second.ids)
		{
			const bool queued = std::any_of(added.begin(), added.end(), [id](const std::pair<uint32_t, uint32_t>& o) { return o.first == id; }) ||
				std::any_of(addedProxies.begin(), addedProxies.end(), [id](const std::pair<uint32_t, const EntityProxy*>& o) { return o.first == id; });
			if (queued)
				continue;
			auto type = newTypes.find(id);
			auto proxy = newProxies.find(id);
			if (type != newTypes.end())
				added.push_back(*type);
			else if (proxy != newProxies.end())
				addedProxies.push_back(*proxy);
			else
				continue;
			retried++;
		}
	}

	// Parameters that changed: on an entity or proxy of the composite, or on an alias (an override of something inside
	// a nested instance, which is what gets edited then). Anything made afresh reads its parameters anyway.
	std::unordered_set<uint32_t> remade;
	for (const auto& entry : added) remade.insert(entry.first);
	for (const auto& entry : addedProxies) remade.insert(entry.first);
	for (const auto& entry : removed) remade.insert(entry.first);
	for (uint32_t proxy : removedProxies) remade.insert(proxy);
	std::vector<uint32_t> modified;
	const auto oldPacks = ParameterPacks(state.current);
	const auto newPacks = ParameterPacks(state.incoming);
	std::unordered_set<uint32_t> packIds;
	for (const auto& entry : oldPacks) packIds.insert(entry.first);
	for (const auto& entry : newPacks) packIds.insert(entry.first);
	for (uint32_t id : packIds)
	{
		if (remade.count(id))
			continue;
		auto oldPack = oldPacks.find(id);
		auto newPack = newPacks.find(id);
		if (!PacksEqual(oldPack == oldPacks.end() ? nullptr : oldPack->second, newPack == newPacks.end() ? nullptr : newPack->second))
			modified.push_back(id);
	}

	// CAGEAnimation and TriggerSequence data is bound when an entity initialises or is live edited, not read afresh:
	// one whose record changed (a keyframe, a sequence entry) is live edited too. Resources are built with an instance.
	RecordDiff records;
	records.current = state.current;
	records.incoming = state.incoming;
	if (!Guarded("comparing animation data", RecordDiffBody, &records))
		DevTools::Log("LiveLink: could not compare the CAGEAnimation/TriggerSequence data - those entities are not live edited");
	for (uint32_t id : records.changed)
	{
		const bool exists = newTypes.count(id) || newProxies.count(id);
		if (exists && !remade.count(id) && std::find(modified.begin(), modified.end(), id) == modified.end())
			modified.push_back(id);
	}

	// An alias pointed somewhere else (same id, new path - a refactor or an undo re-paths aliases in place): its override
	// has to come off what it pointed at and land on what it points at now, even when its values did not change
	const auto oldAliases = AliasPaths(state.current);
	const auto newAliases = AliasPaths(state.incoming);
	const bool aliasesChanged = oldAliases != newAliases;
	std::unordered_set<uint32_t> repointed;
	for (const auto& entry : newAliases)
	{
		auto match = oldAliases.find(entry.first);
		if (match != oldAliases.end() && match->second != entry.second)
		{
			repointed.insert(entry.first);
			if (std::find(modified.begin(), modified.end(), entry.first) == modified.end())
				modified.push_back(entry.first);
		}
	}

	// A variable's default is read through the instance: a changed one live edits the instance itself, whose own live
	// edit handling live edits or refreshes everything in it
	std::unordered_set<uint32_t> variables;
	ConnectorIds(state.current, variables);
	ConnectorIds(state.incoming, variables);

	const std::vector<Allocation*> instanceList = ArrayItems(state.current->instances);
	std::vector<EntityRef*> held;
	for (Allocation* instance : instanceList)
		held.push_back(new EntityRef(instance)); // keep the instances alive across the edit
	DevTools::Log("LiveLink: applying %s (%s): %u parameters, %zu instance(s), +%zu -%zu ~%zu entities, +%zu -%zu proxies%s",
		state.current->debug_name ? state.current->debug_name : "?", Hex(compositeGuid).c_str(), state.parameters, instanceList.size(),
		added.size(), removed.size(), modified.size(), addedProxies.size(), removedProxies.size(), aliasesChanged ? ", aliases changed" : "");

	// What the changed parameters belong to, found while the template still has the aliases the game resolves them by
	// (an override taken away must still reach what it overrode); new aliases are resolved once they are in
	std::vector<LiveEditTarget> targets;
	std::unordered_set<Allocation*> instancesEdited;
	for (uint32_t guid : modified)
	{
		for (Allocation* instance : instanceList)
		{
			if (variables.count(guid))
			{
				if (instancesEdited.insert(instance).second)
				{
					InterlockedIncrement(&instance->references);
					targets.push_back({ instance, guid, instance });
				}
				continue;
			}
			EntityOperation op;
			op.instance = instance;
			op.guid = guid;
			Guarded("finding an entity", ResolveBody, &op);
			targets.push_back({ instance, guid, op.entity });
			if (repointed.count(guid))
				targets.push_back({ instance, guid, nullptr }); // and what it points at now, found after the swap
		}
	}

	int done = 0, failed = 0;
	for (const auto& entity : removed)
		RunOnInstances("removing an entity", RemoveEntityBody, instanceList, entity.first, entity.second, nullptr, done, failed);
	for (uint32_t proxy : removedProxies)
		RunOnInstances("removing a proxy", RemoveEntityBody, instanceList, proxy, 0, nullptr, done, failed);
	auto release = [&]()
	{
		for (LiveEditTarget& target : targets)
		{
			EntityRef ref;
			EntityRef::Adopt(ref, target.entity);
			target.entity = nullptr;
		}
		for (EntityRef* ref : held)
			delete ref;
		held.clear();
	};
	if (!Guarded("swapping the template", SwapTemplateBody, &state))
	{
		result.message = "Could not swap the composite's data in (an exception was raised) - reload the level";
		release();
		return result;
	}
	// New entities and proxies: all constructed and added first, then all initialised - so one that finds a sibling by
	// id while initialising (a TriggerSequence and what it drives) finds it
	std::vector<EntityOperation> constructed;
	std::unordered_set<uint32_t> constructFailed;
	size_t skippedAsOnLoad = 0;
	auto construct = [&](uint32_t guid, uint32_t type, const void* proxy)
	{
		for (Allocation* instance : instanceList)
		{
			EntityOperation op;
			op.instance = instance;
			op.guid = guid;
			op.type = type;
			op.proxy = proxy;
			if (!Guarded(proxy ? "constructing a proxy" : "constructing an entity", ConstructBody, &op))
			{
				failed++;
				constructFailed.insert(guid);
				continue;
			}
			if (op.outcome != "constructed")
			{
				DevTools::Log("LiveLink:   %s %s", Hex(guid).c_str(), op.outcome.c_str());
				if (op.outcome.rfind("skipped as on a level load", 0) == 0)
					skippedAsOnLoad++;
				else if (op.outcome.rfind("skipped", 0) != 0)
				{
					failed++;
					constructFailed.insert(guid);
				}
				continue;
			}
			constructed.push_back(op);
		}
	};
	for (const auto& entity : added)
		construct(entity.first, entity.second, nullptr);
	for (const auto& proxy : addedProxies)
		construct(proxy.first, 0, proxy.second);
	for (EntityOperation& op : constructed)
	{
		if (!Guarded("initialising an entity", InitialiseAddedBody, &op))
			failed++;
		else
		{
			DevTools::Log("LiveLink:   %s %s", Hex(op.guid).c_str(), op.outcome.c_str());
			done++;
		}
		EntityRef ref;
		EntityRef::Adopt(ref, op.entity); // the reference the construction handed over
		op.entity = nullptr;
	}

	int flushed = 0, flushFailed = 0;
	RunOnInstances("flushing caches", FlushInstanceBody, instanceList, 0, 0, nullptr, flushed, flushFailed);
	for (const LiveEditTarget& target : targets)
	{
		EntityOperation op;
		op.instance = target.instance;
		op.guid = target.guid;
		op.entity = target.entity;
		if (!Guarded("live editing an entity", LiveEditBody, &op))
		{
			failed++;
			continue;
		}
		DevTools::Log("LiveLink:   %s %s", Hex(target.guid).c_str(), op.outcome.c_str());
		if (op.outcome == "live edited")
			done++;
	}

	release();

	if (constructFailed.empty())
		g_failedConstructs.erase(compositeGuid);
	else
		g_failedConstructs[compositeGuid] = { state.current, constructFailed };

	char summary[256];
	snprintf(summary, sizeof(summary), "Applied to %zu instance(s): %zu added, %zu removed, %zu modified (%d entity operations done, %d not)",
		instanceList.size(), added.size() + addedProxies.size() - retried, removed.size() + removedProxies.size(), modified.size(), done, failed);
	result.ok = failed == 0 && flushFailed == 0;
	result.message = summary;
	if (retried)
		result.message += ". Retried " + std::to_string(retried) + " that could not be constructed before";
	if (skippedAsOnLoad)
		result.message += ". " + std::to_string(skippedAsOnLoad) + " not built, as a level load would not build them (marked for delete, or not scripted in this build)";
	if (!constructFailed.empty())
		result.message += ". " + std::to_string(constructFailed.size()) + " could not be constructed - they are tried again on the next push of this composite";
	// Only a clean apply is remembered: the same image again after a failure is applied again, not answered 'unchanged'
	if (result.ok)
		g_lastApplied[compositeGuid] = { hash, state.current, state.current->entities.offset, state.current->params.offset };
	else
		g_lastApplied.erase(compositeGuid);
	if (records.resourcesChanged)
		result.message += ". Its resource references changed (models, collision and the like): those need the level saved and reloaded";
	DevTools::Log("LiveLink: %s", summary);
	return result;
}

Result LIVE_LINK::CallMethod(uint32_t root, uint32_t compositeGuid, uint32_t entityGuid, uint32_t methodGuid, const std::vector<uint32_t>& path)
{
	Result result;
	if (const char* notReady = NotReadyForEdits())
	{
		result.message = notReady;
		return result;
	}
	if (!RunningLevelIs(root))
	{
		result.message = "The game is running a different level - save, and load this one in the game";
		return result;
	}
	ManagerAccess access;

	std::vector<Allocation*> instances;
	if (!path.empty())
	{
		Allocation* instance = *reinterpret_cast<Allocation**>(EntityManager() + kManagerRoot);
		for (uint32_t step : path)
		{
			instance = instance ? FindChild(instance, step) : nullptr;
			if (!instance || !IsCompositeInstance(instance))
			{
				result.message = "The instance path does not resolve in the running level (at " + Hex(step) + ")";
				return result;
			}
		}
		if (instance && FindTemplate(compositeGuid) && *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(Object(instance)) + kInstanceTemplate) != 0 &&
			Packed<CompositeTemplate>(*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(Object(instance)) + kInstanceTemplate)) != FindTemplate(compositeGuid))
		{
			result.message = "The instance at that path is not an instance of composite " + Hex(compositeGuid);
			return result;
		}
		instances.push_back(instance);
	}
	else
	{
		CompositeTemplate* composite = FindTemplate(compositeGuid);
		if (!composite)
		{
			result.message = "Composite " + Hex(compositeGuid) + " is not in the running level";
			return result;
		}
		instances = ArrayItems(composite->instances);
	}

	int called = 0, failed = 0;
	for (Allocation* instance : instances)
	{
		EntityOperation find;
		find.instance = instance;
		find.guid = entityGuid;
		Guarded("finding an entity", ResolveBody, &find);
		EntityRef entity;
		EntityRef::Adopt(entity, find.entity);
		if (!entity.ptr || !Object(entity.ptr))
			continue;
		MethodOperation op;
		op.entity = entity.ptr;
		op.method = methodGuid;
		if (Guarded("calling a method", CallMethodBody, &op))
			called++;
		else
			failed++;
	}

	char summary[160];
	snprintf(summary, sizeof(summary), "Called %s on %s in %d of %zu instance(s)", Hex(methodGuid).c_str(), Hex(entityGuid).c_str(), called, instances.size());
	result.ok = called > 0 && failed == 0;
	result.message = called == 0 && failed == 0 ? std::string("The entity is not in any running instance of the composite") : std::string(summary);
	DevTools::Log("LiveLink: %s", result.message.c_str());
	return result;
}

Result LIVE_LINK::Describe(uint32_t compositeGuid)
{
	Result result;
	if (!LevelRunning())
	{
		result.message = "No level is running";
		return result;
	}
	ManagerAccess access;
	CompositeTemplate* composite = FindTemplate(compositeGuid);
	if (!composite)
	{
		result.message = "Composite " + Hex(compositeGuid) + " is not in the running level";
		return result;
	}

	std::string text = std::string(composite->debug_name ? composite->debug_name : "?") + ": " + std::to_string(composite->entities.count) + " entities, "
		+ std::to_string(composite->params.count) + " parameterised, " + std::to_string(composite->links.count) + " linked";
	const std::vector<Allocation*> instances = ArrayItems(composite->instances);
	text += "; " + std::to_string(instances.size()) + " instance(s)";
	for (Allocation* instance : instances)
	{
		void* entities = *reinterpret_cast<void**>(static_cast<uint8_t*>(Object(instance)) + kInstanceEntities);
		const std::vector<Allocation*> children = ArrayItems(entities);
		text += "\n  instance " + Hex(EntityGuid(instance)) + ": " + std::to_string(children.size()) + " entities";
		for (Allocation* child : children)
			text += "\n    " + Hex(EntityGuid(child)) + " type " + Hex(EntityType(child)) + " state " + Hex(*reinterpret_cast<uint32_t*>(EntityState(child)));
	}
	result.ok = true;
	result.message = text;
	return result;
}

namespace
{
	// Where the game camera is and which way it looks: the inverse of the view matrix the debug markers project with
	// (see DEBUG_MARKER.cpp). An editor can put new things in front of the player with it.
	struct CameraReading
	{
		float position[3] = {};
		float forward[3] = {};
		float up[3] = {};
		bool ok = false;
	};
	void ReadCameraBody(void* context)
	{
		CameraReading& camera = *static_cast<CameraReading*>(context);
		void* owner = *reinterpret_cast<void**>(DEBUG_MARKER::engine + DEBUG_MARKER::kEngineCameraOffset);
		if (!owner)
			return;
		const float* view = *reinterpret_cast<const float**>(static_cast<char*>(owner) + DEBUG_MARKER::kCameraMatricesOffset);
		if (!view)
			return;
		DirectX::XMFLOAT4X4 stored;
		memcpy(&stored, view, sizeof(stored));
		DirectX::XMVECTOR determinant;
		DirectX::XMFLOAT4X4 inverse;
		DirectX::XMStoreFloat4x4(&inverse, DirectX::XMMatrixInverse(&determinant, DirectX::XMLoadFloat4x4(&stored)));
		if (DirectX::XMVectorGetX(determinant) == 0.0f)
			return;
		// Row vectors (D3D) keep the translation in the last row and nothing in the last column; column vectors the reverse
		const bool rows = fabsf(stored._14) + fabsf(stored._24) + fabsf(stored._34) < 1e-4f;
		const float position[3] = { rows ? inverse._41 : inverse._14, rows ? inverse._42 : inverse._24, rows ? inverse._43 : inverse._34 };
		const float forward[3] = { rows ? inverse._31 : inverse._13, rows ? inverse._32 : inverse._23, rows ? inverse._33 : inverse._33 };
		const float up[3] = { rows ? inverse._21 : inverse._12, rows ? inverse._22 : inverse._22, rows ? inverse._23 : inverse._32 };
		memcpy(camera.position, position, sizeof(position));
		memcpy(camera.forward, forward, sizeof(forward));
		memcpy(camera.up, up, sizeof(up));
		camera.ok = true;
	}
	std::string Triple(const float v[3])
	{
		char text[96];
		snprintf(text, sizeof(text), "%.3f,%.3f,%.3f", v[0], v[1], v[2]);
		return text;
	}
}

Result LIVE_LINK::Status()
{
	Result result;
	ManagerAccess access;
	uint8_t* manager = EntityManager();
	// Whether the game is rendering from the camera OpenCAGE sends (the CAMERA command, applied in LIVE_CAMERA.cpp)
	const std::string cameraSync = std::string("\ncamera_sync=") + (LIVE_CAMERA::Applying() ? "1" : "0");
	if (!LevelRunning())
	{
		result.ok = true;
		result.message = "running=0" + cameraSync;
		return result;
	}
	const uint32_t rootGuid = *reinterpret_cast<uint32_t*>(manager + kManagerRootGuid);
	CompositeTemplate* root = FindTemplate(rootGuid);
	const PackedArray& templates = *reinterpret_cast<PackedArray*>(manager + kManagerTemplates);
	result.ok = true;
	result.message = "running=1\nroot=" + Hex(rootGuid) + "\nroot_name=" + std::string(root && root->debug_name ? root->debug_name : "") +
		"\ntemplates=" + std::to_string(templates.count);
	// Edits and calls are only taken while a level is being played and its scripts have settled running
	result.message += std::string("\nstate=") + GameStateName(GameState()) + "\nplaying=" + (NotReadyForEdits() || EditsMustWait() ? "0" : "1");
	{
		// Edits and calls are held while this says 1 (a level starting, the game paused or over, or its scripts stopped)
		const char* loading = EditsMustWait();
		result.message += std::string("\nloading=") + (loading ? "1\nloading_reason=" + std::string(loading) : "0");
	}
	CameraReading camera;
	if (Guarded("reading the camera", ReadCameraBody, &camera) && camera.ok)
		result.message += "\ncamera=" + Triple(camera.position) + "\ncamera_forward=" + Triple(camera.forward) + "\ncamera_up=" + Triple(camera.up);
	result.message += cameraSync;
	return result;
}
