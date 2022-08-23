#pragma once

#include "skse64/NiNodes.h"
#include "skse64/GameData.h"


namespace Config {
	struct Options {
		// Add config options for things: left / right / combined hand vectors for dualcast spell direction, min / max scale while dual casting along with hand - hand distance to scale over.
		float dualCastHandsCloseSpellScale = 1.f;
		float dualCastHandsFarSpellScale = 2.f;
		float dualCastHandSeparationScalingDistance = 90.f;

		float spellScaleWhenMagickaEmpty = 0.35f;
		float spellScaleWhenMagickaFull = 1.f;

		bool useCastingTimeForMergeTime = false;
		float spellMergeTime = 0.15f;
		float spellUnMergeTime = 0.1f;

		float magicRotationRoll = 0.f;
		float magicRotationYaw = 0.f;

		int numSmoothingFramesNovice = 10;
		int numSmoothingFramesApprentice = 15;
		int numSmoothingFramesAdept = 20;
		int numSmoothingFramesExpert = 25;
		int numSmoothingFramesMaster = 40;
		float smoothingDualCastMultiplier = 1.25;

		bool useOffHandForDualCastAiming = false;
		bool useMainHandForDualCastAiming = false;
	};
	extern Options options; // global object containing options


	// Fills Options struct from INI file
	bool ReadConfigOptions();

	const std::string & GetConfigPath();

	std::string GetConfigOption(const char * section, const char * key);

	bool GetConfigOptionDouble(const char *section, const char *key, double *out);
	bool GetConfigOptionFloat(const char *section, const char *key, float *out);
	bool GetConfigOptionInt(const char *section, const char *key, int *out);
	bool GetConfigOptionBool(const char *section, const char *key, bool *out);
}
