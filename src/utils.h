#pragma once

#include "skse64/NiGeometry.h"
#include "skse64/GameReferences.h"
#include "skse64/PapyrusSpell.h"

#include "RE.h"


typedef bool(*IAnimationGraphManagerHolder_GetGraphVariableInt)(IAnimationGraphManagerHolder *_this, const BSFixedString& a_variableName, SInt32& a_out);
typedef bool(*IAnimationGraphManagerHolder_GetGraphVariableBool)(IAnimationGraphManagerHolder* _this, const BSFixedString& a_variableName, bool& a_out);
typedef bool(*_SpellItem_IsTwoHanded)(SpellItem *_this);

inline UInt64* get_vtbl(void* object) { return *((UInt64**)object); }

template<typename T>
inline T get_vfunc(void* object, UInt64 index) {
	UInt64* vtbl = get_vtbl(object);
	return (T)(vtbl[index]);
}

inline NiTransform InverseTransform(const NiTransform &t) { NiTransform inverse; t.Invert(inverse); return inverse; }
inline float VectorLengthSquared(const NiPoint3 &vec) { return vec.x*vec.x + vec.y*vec.y + vec.z*vec.z; }
inline float VectorLength(const NiPoint3 &vec) { return sqrtf(VectorLengthSquared(vec)); }
inline float lerp(float a, float b, float t) { return a * (1.0f - t) + b * t; }
inline NiPoint3 lerp(const NiPoint3 &a, const NiPoint3 &b, float t) { return a * (1.0f - t) + b * t; }
inline NiPoint3 ForwardVector(const NiMatrix33 &r) { return { r.data[0][1], r.data[1][1], r.data[2][1] }; }
inline float DotProduct(const NiPoint3 &vec1, const NiPoint3 &vec2) { return vec1.x*vec2.x + vec1.y*vec2.y + vec1.z*vec2.z; }
inline NiPoint3 VectorNormalized(const NiPoint3 &vec) { float length = VectorLength(vec); return length > 0.0f ? vec / length : NiPoint3(); }

NiPoint3 CrossProduct(const NiPoint3 &vec1, const NiPoint3 &vec2);
NiPoint3 RotateVectorByAxisAngle(const NiPoint3 &vector, const NiPoint3 &axis, float angle);

void UpdateNodeTransformLocal(NiAVObject *node, const NiTransform &worldTransform);
bool GetAnimVariableBool(Actor *actor, BSFixedString &variableName);
bool IsCastingRight(Actor *actor);
bool IsCastingLeft(Actor *actor);
bool IsDualCasting(Actor *actor);
SpellItem *GetEquippedSpell(Actor *actor, bool isOffhand);

inline MagicCaster * GetMagicCaster(Actor *actor, bool isLeft) { return (MagicCaster *) (isLeft ? actor->unk1A0 : actor->unk1A8); }

inline EffectSetting * GetCostliestEffect(SpellItem *spell)
{
	MagicItem::EffectItem *effectItem = nullptr;
	if (spell && spell->effectItemList.GetNthItem(magicItemUtils::GetCostliestEffectIndex(spell), effectItem)) {
		return effectItem->mgef;
	}
	return nullptr;
}

enum class SpellSkillLevel {
	Novice,
	Apprentice,
	Adept,
	Expert,
	Master,
};
SpellSkillLevel GetEffectSkillLevel(EffectSetting *effect);
bool IsTwoHandedEffectMergeable(EffectSetting *effect);

void SetParticleScaleDownstream(NiAVObject *root, float scale);
