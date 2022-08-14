#include "skse64/GameRTTI.h"
#include "skse64/PapyrusSpell.h"

#include "utils.h"
#include "RE.h"


NiTransform GetLocalTransform(NiAVObject *node, const NiTransform &worldTransform)
{
	NiPointer<NiNode> parent = node->m_parent;
	if (parent) {
		NiTransform inverseParent = InverseTransform(node->m_parent->m_worldTransform);
		return inverseParent * worldTransform;
	}
	return worldTransform;
}

void UpdateNodeTransformLocal(NiAVObject *node, const NiTransform &worldTransform)
{
	// Given world transform, set the necessary local transform
	node->m_localTransform = GetLocalTransform(node, worldTransform);
}

bool GetAnimVariableBool(Actor *actor, BSFixedString &variableName)
{
	IAnimationGraphManagerHolder *animGraph = &actor->animGraphHolder;
	UInt64 *vtbl = *((UInt64 **)animGraph);
	bool var = false;
	((IAnimationGraphManagerHolder_GetGraphVariableBool)(vtbl[0x12]))(animGraph, variableName, var);
	return var;
}

bool IsDualCasting(Actor *actor)
{
	static BSFixedString animVarName("IsCastingDual");
	return GetAnimVariableBool(actor, animVarName);
}

bool IsCastingRight(Actor *actor)
{
	static BSFixedString animVarName("IsCastingRight");
	return GetAnimVariableBool(actor, animVarName);
}

bool IsCastingLeft(Actor *actor)
{
	static BSFixedString animVarName("IsCastingLeft");
	return GetAnimVariableBool(actor, animVarName);
}

NiPoint3 CrossProduct(const NiPoint3 &vec1, const NiPoint3 &vec2)
{
	NiPoint3 result;
	result.x = vec1.y * vec2.z - vec1.z * vec2.y;
	result.y = vec1.z * vec2.x - vec1.x * vec2.z;
	result.z = vec1.x * vec2.y - vec1.y * vec2.x;
	return result;
}

NiPoint3 RotateVectorByAxisAngle(const NiPoint3 &vector, const NiPoint3 &axis, float angle)
{
	// Rodrigues' rotation formula
	float cosTheta = cosf(angle);
	return vector * cosTheta + (CrossProduct(axis, vector) * sinf(angle)) + axis * DotProduct(axis, vector) * (1.0f - cosTheta);
}


void SetParticleScaleDownstream(NiAVObject *root, float scale)
{
	BSTriShape *geom = root->GetAsBSTriShape();
	if (geom) {
		// Normally, we'd need to set the local transform and update,
		// but as long as this is called after any update calls to this node, we can set the world transform directly.
		geom->m_worldTransform.scale *= scale;
		return;
	}

	NiParticleSystem *particles = DYNAMIC_CAST(root, NiAVObject, NiParticleSystem);
	if (particles) {
		particles->size *= scale;
		return;
	}

	NiNode *node = root->GetAsNiNode();
	if (node) {
		for (int i = 0; i < node->m_children.m_emptyRunStart; i++) {
			NiAVObject *child = node->m_children.m_data[i];
			if (child) {
				SetParticleScaleDownstream(child, scale);
			}
		}
	}
}

SpellItem *GetEquippedSpell(Actor *actor, bool isOffhand)
{
	TESForm *form = actor->GetEquippedObject(isOffhand);
	if (form) {
		return DYNAMIC_CAST(form, TESForm, SpellItem);
	}
	return nullptr;
}

SpellSkillLevel GetSpellSkillLevel(SpellItem* spell)
{
	MagicItem::EffectItem* effectItem = nullptr;
	if (spell->effectItemList.GetNthItem(magicItemUtils::GetCostliestEffectIndex(spell), effectItem)) {
		if (EffectSetting* effect = effectItem->mgef) {
			UInt32 skillLevel = effect->properties.level;
			if (skillLevel >= 100) {
				return SpellSkillLevel::Master;
			}
			if (skillLevel >= 75) {
				return SpellSkillLevel::Expert;
			}
			if (skillLevel >= 50) {
				return SpellSkillLevel::Adept;
			}
			if (skillLevel >= 25) {
				return SpellSkillLevel::Apprentice;
			}
		}
	}

	return SpellSkillLevel::Novice;
}
