// Copyright CamSim Contributors. All Rights Reserved.

#include "Sensor/SensorPresets.h"
#include "Sensor/SensorTypes.h"

bool CamSimSensorPresets::Apply(const FString& Name, FSensorModeConfig& InOut)
{
	// Each branch fully re-initialises Optics/Detector from struct defaults
	// before setting the preset's values, so callers may re-apply a preset
	// on top of a previous one (e.g. a yaml override switches presets)
	// without leftover fields from the old preset.
	if (Name == TEXT("eo_hd_cmos"))
	{
		InOut.Optics = FSensorOpticsConfig();
		InOut.Optics.FNumber            = 4.0f;
		InOut.Optics.PixelPitchUm       = 2.9f;
		InOut.Optics.WavelengthUm       = 0.55f;
		InOut.Optics.VignettingExponent = 4.0f;

		InOut.Detector = FSensorDetectorConfig();
		InOut.Detector.Type              = ESensorDetectorType::Photon;
		InOut.Detector.FullWellE         = 10000.0f;
		InOut.Detector.ReadNoiseE        = 2.0f;
		InOut.Detector.Prnu              = 0.01f;
		InOut.Detector.DsnuE             = 1.0f;
		InOut.Detector.DarkCurrentEs     = 5.0f;
		InOut.Detector.MaxAnalogGainDb   = 30.0f;
		InOut.Detector.AdcBits           = 12;
		InOut.Detector.HotPixelFraction  = 1e-5f;
		InOut.Detector.DeadPixelFraction = 1e-5f;
		return true;
	}

	if (Name == TEXT("mwir_cooled"))
	{
		InOut.Optics = FSensorOpticsConfig();
		InOut.Optics.FNumber            = 4.0f;
		InOut.Optics.PixelPitchUm       = 15.0f;
		InOut.Optics.WavelengthUm       = 4.0f;
		InOut.Optics.VignettingExponent = 4.0f;

		InOut.Detector = FSensorDetectorConfig();
		InOut.Detector.Type              = ESensorDetectorType::Photon;
		InOut.Detector.FullWellE         = 7000000.0f;
		InOut.Detector.ReadNoiseE        = 400.0f;
		InOut.Detector.Prnu              = 0.001f;
		InOut.Detector.DsnuE             = 2000.0f;
		InOut.Detector.DarkCurrentEs     = 0.0f;  // residual after NUC; cooled
		InOut.Detector.AdcBits           = 14;
		InOut.Detector.HotPixelFraction  = 1e-4f;
		InOut.Detector.DeadPixelFraction = 1e-4f;
		// MaxAnalogGainDb: not applicable (AGC drives IR exposure) — struct default.
		return true;
	}

	if (Name == TEXT("lwir_uncooled"))
	{
		InOut.Optics = FSensorOpticsConfig();
		InOut.Optics.FNumber            = 1.2f;
		InOut.Optics.PixelPitchUm       = 12.0f;
		InOut.Optics.WavelengthUm       = 10.0f;
		InOut.Optics.VignettingExponent = 4.0f;

		InOut.Detector = FSensorDetectorConfig();
		InOut.Detector.Type              = ESensorDetectorType::Microbolometer;
		InOut.Detector.TemporalNoise     = 0.004f;
		InOut.Detector.PixelFpn          = 0.003f;
		InOut.Detector.ColumnFpn         = 0.0015f;
		InOut.Detector.RowFpn            = 0.001f;
		InOut.Detector.AdcBits           = 14;
		InOut.Detector.HotPixelFraction  = 1e-4f;
		InOut.Detector.DeadPixelFraction = 1e-4f;
		// FullWellE/ReadNoiseE/Prnu/DsnuE/DarkCurrentEs: not applicable
		// (microbolometer) — struct defaults.
		return true;
	}

	return false;
}
