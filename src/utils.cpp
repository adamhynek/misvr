#include "skse64/GameRTTI.h"

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


bool IsDualCasting(Actor *actor)
{
	IAnimationGraphManagerHolder *animGraph = &actor->animGraphHolder;
	UInt64 *vtbl = *((UInt64 **)animGraph);
	static BSFixedString isCastingDualAnimVarName("IsCastingDual");
	bool isDualCasting = false;
	((IAnimationGraphManagerHolder_GetGraphVariableBool)(vtbl[0x12]))(animGraph, isCastingDualAnimVarName, isDualCasting);
	return isDualCasting;
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


void SetParticleScaleDownstream(std::unordered_map<NiAVObject *, float> &nodeScales, NiAVObject *root, float scale)
{
	BSTriShape *geom = root->GetAsBSTriShape();
	if (geom) {
		if (nodeScales.count(geom) == 0) { // Not in the map yet
			nodeScales[geom] = geom->m_localTransform.scale;
		}
		geom->m_localTransform.scale = nodeScales[geom] * scale;
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
				SetParticleScaleDownstream(nodeScales, child, scale);
			}
		}
	}
}

void RestoreParticleScaleDownstream(std::unordered_map<NiAVObject *, float> &nodeScales, NiAVObject *root)
{
	BSTriShape *geom = root->GetAsBSTriShape();
	if (geom && nodeScales.count(geom) != 0) {
		geom->m_localTransform.scale = nodeScales[geom];
		return;
	}

	NiNode *node = root->GetAsNiNode();
	if (node) {
		for (int i = 0; i < node->m_children.m_emptyRunStart; i++) {
			NiAVObject *child = node->m_children.m_data[i];
			if (child) {
				RestoreParticleScaleDownstream(nodeScales, child);
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
