#pragma once

#include "skse64/NiGeometry.h"


template <class T>
struct NiTListItem
{
	NiTListItem* next; // 00
	NiTListItem* prev; // 08
	T item; // 10
};
static_assert(sizeof(NiTListItem<void *>) == 0x18);

template <class T>
struct NiTPointerList
{
	NiTListItem<T> *head; // 00
	NiTListItem<T> *tail; // 08
	UInt64 allocator; // 10
};
static_assert(sizeof(NiTPointerList<void *>) == 0x18);


class MagicCaster
{
public:
	enum class State
	{
		// Thank you Noah Boddie for REing this
		kNone = 0,
		kCastStart = 1,
		kCharging = 2,
		kCharged = 3,
		kReleased = 4,
		kConcentrating = 6,
	};

	void *vtbl; // 00
	tArray<UInt64> sounds; // 08
	UInt32 desiredTarget; // 20
	UInt32 pad24; // 24
	MagicItem *currentSpell; // 28
	UInt32 state; // 30
	float castingTimer; // 34
	float currentSpellCost; // 38
	float magnitudeOverride; // 3C
	float nextTargetUpdate; // 40
	float projectileTimer; // 44
};
static_assert(sizeof(MagicCaster) == 0x48);


struct NiParticleInfo
{
public:
	NiPoint3 velocity; // 00
	float age; // 0C - Starts at lifeSpan then ticks down?
	float lifeSpan; // 10 - How long it has to live total
	float lastUpdate; // 14 - Time that it was last updated. Relative to start of particle system?
	UInt32 unk18; // likes to be 0 - maybe generation and code
	UInt16 unk1C; // appears to tick down then reset
	UInt16 unk1E; // some flags or something
};
static_assert(sizeof(NiParticleInfo) == 0x20);

struct NiParticlesData : NiObject
{
	UInt8 unk10[0x48 - 0x10];
	float *unk48; // More values than number of particles...
	NiColorA *colors; // 50
	float *unk58; // 58 - maybe radii
	float *sizes; // 60
	float *unk68; // all negative?
	UInt64 unk70;
	NiRect<float> *subtextureOffsets; // 78 - maybe
	UInt32 unk80;
	NiPoint3 unk84;
	UInt32 unk90;
	UInt16 numParticles; // 94
	UInt8 *textureIndices; // 98 - maybe
	UInt64 unkA0;
};
static_assert(offsetof(NiParticlesData, sizes) == 0x60);
static_assert(offsetof(NiParticlesData, numParticles) == 0x94);
static_assert(sizeof(NiParticlesData) == 0xA8);

struct NiPSysData : NiParticlesData
{
	NiParticleInfo *particleInfos; // A8
	float *unkB0; // maybe rotation speeds
	UInt16 numAddedParticles; // B8
	UInt16 addedParticlesBase; // BC
};
static_assert(offsetof(NiPSysData, particleInfos) == 0xA8);
static_assert(sizeof(NiPSysData) == 0xC0);

struct NiPSysModifier : NiObject
{
	virtual bool Update(UInt64 unk0, NiPSysData *data, UInt64 unk1, UInt64 unk2, UInt64 unk3); // 25

	const char *name; // 10
	UInt32 order = 3000; // 18 - default ORDER_GENERAL
	struct NiParticleSystem *particleSystem = nullptr;
	bool active = true; // 28
};
static_assert(offsetof(NiPSysModifier, particleSystem) == 0x20);
static_assert(sizeof(NiPSysModifier) == 0x30);

struct NiParticles : BSGeometry
{
	NiPSysData *data = nullptr; // 198
	UInt64 unk1A0 = 0;
};
static_assert(sizeof(NiParticles) == 0x1A8);

struct NiParticleSystem : NiParticles
{
	NiTPointerList<NiPointer<NiPSysModifier>> modifierList; // 1A8
	float unk1C0 = 1.f; // emit scale ?? these next two seem to affect how "intense" the effect is or something. The higher this one is, the more intense. The lower the next one, the more intense?
	float unk1C4 = 1.f; // previous emit scale ??
	float size = 1.f; // 1C8
	float age = (std::numeric_limits<float>::min)(); // 1CC - how many seconds has the particle system been active
	UInt16 unk1D0 = 0;
	UInt8 unk1D2 = 1;
};
static_assert(offsetof(NiParticleSystem, size) == 0x1C8);
static_assert(sizeof(NiParticleSystem) == 0x1D8);
