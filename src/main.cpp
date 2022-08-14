#include "common/IDebugLog.h"  // IDebugLog
#include "skse64_common/skse_version.h"  // RUNTIME_VERSION
#include "skse64/PluginAPI.h"  // SKSEInterface, PluginInfo
#include "xbyak/xbyak.h"
#include "skse64_common/BranchTrampoline.h"

#include <ShlObj.h>  // CSIDL_MYDOCUMENTS
#include <deque>

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
uintptr_t postWandUpdateHookedFuncAddr = 0;

auto postMagicNodeUpdateHookLoc = RelocAddr<uintptr_t>(0x6AC035);
auto postMagicNodeUpdateHookedFunc = RelocAddr<uintptr_t>(0x398940);

auto postWandUpdateHookLoc = RelocAddr<uintptr_t>(0x13233C7); // A call shortly after the wand nodes are updated as part of Main::Draw()
auto postWandUpdateHookedFunc = RelocAddr<uintptr_t>(0xDCF900);

// Couple of functions we use from the exe
typedef NiMatrix33 * (*_MatrixFromForwardVector)(NiMatrix33 *matOut, NiPoint3 *forward, NiPoint3 *world);
RelocAddr<_MatrixFromForwardVector> MatrixFromForwardVector(0xC4C1E0);

typedef float(*_Actor_GetActorValuePercentage)(Actor *_this, UInt32 actorValue);
RelocAddr<_Actor_GetActorValuePercentage> Actor_GetActorValuePercentage(0x5DEB30);

typedef NiMatrix33 * (*_EulerToNiMatrix)(NiMatrix33 *out, float pitch, float roll, float yaw);
RelocAddr<_EulerToNiMatrix> EulerToNiMatrix(0xC995A0);
inline NiMatrix33 EulerToMatrix(float pitch, float roll, float yaw) { NiMatrix33 out; EulerToNiMatrix(&out, pitch, roll, yaw); return out; }

RelocPtr<float> g_deltaTime(0x30C3A08);

RelocPtr<float> fMagicRotationPitch(0x1EAEB00);


enum class DualCastState {
	Idle,
	Cast
};
DualCastState state = DualCastState::Idle;

struct SavedState {
	NiTransform secondaryMagicOffsetNodeLocalTransform;
	float currentDualCastScale;
};
SavedState savedState;

std::deque<NiPoint3> g_primaryAimVectors{ 100, NiPoint3() };
std::deque<NiPoint3> g_secondaryAimVectors{ 100, NiPoint3() };
NiPoint3 GetSmoothedVector(std::deque<NiPoint3> &vectors, int numFrames)
{
	NiPoint3 smoothedVector = NiPoint3();

	int i = 0;
	for (NiPoint3 &vector : vectors) {
		smoothedVector += vector;
		if (++i >= numFrames) {
			break;
		}
	}

	return VectorNormalized(smoothedVector);
}

int GetNumSmoothingFramesForSpell(SpellItem *spell, bool isDualCasting)
{
	int numSmoothingFrames;
	SpellSkillLevel spellLevel = GetSpellSkillLevel(spell);
	switch (spellLevel) {
	case SpellSkillLevel::Master:
		numSmoothingFrames = Config::options.numSmoothingFramesMaster;
		break;
	case SpellSkillLevel::Expert:
		numSmoothingFrames = Config::options.numSmoothingFramesExpert;
		break;
	case SpellSkillLevel::Adept:
		numSmoothingFrames = Config::options.numSmoothingFramesAdept;
		break;
	case SpellSkillLevel::Apprentice:
		numSmoothingFrames = Config::options.numSmoothingFramesApprentice;
		break;
	default:
		numSmoothingFrames = Config::options.numSmoothingFramesNovice;
	}

	float smoothingMultiplier = 0.011f / *g_deltaTime; // Half the number of frames at 45fps compared to 90fps, etc.
	if (isDualCasting) {
		smoothingMultiplier *= Config::options.smoothingDualCastMultiplier;
	}
	return round(float(numSmoothingFrames) * smoothingMultiplier);
}

void PostMagicNodeUpdateHook()
{
	// Do state updates + pos/rot updates in this hook right after the magic nodes get updated, but before vrik so that vrik can apply head bobbing on top.

	PlayerCharacter *player = *g_thePlayer;
	if (!player->GetNiNode()) return;

	NiPointer<NiAVObject> secondaryMagicAimNode = player->unk3F0[PlayerCharacter::Node::kNode_SecondaryMagicAimNode];
	NiPointer<NiAVObject> primaryMagicAimNode = player->unk3F0[PlayerCharacter::Node::kNode_PrimaryMagicAimNode];
	NiPointer<NiAVObject> secondaryMagicOffsetNode = player->unk3F0[PlayerCharacter::Node::kNode_SecondaryMagicOffsetNode];
	NiPointer<NiAVObject> primaryMagicOffsetNode = player->unk3F0[PlayerCharacter::Node::kNode_PrimaryMagicOffsetNode];

	if (!secondaryMagicOffsetNode || !primaryMagicOffsetNode || !secondaryMagicAimNode || !primaryMagicAimNode) return;

	NiPoint3 midpoint = lerp(secondaryMagicOffsetNode->m_worldTransform.pos, primaryMagicOffsetNode->m_worldTransform.pos, 0.5f);

	bool isLeftHanded = *g_leftHandedMode;

	NiAVObject *leftAimNode = isLeftHanded ? primaryMagicAimNode : secondaryMagicAimNode;
	NiAVObject *rightAimNode = isLeftHanded ? secondaryMagicAimNode : primaryMagicAimNode;

	bool isCastingPrimary = IsCastingRight(player);
	bool isCastingSecondary = IsCastingLeft(player);
	bool isCastingLeft = isLeftHanded ? isCastingPrimary : isCastingSecondary;
	bool isCastingRight = isLeftHanded ? isCastingSecondary : isCastingPrimary;

	SpellItem* primarySpell = GetEquippedSpell(player, false);
	SpellItem* secondarySpell = GetEquippedSpell(player, true);
	bool isTwoHandedSpell = primarySpell && get_vfunc<_SpellItem_IsTwoHanded>(primarySpell, 0x67)(primarySpell);

	bool isDualCasting = IsDualCasting(player) || (isTwoHandedSpell && isCastingRight && isCastingLeft);

	// First, apply user-supplied roll/yaw aim values while casting, as the base game does not support these.

	{ // Update right magic aim node with additional rotation values
		NiPoint3 euler = { *fMagicRotationPitch, 0.f, 0.f };
		if (isCastingRight || isDualCasting) {
			 euler.y = Config::options.magicRotationRoll;
			 euler.z = Config::options.magicRotationYaw;
		}
		euler *= 0.017453292;
		rightAimNode->m_localTransform.rot = EulerToMatrix(euler.x, euler.y, euler.z);
		NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
		CALL_MEMBER_FN(rightAimNode, UpdateNode)(&ctx);
	}

	{ // Update left magic aim node with additional rotation values
		NiPoint3 euler = { *fMagicRotationPitch, 0.f, 0.f };
		if (isCastingLeft || isDualCasting) {
			euler.y = -Config::options.magicRotationRoll;
			euler.z = -Config::options.magicRotationYaw;
		}
		euler *= 0.017453292;
		leftAimNode->m_localTransform.rot = EulerToMatrix(euler.x, euler.y, euler.z);
		NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
		CALL_MEMBER_FN(leftAimNode, UpdateNode)(&ctx);
	}

	{ // Update stored aiming directions for this frame after updating them
		NiPoint3 secondaryForward = ForwardVector(secondaryMagicAimNode->m_worldTransform.rot);
		g_secondaryAimVectors.pop_back();
		g_secondaryAimVectors.push_front(secondaryForward);

		NiPoint3 primaryForward = ForwardVector(primaryMagicAimNode->m_worldTransform.rot);
		g_primaryAimVectors.pop_back();
		g_primaryAimVectors.push_front(primaryForward);
	}

	// Dualcast state updates
	if (state == DualCastState::Idle) {
		if (!isDualCasting) {
			NiAVObject::ControllerUpdateContext ctx{ 0, 0 };

			if (secondarySpell) { // Secondary aim node update with smoothed direction
				NiPoint3 forward = GetSmoothedVector(g_secondaryAimVectors, GetNumSmoothingFramesForSpell(secondarySpell, false));

				NiPoint3 worldUp = { 0, 0, 1 };
				NiMatrix33 rot; MatrixFromForwardVector(&rot, &forward, &worldUp);
				NiTransform transform = secondaryMagicAimNode->m_worldTransform;
				transform.rot = rot;

				UpdateNodeTransformLocal(secondaryMagicAimNode, transform);
				CALL_MEMBER_FN(secondaryMagicAimNode, UpdateNode)(&ctx);
			}

			if (primarySpell) { // Primary aim node update with smoothed direction
				NiPoint3 forward = GetSmoothedVector(g_primaryAimVectors, GetNumSmoothingFramesForSpell(primarySpell, false));

				NiPoint3 worldUp = { 0, 0, 1 };
				NiMatrix33 rot; MatrixFromForwardVector(&rot, &forward, &worldUp);
				NiTransform transform = primaryMagicAimNode->m_worldTransform;
				transform.rot = rot;

				UpdateNodeTransformLocal(primaryMagicAimNode, transform);
				CALL_MEMBER_FN(primaryMagicAimNode, UpdateNode)(&ctx);
			}
		}
		else { // Dual casting
			savedState.secondaryMagicOffsetNodeLocalTransform = secondaryMagicOffsetNode->m_localTransform;
			savedState.currentDualCastScale = 1.f;

			state = DualCastState::Cast;
		}
	}
	if (state == DualCastState::Cast) {
		if (!isDualCasting) {
			NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
			secondaryMagicOffsetNode->m_localTransform = savedState.secondaryMagicOffsetNodeLocalTransform;
			CALL_MEMBER_FN(secondaryMagicOffsetNode, UpdateNode)(&ctx);

			state = DualCastState::Idle;
		}
		else { // Dual casting
			{
				float distanceBetweenHands = VectorLength(secondaryMagicOffsetNode->m_worldTransform.pos - primaryMagicOffsetNode->m_worldTransform.pos);
				float minScale = Config::options.dualCastMinSpellScale;
				float maxScale = Config::options.dualCastMaxSpellScale;
				float scale = std::clamp(minScale + distanceBetweenHands / Config::options.dualCastHandSeparationScalingDistance, minScale, maxScale);
				savedState.currentDualCastScale = scale;
			}

			// Secondary offset node update
			{
				NiTransform transform = secondaryMagicOffsetNode->m_worldTransform;
				transform.pos = midpoint;

				UpdateNodeTransformLocal(secondaryMagicOffsetNode, transform);
				NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
				CALL_MEMBER_FN(secondaryMagicOffsetNode, UpdateNode)(&ctx);
			}

			// Primary offset node update
			{
				NiTransform transform = primaryMagicOffsetNode->m_worldTransform;
				transform.pos = midpoint;
				if (Config::options.hidePrimaryMagicNode) {
					// Move the primary node way below us to hide it
					transform.pos += NiPoint3(0, 0, -10000);
				}

				UpdateNodeTransformLocal(primaryMagicOffsetNode, transform);
				NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
				CALL_MEMBER_FN(primaryMagicOffsetNode, UpdateNode)(&ctx);
			}

			// Aim node update
			{
				SpellItem *spell = secondarySpell ? secondarySpell : primarySpell;
				int numSmoothingFrames = GetNumSmoothingFramesForSpell(spell, true);
				NiPoint3 secondaryForward = GetSmoothedVector(g_secondaryAimVectors, numSmoothingFrames);
				NiPoint3 primaryForward = GetSmoothedVector(g_primaryAimVectors, numSmoothingFrames);

				NiPoint3 forward;
				if (Config::options.useMainHandForDualCastAiming && !Config::options.useOffHandForDualCastAiming) {
					// Main hand only
					forward = secondaryForward;
				}
				else if (Config::options.useOffHandForDualCastAiming && !Config::options.useMainHandForDualCastAiming) {
					// Offhand only
					forward = primaryForward;
				}
				else {
					// Combine both hands

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
				NiAVObject::ControllerUpdateContext ctx{ 0, 0 };
				CALL_MEMBER_FN(secondaryMagicAimNode, UpdateNode)(&ctx);
			}
		}
	}
}


void PostWandUpdateHook()
{
	// Do scale overrides in this hook, which is after the last time the wand nodes have their world transforms updated.
	// This allows us to set the scale of the magic offset node world transforms without them getting overwritten.

	PlayerCharacter *player = *g_thePlayer;
	if (!player->GetNiNode()) return;
	
	if (!player->actorState.IsWeaponDrawn()) {
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
		SetParticleScaleDownstream(secondaryMagicOffsetNode, magickaScale);
		SetParticleScaleDownstream(primaryMagicOffsetNode, magickaScale);
	}
	if (state == DualCastState::Cast) {
		float scale = magickaScale * savedState.currentDualCastScale;
		SetParticleScaleDownstream(secondaryMagicOffsetNode, scale);
		SetParticleScaleDownstream(primaryMagicOffsetNode, scale);
	}
}


void PerformHooks()
{
	postMagicNodeUpdateHookedFuncAddr = postMagicNodeUpdateHookedFunc.GetUIntPtr();
	postWandUpdateHookedFuncAddr = postWandUpdateHookedFunc.GetUIntPtr();

	{
		struct Code : Xbyak::CodeGenerator {
			Code(void * buf) : Xbyak::CodeGenerator(256, buf)
			{
				Xbyak::Label jumpBack, ret;

				push(rcx);
				sub(rsp, 0x28); // Need to keep the stack 16 byte aligned

				// Call our hook
				mov(rax, (uintptr_t)PostMagicNodeUpdateHook);
				call(rax);

				add(rsp, 0x28);
				pop(rcx);

				// Original code
				mov(rax, postMagicNodeUpdateHookedFuncAddr); // TESRace::IsBeast()
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

				push(rcx);
				push(rdx);
				push(r8);
				sub(rsp, 0x28); // Need to keep the stack 16 byte aligned

				// Call our hook
				mov(rax, (uintptr_t)PostWandUpdateHook);
				call(rax);

				add(rsp, 0x28);
				pop(r8);
				pop(rdx);
				pop(rcx);

				// Original code
				mov(rax, postWandUpdateHookedFuncAddr);
				call(rax);

				// Jump back to whence we came (+ the size of the initial branch instruction)
				jmp(ptr[rip + jumpBack]);

				L(jumpBack);
				dq(postWandUpdateHookLoc.GetUIntPtr() + 5);
			}
		};

		void * codeBuf = g_localTrampoline.StartAlloc();
		Code code(codeBuf);
		g_localTrampoline.EndAlloc(code.getCurr());

		g_branchTrampoline.Write5Branch(postWandUpdateHookLoc.GetUIntPtr(), uintptr_t(code.getCode()));

		_MESSAGE("Post Wand Update hook complete");
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
