#include <chrono>
#include <filesystem>

#include "config.h"


namespace Config {
	// Define extern options
	Options options;

	bool ReadFloat(const std::string &name, float &val)
	{
		if (!GetConfigOptionFloat("Settings", name.c_str(), &val)) {
			_WARNING("Failed to read float config option: %s", name.c_str());
			return false;
		}

		return true;
	}

	bool ReadDouble(const std::string &name, double &val)
	{
		if (!GetConfigOptionDouble("Settings", name.c_str(), &val)) {
			_WARNING("Failed to read double config option: %s", name.c_str());
			return false;
		}

		return true;
	}

	bool ReadBool(const std::string &name, bool &val)
	{
		if (!GetConfigOptionBool("Settings", name.c_str(), &val)) {
			_WARNING("Failed to read bool config option: %s", name.c_str());
			return false;
		}

		return true;
	}

	bool ReadInt(const std::string &name, int &val)
	{
		if (!GetConfigOptionInt("Settings", name.c_str(), &val)) {
			_WARNING("Failed to read int config option: %s", name.c_str());
			return false;
		}

		return true;
	}

	bool ReadString(const std::string &name, std::string &val)
	{
		std::string	data = GetConfigOption("Settings", name.c_str());
		if (data.empty()) {
			_WARNING("Failed to read str config option: %s", name.c_str());
			return false;
		}

		val = data;
		return true;
	}

	bool ReadVector(const std::string &name, NiPoint3 &vec)
	{
		if (!ReadFloat(name + "X", vec.x)) return false;
		if (!ReadFloat(name + "Y", vec.y)) return false;
		if (!ReadFloat(name + "Z", vec.z)) return false;

		return true;
	}

	bool ReadConfigOptions()
	{
		if (!ReadFloat("dualCastHandsCloseSpellScale", options.dualCastHandsCloseSpellScale)) return false;
		if (!ReadFloat("dualCastHandsFarSpellScale", options.dualCastHandsFarSpellScale)) return false;
		if (!ReadFloat("DualCastHandSeparationScalingDistance", options.dualCastHandSeparationScalingDistance)) return false;

		if (!ReadFloat("SpellScaleWhenMagickaEmpty", options.spellScaleWhenMagickaEmpty)) return false;
		if (!ReadFloat("SpellScaleWhenMagickaFull", options.spellScaleWhenMagickaFull)) return false;

		if (!ReadFloat("MagicRotationRoll", options.magicRotationRoll)) return false;
		if (!ReadFloat("MagicRotationYaw", options.magicRotationYaw)) return false;

		if (!ReadInt("numSmoothingFramesNovice", options.numSmoothingFramesNovice)) return false;
		if (!ReadInt("numSmoothingFramesApprentice", options.numSmoothingFramesApprentice)) return false;
		if (!ReadInt("numSmoothingFramesAdept", options.numSmoothingFramesAdept)) return false;
		if (!ReadInt("numSmoothingFramesExpert", options.numSmoothingFramesExpert)) return false;
		if (!ReadInt("numSmoothingFramesMaster", options.numSmoothingFramesMaster)) return false;
		if (!ReadFloat("smoothingDualCastMultiplier", options.smoothingDualCastMultiplier)) return false;

		if (!ReadBool("UseOffHandForDualCastAiming", options.useOffHandForDualCastAiming)) return false;
		if (!ReadBool("UseMainHandForDualCastAiming", options.useMainHandForDualCastAiming)) return false;

		return true;
	}

	const std::string & GetConfigPath()
	{
		static std::string s_configPath;

		if (s_configPath.empty()) {
			std::string	runtimePath = GetRuntimeDirectory();
			if (!runtimePath.empty()) {
				s_configPath = runtimePath + "Data\\SKSE\\Plugins\\misvr.ini";

				_MESSAGE("config path = %s", s_configPath.c_str());
			}
		}

		return s_configPath;
	}

	std::string GetConfigOption(const char *section, const char *key)
	{
		std::string	result;

		const std::string & configPath = GetConfigPath();
		if (!configPath.empty()) {
			char	resultBuf[256];
			resultBuf[0] = 0;

			UInt32	resultLen = GetPrivateProfileString(section, key, NULL, resultBuf, sizeof(resultBuf), configPath.c_str());

			result = resultBuf;
		}

		return result;
	}

	bool GetConfigOptionDouble(const char *section, const char *key, double *out)
	{
		std::string	data = GetConfigOption(section, key);
		if (data.empty())
			return false;

		*out = std::stod(data);
		return true;
	}

	bool GetConfigOptionFloat(const char *section, const char *key, float *out)
	{
		std::string	data = GetConfigOption(section, key);
		if (data.empty())
			return false;

		*out = std::stof(data);
		return true;
	}

	bool GetConfigOptionInt(const char *section, const char *key, int *out)
	{
		std::string	data = GetConfigOption(section, key);
		if (data.empty())
			return false;

		*out = std::stoi(data);
		return true;
	}

	bool GetConfigOptionBool(const char *section, const char *key, bool *out)
	{
		std::string	data = GetConfigOption(section, key);
		if (data.empty())
			return false;

		int val = std::stoi(data);
		if (val == 1) {
			*out = true;
			return true;
		}
		else if (val == 0) {
			*out = false;
			return true;
		}
		else {
			return false;
		}
	}
}
