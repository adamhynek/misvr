#include "common/IDebugLog.h"  // IDebugLog
#include "common/ITimer.h"
#include "skse64_common/skse_version.h"  // RUNTIME_VERSION
#include "skse64/PluginAPI.h"  // SKSEInterface, PluginInfo
#include "xbyak/xbyak.h"
#include "skse64_common/BranchTrampoline.h"
#include "skse64/PapyrusEvents.h"
#include "skse64/GameData.h"
#include "skse64_common/SafeWrite.h"
#include "skse64/NiNodes.h"
#include "skse64/NiGeometry.h"

#include <ShlObj.h>  // CSIDL_MYDOCUMENTS

#include "version.h"

// SKSE globals
static PluginHandle	g_pluginHandle = kPluginHandle_Invalid;
static SKSEMessagingInterface *g_messaging = nullptr;

SKSEVRInterface *g_vrInterface = nullptr;
SKSETrampolineInterface *g_trampoline = nullptr;

// Hook stuff
uintptr_t postMagicNodeUpdateHookedFuncAddr = 0;
uintptr_t postPCUpdateHookedFuncAddr = 0;

auto postMagicNodeUpdateHookLoc = RelocAddr<uintptr_t>(0x6AC035);
auto postMagicNodeUpdateHookedFunc = RelocAddr<uintptr_t>(0x398940);

auto postPlayerCharacterVRUpdateHookLoc = RelocAddr<uintptr_t>(0x6C6A18);
auto postPlayerCharacterVRUpdateHookedFunc = RelocAddr<uintptr_t>(0xC9BC10);


typedef bool(*IAnimationGraphManagerHolder_GetGraphVariableInt)(IAnimationGraphManagerHolder *_this, const BSFixedString& a_variableName, SInt32& a_out);
typedef bool(*IAnimationGraphManagerHolder_GetGraphVariableBool)(IAnimationGraphManagerHolder *_this, const BSFixedString& a_variableName, bool& a_out);

typedef NiMatrix33 * (*_MatrixFromForwardVector)(NiMatrix33 *matOut, NiPoint3 *forward, NiPoint3 *world);
RelocAddr<_MatrixFromForwardVector> MatrixFromForwardVector(0xC4C1E0);

inline NiTransform InverseTransform(const NiTransform &t) { NiTransform inverse; t.Invert(inverse); return inverse; }
inline float VectorLengthSquared(const NiPoint3 &vec) { return vec.x*vec.x + vec.y*vec.y + vec.z*vec.z; }
inline float VectorLength(const NiPoint3 &vec) { return sqrtf(VectorLengthSquared(vec)); }
inline float lerp(float a, float b, float t) { return a * (1.0f - t) + b * t; }
inline NiPoint3 lerp(const NiPoint3 &a, const NiPoint3 &b, float t) { return a * (1.0f - t) + b * t; }
inline NiPoint3 ForwardVector(const NiMatrix33 &r) { return { r.data[0][1], r.data[1][1], r.data[2][1] }; }
inline float DotProduct(const NiPoint3 &vec1, const NiPoint3 &vec2) { return vec1.x*vec2.x + vec1.y*vec2.y + vec1.z*vec2.z; }
inline NiPoint3 VectorNormalized(const NiPoint3 &vec) { float length = VectorLength(vec); return length > 0.0f ? vec / length : NiPoint3(); }


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

void SetGeometryScaleDownstream(std::unordered_map<NiAVObject *, float> &nodeScales, NiAVObject *root, float scale)
{
	BSGeometry *geom = root->GetAsBSTriShape();
	if (geom && nodeScales.count(geom) != 0) {
		geom->m_localTransform.scale = nodeScales[geom] * scale;
	}

	NiNode *node = root->GetAsNiNode();
	if (node) {
		for (int i = 0; i < node->m_children.m_emptyRunStart; i++) {
			NiAVObject *child = node->m_children.m_data[i];
			if (child) {
				SetGeometryScaleDownstream(nodeScales, child, scale);
			}
		}
	}
}

void SaveGeometryScaleDownstream(std::unordered_map<NiAVObject *, float> &nodeScales, NiAVObject *root)
{
	BSGeometry *geom = root->GetAsBSTriShape();
	if (geom) {
		nodeScales[geom] = geom->m_localTransform.scale;
	}

	NiNode *node = root->GetAsNiNode();
	if (node) {
		for (int i = 0; i < node->m_children.m_emptyRunStart; i++) {
			NiAVObject *child = node->m_children.m_data[i];
			if (child) {
				SaveGeometryScaleDownstream(nodeScales, child);
			}
		}
	}
}

void RestoreGeometryScaleDownstream(std::unordered_map<NiAVObject *, float> &nodeScales, NiAVObject *root)
{
	BSGeometry *geom = root->GetAsBSTriShape();
	if (geom && nodeScales.count(geom) != 0) {
		geom->m_localTransform.scale = nodeScales[geom];
	}

	NiNode *node = root->GetAsNiNode();
	if (node) {
		for (int i = 0; i < node->m_children.m_emptyRunStart; i++) {
			NiAVObject *child = node->m_children.m_data[i];
			if (child) {
				RestoreGeometryScaleDownstream(nodeScales, child);
			}
		}
	}
}

enum class DualCastState {
	Idle,
	Cast
};
DualCastState state = DualCastState::Idle;

struct SavedState {
	NiTransform secondaryMagicOffsetNodeLocalTransform;
	NiTransform secondaryMagicAimNodeLocalTransform;
	float currentDualCastScale;
};
SavedState savedState;

std::unordered_map<NiAVObject *, float> primaryNodeScales;
std::unordered_map<NiAVObject *, float> secondaryNodeScales;


// TODO:
// - Do node scales based on magicka percentage
// - Add config options for things: left/right/combined hand vectors for dualcast spell direction, min/max scale while dual casting along with hand-hand distance to scale over.

void PostMagicNodeUpdateHook()
{
	// Do state updates + pos/rot updates in this hook right after the magic nodes get updated, but before vrik so that vrik can apply head bobbing on top.

	PlayerCharacter *player = *g_thePlayer;
	if (!player->GetNiNode()) return;

	bool isDualCasting = IsDualCasting(player);
	NiPointer<NiAVObject> secondaryMagicAimNode = player->unk3F0[PlayerCharacter::Node::kNode_SecondaryMagicAimNode];
	NiPointer<NiAVObject> primaryMagicAimNode = player->unk3F0[PlayerCharacter::Node::kNode_PrimaryMagicAimNode];
	NiPointer<NiAVObject> secondaryMagicOffsetNode = player->unk3F0[PlayerCharacter::Node::kNode_SecondaryMagicOffsetNode];
	NiPointer<NiAVObject> primaryMagicOffsetNode = player->unk3F0[PlayerCharacter::Node::kNode_PrimaryMagicOffsetNode];

	if (!secondaryMagicOffsetNode || !primaryMagicOffsetNode || !secondaryMagicAimNode || !primaryMagicAimNode) return;

	NiPoint3 midpoint = lerp(secondaryMagicOffsetNode->m_worldTransform.pos, primaryMagicOffsetNode->m_worldTransform.pos, 0.5f);

	if (state == DualCastState::Idle) {
		if (isDualCasting) {//if (hasStartedDualCasting) {
			savedState.secondaryMagicAimNodeLocalTransform = secondaryMagicAimNode->m_localTransform;
			savedState.secondaryMagicOffsetNodeLocalTransform = secondaryMagicOffsetNode->m_localTransform;

			secondaryNodeScales.clear();
			SaveGeometryScaleDownstream(secondaryNodeScales, secondaryMagicOffsetNode);

			savedState.currentDualCastScale = 1.f;

			state = DualCastState::Cast;
		}
	}
	if (state == DualCastState::Cast) {
		if (!isDualCasting) {
			NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
			secondaryMagicAimNode->m_localTransform = savedState.secondaryMagicAimNodeLocalTransform;
			CALL_MEMBER_FN(secondaryMagicAimNode, UpdateNode)(&ctx);

			secondaryMagicOffsetNode->m_localTransform = savedState.secondaryMagicOffsetNodeLocalTransform;
			RestoreGeometryScaleDownstream(secondaryNodeScales, secondaryMagicOffsetNode);
			CALL_MEMBER_FN(secondaryMagicOffsetNode, UpdateNode)(&ctx);

			state = DualCastState::Idle;
		}
		else {
			// Secondary offset node update
			{
				NiTransform transform = secondaryMagicOffsetNode->m_worldTransform;
				transform.pos = midpoint;

				float distanceBetweenHands = VectorLength(secondaryMagicOffsetNode->m_worldTransform.pos - primaryMagicOffsetNode->m_worldTransform.pos);
				float scale = std::clamp(1.f + distanceBetweenHands / 90.f, 1.f, 2.f);
				savedState.currentDualCastScale = scale;

				UpdateNodeTransformLocal(secondaryMagicOffsetNode, transform);
				NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
				CALL_MEMBER_FN(secondaryMagicOffsetNode, UpdateNode)(&ctx);
			}

			// Primary offset node update
			{
				// Move the primary node way below us to hide it
				NiTransform transform = primaryMagicOffsetNode->m_worldTransform;
				transform.pos = midpoint + NiPoint3(0, 0, -10000);
				UpdateNodeTransformLocal(primaryMagicOffsetNode, transform);
				NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
				CALL_MEMBER_FN(primaryMagicOffsetNode, UpdateNode)(&ctx);
			}

			// Aim node update
			{
				// First give us the original direction the left hand would be pointing,
				// since the game actually doesn't reset the rotation of the magic aim node like it does with the position
				secondaryMagicAimNode->m_localTransform = savedState.secondaryMagicAimNodeLocalTransform;
				NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
				CALL_MEMBER_FN(secondaryMagicAimNode, UpdateNode)(&ctx);

				// TODO: Maybe just a lerp is enough?
				NiPoint3 secondaryForward = ForwardVector(secondaryMagicAimNode->m_worldTransform.rot);
				NiPoint3 primaryForward = ForwardVector(primaryMagicAimNode->m_worldTransform.rot);
				float angle = acosf(std::clamp(DotProduct(primaryForward, secondaryForward), -1.f, 1.f));
				NiPoint3 axis = VectorNormalized(CrossProduct(primaryForward, secondaryForward));

				NiPoint3 forward = RotateVectorByAxisAngle(primaryForward, axis, angle * 0.5f);

				NiMatrix33 rot;
				NiPoint3 worldUp = { 0, 0, 1 };
				MatrixFromForwardVector(&rot, &forward, &worldUp);
				NiTransform transform = secondaryMagicAimNode->m_worldTransform;
				transform.rot = rot;

				transform.pos = midpoint;

				UpdateNodeTransformLocal(secondaryMagicAimNode, transform);
				CALL_MEMBER_FN(secondaryMagicAimNode, UpdateNode)(&ctx);
			}
		}
	}
}

void PostPlayerCharacterVRUpdateHook()
{
	// Do scale overrides in this hook which is after vrik has done its scale modifications.

	PlayerCharacter *player = *g_thePlayer;
	if (!player->GetNiNode()) return;

	NiPointer<NiAVObject> secondaryMagicOffsetNode = player->unk3F0[PlayerCharacter::Node::kNode_SecondaryMagicOffsetNode];
	NiPointer<NiAVObject> primaryMagicOffsetNode = player->unk3F0[PlayerCharacter::Node::kNode_PrimaryMagicOffsetNode];

	if (!secondaryMagicOffsetNode || !primaryMagicOffsetNode) return;

	if (state == DualCastState::Idle) {
		/*
		// Secondary offset node update
		{
			NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
			CALL_MEMBER_FN(secondaryMagicOffsetNode, UpdateNode)(&ctx);
		}

		// Primary offset node update
		{
			NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
			CALL_MEMBER_FN(primaryMagicOffsetNode, UpdateNode)(&ctx);
		}
		*/
	}
	if (state == DualCastState::Cast) {
		// Secondary offset node update
		{
			SetGeometryScaleDownstream(secondaryNodeScales, secondaryMagicOffsetNode, savedState.currentDualCastScale);

			NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
			CALL_MEMBER_FN(secondaryMagicOffsetNode, UpdateNode)(&ctx);
		}
	}
}


void PerformHooks()
{
	postMagicNodeUpdateHookedFuncAddr = postMagicNodeUpdateHookedFunc.GetUIntPtr();
	postPCUpdateHookedFuncAddr = postPlayerCharacterVRUpdateHookedFunc.GetUIntPtr();

	{
		struct Code : Xbyak::CodeGenerator {
			Code(void * buf) : Xbyak::CodeGenerator(256, buf)
			{
				Xbyak::Label jumpBack, ret;

				push(rax);
				push(rcx);
				push(rdx);
				push(r8);
				push(r9);
				push(r10);
				push(r11);
				sub(rsp, 0x68); // Need to keep the stack 16 byte aligned
				movsd(ptr[rsp], xmm0);
				movsd(ptr[rsp + 0x10], xmm1);
				movsd(ptr[rsp + 0x20], xmm2);
				movsd(ptr[rsp + 0x30], xmm3);
				movsd(ptr[rsp + 0x40], xmm4);
				movsd(ptr[rsp + 0x50], xmm5);

				// Call our hook
				mov(rax, (uintptr_t)PostMagicNodeUpdateHook);
				call(rax);

				movsd(xmm0, ptr[rsp]);
				movsd(xmm1, ptr[rsp + 0x10]);
				movsd(xmm2, ptr[rsp + 0x20]);
				movsd(xmm3, ptr[rsp + 0x30]);
				movsd(xmm4, ptr[rsp + 0x40]);
				movsd(xmm5, ptr[rsp + 0x50]);
				add(rsp, 0x68);
				pop(r11);
				pop(r10);
				pop(r9);
				pop(r8);
				pop(rdx);
				pop(rcx);
				pop(rax);

				// Original code
				mov(rax, postMagicNodeUpdateHookedFuncAddr);
				call(rax);

				// Jump back to whence we came (+ the size of the initial branch instruction)
				jmp(ptr[rip + jumpBack]);

				L(jumpBack);
				dq(postMagicNodeUpdateHookLoc.GetUIntPtr() + 5);
			}
		};

		void * codeBuf = g_localTrampoline.StartAlloc();
		Code code(codeBuf);
		g_localTrampoline.EndAlloc(code.getCurr());

		g_branchTrampoline.Write5Branch(postMagicNodeUpdateHookLoc.GetUIntPtr(), uintptr_t(code.getCode()));

		_MESSAGE("Post magic node update hook complete");
	}

	{
		struct Code : Xbyak::CodeGenerator {
			Code(void * buf) : Xbyak::CodeGenerator(256, buf)
			{
				Xbyak::Label jumpBack, ret;

				push(rax);
				push(rcx);
				push(rdx);
				push(r8);
				push(r9);
				push(r10);
				push(r11);
				sub(rsp, 0x68); // Need to keep the stack 16 byte aligned
				movsd(ptr[rsp], xmm0);
				movsd(ptr[rsp + 0x10], xmm1);
				movsd(ptr[rsp + 0x20], xmm2);
				movsd(ptr[rsp + 0x30], xmm3);
				movsd(ptr[rsp + 0x40], xmm4);
				movsd(ptr[rsp + 0x50], xmm5);

				// Call our hook
				mov(rax, (uintptr_t)PostPlayerCharacterVRUpdateHook);
				call(rax);

				movsd(xmm0, ptr[rsp]);
				movsd(xmm1, ptr[rsp + 0x10]);
				movsd(xmm2, ptr[rsp + 0x20]);
				movsd(xmm3, ptr[rsp + 0x30]);
				movsd(xmm4, ptr[rsp + 0x40]);
				movsd(xmm5, ptr[rsp + 0x50]);
				add(rsp, 0x68);
				pop(r11);
				pop(r10);
				pop(r9);
				pop(r8);
				pop(rdx);
				pop(rcx);
				pop(rax);

				// Original code
				mov(rax, postPCUpdateHookedFuncAddr);
				call(rax);

				// Jump back to whence we came (+ the size of the initial branch instruction)
				jmp(ptr[rip + jumpBack]);

				L(jumpBack);
				dq(postPlayerCharacterVRUpdateHookLoc.GetUIntPtr() + 5);
			}
		};

		void * codeBuf = g_localTrampoline.StartAlloc();
		Code code(codeBuf);
		g_localTrampoline.EndAlloc(code.getCurr());

		g_branchTrampoline.Write5Branch(postPlayerCharacterVRUpdateHookLoc.GetUIntPtr(), uintptr_t(code.getCode()));

		_MESSAGE("Post PlayerCharacter::VRUpdate hook complete");
	}
}


bool TryHook()
{
	// This should be sized to the actual amount used by your trampoline
	static const size_t TRAMPOLINE_SIZE = 512;

	if (g_trampoline) {
		void* branch = g_trampoline->AllocateFromBranchPool(g_pluginHandle, TRAMPOLINE_SIZE);
		if (!branch) {
			_ERROR("couldn't acquire branch trampoline from SKSE. this is fatal. skipping remainder of init process.");
			return false;
		}

		g_branchTrampoline.SetBase(TRAMPOLINE_SIZE, branch);

		void* local = g_trampoline->AllocateFromLocalPool(g_pluginHandle, TRAMPOLINE_SIZE);
		if (!local) {
			_ERROR("couldn't acquire codegen buffer from SKSE. this is fatal. skipping remainder of init process.");
			return false;
		}

		g_localTrampoline.SetBase(TRAMPOLINE_SIZE, local);
	}
	else {
		if (!g_branchTrampoline.Create(TRAMPOLINE_SIZE)) {
			_ERROR("couldn't create branch trampoline. this is fatal. skipping remainder of init process.");
			return false;
		}
		if (!g_localTrampoline.Create(TRAMPOLINE_SIZE, nullptr))
		{
			_ERROR("couldn't create codegen buffer. this is fatal. skipping remainder of init process.");
			return false;
		}
	}

	PerformHooks();
	return true;
}


extern "C" {
	void OnDataLoaded()
	{
	}

	void OnInputLoaded()
	{
	}

	// Listener for SKSE Messages
	void OnSKSEMessage(SKSEMessagingInterface::Message* msg)
	{
		if (msg) {
			if (msg->type == SKSEMessagingInterface::kMessage_InputLoaded) {
				OnInputLoaded();
			}
			else if (msg->type == SKSEMessagingInterface::kMessage_DataLoaded) {
				OnDataLoaded();
			}
			else if (msg->type == SKSEMessagingInterface::kMessage_PostLoad) {
				
			}
		}
	}

	bool SKSEPlugin_Query(const SKSEInterface* skse, PluginInfo* info)
	{
		gLog.OpenRelative(CSIDL_MYDOCUMENTS, "\\My Games\\Skyrim VR\\SKSE\\misvr.log");
		gLog.SetPrintLevel(IDebugLog::kLevel_DebugMessage);
		gLog.SetLogLevel(IDebugLog::kLevel_DebugMessage);

		_MESSAGE("MISVR v%s", MISVR_VERSION_VERSTRING);

		info->infoVersion = PluginInfo::kInfoVersion;
		info->name = "MISVR";
		info->version = MISVR_VERSION_MAJOR;

		g_pluginHandle = skse->GetPluginHandle();

		if (skse->isEditor) {
			_FATALERROR("[FATAL ERROR] Loaded in editor, marking as incompatible!\n");
			return false;
		}
		else if (skse->runtimeVersion != RUNTIME_VR_VERSION_1_4_15) {
			_FATALERROR("[FATAL ERROR] Unsupported runtime version %08X!\n", skse->runtimeVersion);
			return false;
		}

		return true;
	}

	bool SKSEPlugin_Load(const SKSEInterface * skse)
	{	// Called by SKSE to load this plugin
		_MESSAGE("MISVR loaded");

		_MESSAGE("Registering for SKSE messages");
		g_messaging = (SKSEMessagingInterface*)skse->QueryInterface(kInterface_Messaging);
		g_messaging->RegisterListener(g_pluginHandle, "SKSE", OnSKSEMessage);

		g_trampoline = (SKSETrampolineInterface *)skse->QueryInterface(kInterface_Trampoline);
		if (!g_trampoline) {
			_WARNING("Couldn't get trampoline interface");
		}
		if (!TryHook()) {
			_ERROR("[CRITICAL] Failed to perform hooks");
			return false;
		}

		return true;
	}
};
