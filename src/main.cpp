#include "common/IDebugLog.h"  // IDebugLog
#include "skse64_common/skse_version.h"  // RUNTIME_VERSION
#include "skse64/PluginAPI.h"  // SKSEInterface, PluginInfo
#include "xbyak/xbyak.h"
#include "skse64_common/BranchTrampoline.h"

#include <ShlObj.h>  // CSIDL_MYDOCUMENTS

#include "version.h"
#include "config.h"
#include "RE.h"
#include "utils.h"


// SKSE globals
static PluginHandle	g_pluginHandle = kPluginHandle_Invalid;
static SKSEMessagingInterface *g_messaging = nullptr;

SKSETrampolineInterface *g_trampoline = nullptr;

// Hook stuff
uintptr_t postMagicNodeUpdateHookedFuncAddr = 0;
uintptr_t postPCUpdateHookedFuncAddr = 0;

auto postMagicNodeUpdateHookLoc = RelocAddr<uintptr_t>(0x6AC035);
auto postMagicNodeUpdateHookedFunc = RelocAddr<uintptr_t>(0x398940);

auto postPlayerCharacterVRUpdateHookLoc = RelocAddr<uintptr_t>(0x6C6A18);
auto postPlayerCharacterVRUpdateHookedFunc = RelocAddr<uintptr_t>(0xC9BC10);

// Couple of functions we use from the exe
typedef NiMatrix33 * (*_MatrixFromForwardVector)(NiMatrix33 *matOut, NiPoint3 *forward, NiPoint3 *world);
RelocAddr<_MatrixFromForwardVector> MatrixFromForwardVector(0xC4C1E0);

typedef float(*_Actor_GetActorValuePercentage)(Actor *_this, UInt32 actorValue);
RelocAddr<_Actor_GetActorValuePercentage> Actor_GetActorValuePercentage(0x5DEB30);

typedef NiMatrix33 * (*_EulerToMatrix)(NiMatrix33 *out, float pitch, float roll, float yaw);
RelocAddr<_EulerToMatrix> EulerToMatrix(0xC995A0);
inline NiMatrix33 EulerToNiMatrix(float pitch, float roll, float yaw) { NiMatrix33 out; EulerToMatrix(&out, pitch, roll, yaw); return out; }

RelocPtr<float> fMagicRotationPitch(0x1EAEB00);


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

SpellItem *currentMainHandSpell = nullptr;
SpellItem *currentOffHandSpell = nullptr;


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

	// First, apply user-supplied roll/yaw aim values, as the base game does not support these
	bool isLeftHanded = *g_leftHandedMode;
	NiAVObject *leftAimNode = isLeftHanded ? primaryMagicAimNode : secondaryMagicAimNode;
	NiAVObject *rightAimNode = isLeftHanded ? secondaryMagicAimNode : primaryMagicAimNode;
	{
		NiPoint3 euler = { *fMagicRotationPitch, Config::options.magicRotationRoll, Config::options.magicRotationYaw };
		euler *= 0.017453292;
		rightAimNode->m_localTransform.rot = EulerToNiMatrix(euler.x, euler.y, euler.z);
		NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
		CALL_MEMBER_FN(rightAimNode, UpdateNode)(&ctx);
	}
	{
		NiPoint3 euler = { *fMagicRotationPitch, -Config::options.magicRotationRoll, -Config::options.magicRotationYaw };
		euler *= 0.017453292;
		leftAimNode->m_localTransform.rot = EulerToNiMatrix(euler.x, euler.y, euler.z);
		NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
		CALL_MEMBER_FN(leftAimNode, UpdateNode)(&ctx);
	}

	// Dualcast state updates
	if (state == DualCastState::Idle) {
		if (isDualCasting) {
			savedState.secondaryMagicAimNodeLocalTransform = secondaryMagicAimNode->m_localTransform;
			savedState.secondaryMagicOffsetNodeLocalTransform = secondaryMagicOffsetNode->m_localTransform;
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
			RestoreParticleScaleDownstream(secondaryNodeScales, secondaryMagicOffsetNode);
			CALL_MEMBER_FN(secondaryMagicOffsetNode, UpdateNode)(&ctx);

			state = DualCastState::Idle;
		}
		else {
			// Secondary offset node update
			{
				NiTransform transform = secondaryMagicOffsetNode->m_worldTransform;
				transform.pos = midpoint;

				float distanceBetweenHands = VectorLength(secondaryMagicOffsetNode->m_worldTransform.pos - primaryMagicOffsetNode->m_worldTransform.pos);
				float minScale = Config::options.dualCastMinSpellScale;
				float maxScale = Config::options.dualCastMaxSpellScale;
				float scale = std::clamp(minScale + distanceBetweenHands / Config::options.dualCastHandSeparationScalingDistance, minScale, maxScale);
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

				NiPoint3 forward;
				if (Config::options.useMainHandForDualCastAiming && !Config::options.useOffHandForDualCastAiming) {
					// Main hand only
					forward = ForwardVector(primaryMagicAimNode->m_worldTransform.rot);
				}
				else if (Config::options.useOffHandForDualCastAiming && !Config::options.useMainHandForDualCastAiming) {
					// Offhand only
					forward = ForwardVector(secondaryMagicAimNode->m_worldTransform.rot);
				}
				else {
					// Combine both hands
					// TODO: Maybe just a lerp is close enough?
					NiPoint3 secondaryForward = ForwardVector(secondaryMagicAimNode->m_worldTransform.rot);
					NiPoint3 primaryForward = ForwardVector(primaryMagicAimNode->m_worldTransform.rot);
					float angle = acosf(std::clamp(DotProduct(primaryForward, secondaryForward), -1.f, 1.f)); // clamp input of acos to be safe
					NiPoint3 axis = VectorNormalized(CrossProduct(primaryForward, secondaryForward));

					forward = RotateVectorByAxisAngle(primaryForward, axis, angle * 0.5f);
				}

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

	SpellItem *mainHandSpell = GetEquippedSpell(player, false);
	SpellItem *offHandSpell = GetEquippedSpell(player, true);

	bool isSheathed = !player->actorState.IsWeaponDrawn();
	if (isSheathed) {
		mainHandSpell = nullptr;
		offHandSpell = nullptr;
	}

	if (mainHandSpell != currentMainHandSpell) {
		// Switched spells -> reset node scales for that hand
		if (primaryNodeScales.size() > 0) primaryNodeScales.clear();
		currentMainHandSpell = mainHandSpell;
	}
	if (offHandSpell != currentOffHandSpell) {
		// Switched spells -> reset node scales for that hand
		if (secondaryNodeScales.size() > 0) secondaryNodeScales.clear();
		currentOffHandSpell = offHandSpell;
	}

	if (isSheathed) {
		// Just don't mess with anything while sheathed
		return;
	}

	NiPointer<NiAVObject> secondaryMagicOffsetNode = player->unk3F0[PlayerCharacter::Node::kNode_SecondaryMagicOffsetNode];
	NiPointer<NiAVObject> primaryMagicOffsetNode = player->unk3F0[PlayerCharacter::Node::kNode_PrimaryMagicOffsetNode];

	if (!secondaryMagicOffsetNode || !primaryMagicOffsetNode) return;

	float minScale = Config::options.spellScaleWhenMagickaEmpty;
	float maxScale = Config::options.spellScaleWhenMagickaFull;

	// ActorValue ids:
	// - health is 24
	// - magicka is 25
	// - stamina is 26
	float magickaPercentage = Actor_GetActorValuePercentage(player, 25);
	//_MESSAGE("Magicka percent: %.2f", magickaPercentage);
	float magickaScale = minScale + (maxScale - minScale) * magickaPercentage;

	if (state == DualCastState::Idle) {
		// Secondary offset node update
		{
			SetParticleScaleDownstream(secondaryNodeScales, secondaryMagicOffsetNode, magickaScale);
			NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
			CALL_MEMBER_FN(secondaryMagicOffsetNode, UpdateNode)(&ctx);
		}

		// Primary offset node update
		{
			SetParticleScaleDownstream(primaryNodeScales, primaryMagicOffsetNode, magickaScale);
			NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
			CALL_MEMBER_FN(primaryMagicOffsetNode, UpdateNode)(&ctx);
		}

		//_MESSAGE("%d\t%d", secondaryNodeScales.size(), primaryNodeScales.size());
	}
	if (state == DualCastState::Cast) {
		// Secondary offset node update
		{
			float scale = magickaScale * savedState.currentDualCastScale;
			SetParticleScaleDownstream(secondaryNodeScales, secondaryMagicOffsetNode, scale);

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

		if (Config::ReadConfigOptions()) {
			_MESSAGE("Successfully read config parameters");
		}
		else {
			_WARNING("[WARNING] Failed to read config options. Using defaults instead.");
		}

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
