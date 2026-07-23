#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace spex
{
enum SpectralFeatureIndex
{
    windowedPeakAmplitude = 0,
    slidingWindowRms,
    interpolatedSpectralPeak,
    papr,
    localSpectralCrest,
    spectralFlatness,
    numSpectralFeatures
};

struct SpectralFeatureSnapshot
{
    std::array<float, numSpectralFeatures> values {};
};

template <size_t MaxSpectrumBins>
class SpectralFeatureAnalyzer
{
public:
    static_assert(MaxSpectrumBins > 0, "SpectralFeatureAnalyzer requires at least one spectrum bin");

    using Snapshot = SpectralFeatureSnapshot;

    void setFeatureFrequencyRange(float minHz, float maxHz)
    {
        featureMinHz = std::max(0.0f, minHz);
        featureMaxHz = std::max(0.0f, maxHz);
    }

    void setFlatnessPowerFloorDb(float floorDb)
    {
        flatnessPowerFloorDb = std::clamp(floorDb, -180.0f, -30.0f);
    }

    const Snapshot& getLatestSnapshot() const
    {
        return latestFeatures;
    }

    const Snapshot& analyze(const float* magnitudes,
                            int magnitudeCount,
                            float sampleRate,
                            int fftSize,
                            float peakAbs,
                            double sumSquares)
    {
        latestFeatures.values.fill(std::numeric_limits<float>::quiet_NaN());

        constexpr float epsilon = 1.0e-12f;
        const int safeFftSize = std::max(fftSize, 1);
        const float meanPower = static_cast<float>(sumSquares / static_cast<double>(safeFftSize));
        const float rms = std::sqrt(std::max(meanPower, epsilon));
        const float safePeak = std::max(peakAbs, epsilon);

        latestFeatures.values[windowedPeakAmplitude] = gainToDecibels(safePeak, -80.0f);
        latestFeatures.values[slidingWindowRms] = gainToDecibels(rms, -80.0f);

        if (magnitudes == nullptr || magnitudeCount <= 0 || sampleRate <= 0.0f || fftSize <= 0)
            return latestFeatures;

        const int maxBin = std::min({ magnitudeCount - 1, fftSize / 2, static_cast<int>(MaxSpectrumBins) - 1 });
        if (maxBin < 0)
            return latestFeatures;

        const float nyquist = sampleRate * 0.5f;
        const float minHz = std::clamp(featureMinHz, 0.0f, nyquist);
        const float maxHz = (featureMaxHz <= 0.0f)
            ? nyquist
            : std::clamp(featureMaxHz, minHz, nyquist);

        int analysisStartBin = static_cast<int>(std::ceil(minHz * static_cast<float>(fftSize) / sampleRate));
        int analysisEndBin = static_cast<int>(std::floor(maxHz * static_cast<float>(fftSize) / sampleRate));
        analysisStartBin = std::clamp(analysisStartBin, 0, maxBin);
        analysisEndBin = std::clamp(analysisEndBin, 0, maxBin);
        if (analysisEndBin < analysisStartBin)
            std::swap(analysisStartBin, analysisEndBin);

        int searchStartBin = analysisStartBin;
        int searchEndBin = analysisEndBin;
        if (searchEndBin < searchStartBin)
        {
            searchStartBin = 0;
            searchEndBin = maxBin;
        }

        int peakBin = searchStartBin;
        for (int i = searchStartBin + 1; i <= searchEndBin; ++i)
        {
            if (magnitudes[i] > magnitudes[peakBin])
                peakBin = i;
        }

        float interpMag = magnitudes[peakBin];
        if (peakBin > 0 && peakBin < maxBin)
        {
            const float left = std::log(std::max(magnitudes[peakBin - 1], epsilon));
            const float center = std::log(std::max(magnitudes[peakBin], epsilon));
            const float right = std::log(std::max(magnitudes[peakBin + 1], epsilon));
            const float denominator = left - 2.0f * center + right;
            const float delta = std::abs(denominator) > epsilon ? 0.5f * (left - right) / denominator : 0.0f;
            const float interpolatedLogMagnitude = center - 0.25f * (left - right) * delta;
            interpMag = std::exp(interpolatedLogMagnitude);
        }
        latestFeatures.values[interpolatedSpectralPeak] = gainToDecibels(interpMag, -80.0f);

        double powerSum = 0.0;
        double maxPower = 0.0;
        int count = 0;
        for (int i = analysisStartBin; i <= analysisEndBin; ++i)
        {
            const double power = static_cast<double>(magnitudes[i]) * static_cast<double>(magnitudes[i]);
            powerSum += power;
            maxPower = std::max(maxPower, power);
            ++count;
        }

        if (count <= 0)
        {
            latestFeatures.values[papr] = std::numeric_limits<float>::quiet_NaN();
            latestFeatures.values[localSpectralCrest] = 0.0f;
            latestFeatures.values[spectralFlatness] = std::numeric_limits<float>::quiet_NaN();
            return latestFeatures;
        }

        const double averagePower = powerSum / static_cast<double>(count);
        const float spectralPapr = static_cast<float>(maxPower / std::max(averagePower, 1.0e-20));
        latestFeatures.values[papr] = 10.0f * std::log10(std::max(spectralPapr, epsilon));

        constexpr int envelopeRadiusBins = 2;
        int crestPeakBin = analysisStartBin;
        for (int i = analysisStartBin; i <= analysisEndBin; ++i)
        {
            const int smoothStart = std::max(analysisStartBin, i - envelopeRadiusBins);
            const int smoothEnd = std::min(analysisEndBin, i + envelopeRadiusBins);
            double smoothedPower = 0.0;
            for (int j = smoothStart; j <= smoothEnd; ++j)
            {
                const double magnitude = static_cast<double>(magnitudes[j]);
                smoothedPower += magnitude * magnitude;
            }
            smoothedPower /= static_cast<double>(smoothEnd - smoothStart + 1);
            powerEnvelope[static_cast<size_t>(i)] = smoothedPower;

            if (smoothedPower > powerEnvelope[static_cast<size_t>(crestPeakBin)])
                crestPeakBin = i;
        }

        int localStartBin = crestPeakBin > 0 ? crestPeakBin / 2 : analysisStartBin;
        int localEndBin = crestPeakBin > 0 ? crestPeakBin * 2 : crestPeakBin + 32;
        localStartBin = std::max(localStartBin, analysisStartBin);
        localEndBin = std::min(localEndBin, analysisEndBin);

        int baselineCount = 0;
        for (int i = localStartBin; i <= localEndBin; ++i)
        {
            if (std::abs(i - crestPeakBin) <= envelopeRadiusBins)
                continue;
            crestBaselineScratch[static_cast<size_t>(baselineCount++)] = powerEnvelope[static_cast<size_t>(i)];
        }

        if (baselineCount < 8)
        {
            baselineCount = 0;
            for (int i = analysisStartBin; i <= analysisEndBin; ++i)
            {
                if (std::abs(i - crestPeakBin) <= envelopeRadiusBins)
                    continue;
                crestBaselineScratch[static_cast<size_t>(baselineCount++)] = powerEnvelope[static_cast<size_t>(i)];
            }
        }

        double localBaselinePower = averagePower;
        if (baselineCount > 0)
        {
            auto baselineBegin = crestBaselineScratch.begin();
            auto baselineEnd = baselineBegin + baselineCount;
            std::sort(baselineBegin, baselineEnd);
            const int trimCount = baselineCount / 10;
            const int first = trimCount;
            const int last = baselineCount - trimCount;

            if (last > first)
            {
                double trimmedPowerSum = 0.0;
                for (int i = first; i < last; ++i)
                    trimmedPowerSum += crestBaselineScratch[static_cast<size_t>(i)];
                localBaselinePower = trimmedPowerSum / static_cast<double>(last - first);
            }
        }

        const double localCrest = powerEnvelope[static_cast<size_t>(crestPeakBin)]
            / std::max(localBaselinePower, 1.0e-20);
        latestFeatures.values[localSpectralCrest] =
            10.0f * std::log10(std::max(static_cast<float>(localCrest), epsilon));

        if (averagePower <= 1.0e-30)
        {
            latestFeatures.values[spectralFlatness] = std::numeric_limits<float>::quiet_NaN();
            return latestFeatures;
        }

        const double powerFloor = std::max(std::pow(10.0, static_cast<double>(flatnessPowerFloorDb) / 10.0), 1.0e-30);
        double logPowerSum = 0.0;
        double meanPowerWithFloor = 0.0;
        for (int i = analysisStartBin; i <= analysisEndBin; ++i)
        {
            const double power = static_cast<double>(magnitudes[i]) * static_cast<double>(magnitudes[i]);
            const double adjustedPower = power + powerFloor;
            logPowerSum += std::log(adjustedPower);
            meanPowerWithFloor += adjustedPower;
        }

        const double geometricMean = std::exp(logPowerSum / static_cast<double>(count));
        const double arithmeticMean = meanPowerWithFloor / static_cast<double>(count);
        const float flatness = static_cast<float>(geometricMean / std::max(arithmeticMean, 1.0e-30));
        latestFeatures.values[spectralFlatness] = std::clamp(flatness, 0.0f, 1.0f);
        return latestFeatures;
    }

private:
    static float gainToDecibels(float gain, float floorDb)
    {
        if (gain <= 0.0f)
            return floorDb;

        return std::max(20.0f * std::log10(gain), floorDb);
    }

    Snapshot latestFeatures {};
    float featureMinHz { 0.0f };
    float featureMaxHz { 0.0f };
    float flatnessPowerFloorDb { -120.0f };
    std::array<double, MaxSpectrumBins> powerEnvelope {};
    std::array<double, MaxSpectrumBins> crestBaselineScratch {};
};
}