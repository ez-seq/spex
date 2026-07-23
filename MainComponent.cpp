#include "MainComponent.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace
{
constexpr int kFeatureCount = spex::numSpectralFeatures;

const std::array<const char*, kFeatureCount> kFeatureNames
{
    "Windowed Peak",
    "Sliding RMS",
    "Interpolated Spectral Peak",
    "Spectral PAPR",
    "Local Spectral Crest",
    "Spectral Flatness"
};

const std::array<juce::Range<float>, kFeatureCount> kDefaultValueRanges
{
    juce::Range<float>(-90.0f, 0.0f),
    juce::Range<float>(-90.0f, 0.0f),
    juce::Range<float>(-90.0f, 20.0f),
    juce::Range<float>(0.0f, 36.0f),
    juce::Range<float>(0.0f, 1.0f)
};

const std::array<float, kFeatureCount> kSmoothingAlpha
{
    0.86f,
    0.90f,
    0.88f,
    0.85f,
    0.85f,
    0.65f
};

const std::array<float, kFeatureCount> kHysteresis
{
    0.20f,
    0.20f,
    0.20f,
    0.15f,
    0.15f,
    0.0001f
};

const std::array<juce::Colour, kFeatureCount> kFeatureColours
{
    juce::Colour(0xff38bdf8),
    juce::Colour(0xff22c55e),
    juce::Colour(0xfff59e0b),
    juce::Colour(0xffef4444),
    juce::Colour(0xffa78bfa),
    juce::Colour(0xffa78bfa)
};

juce::String csvHeaderName(int featureIndex)
{
    switch (featureIndex)
    {
        case spex::windowedPeakAmplitude:    return "windowed_peak_dbfs";
        case spex::slidingWindowRms:         return "rms_dbfs";
        case spex::interpolatedSpectralPeak: return "interp_spectral_peak_db";
        case spex::papr:                     return "spectral_papr_db";
        case spex::localSpectralCrest:       return "local_spectral_crest_db";
        case spex::spectralFlatness:         return "spectral_flatness";
        default:                                                 return "feature";
    }
}

juce::String formatFeatureValueText(int featureIndex, float value)
{
    if (!std::isfinite(value))
        return "nan";

    switch (featureIndex)
    {
        case spex::spectralFlatness:
            return juce::String(value, 4);
        default:
            return juce::String(value, 2) + " dB";
    }
}

juce::Range<float> makeAutoscaledRange(int featureIndex,
                                       float minValue,
                                       float maxValue,
                                       bool hasBounds)
{
    const auto fallback = kDefaultValueRanges[static_cast<size_t>(featureIndex)];
    if (!hasBounds)
        return fallback;

    const float lo = std::min(minValue, maxValue);
    const float hi = std::max(minValue, maxValue);
    const float span = hi - lo;
    const float pad = std::max(span * 0.1f, featureIndex == spex::spectralFlatness ? 0.005f : 0.1f);

    float start = lo - pad;
    float end = hi + pad;

    if (span < 1.0e-6f)
    {
        const float center = lo;
        const float halfWidth = featureIndex == spex::spectralFlatness ? 0.02f : 0.5f;
        start = center - halfWidth;
        end = center + halfWidth;
    }

    if (featureIndex == spex::spectralFlatness)
    {
        start = std::max(0.0f, start);
        end = std::min(1.0f, end);
        if (end - start < 0.01f)
            end = std::min(1.0f, start + 0.01f);
    }

    return { start, end };
}

void drawTrace(juce::Graphics& g,
               juce::Rectangle<float> area,
               const std::vector<float>& history,
               juce::Range<float> valueRange,
               juce::Colour colour)
{
    g.setColour(juce::Colour(0x141ffffff));
    g.fillRoundedRectangle(area, 4.0f);

    if (history.size() < 2)
        return;

    g.setColour(juce::Colour(0x20ffffff));
    g.drawLine(area.getX(), area.getCentreY(), area.getRight(), area.getCentreY(), 1.0f);

    juce::Path trace;
    const auto clampToRange = [&](float value)
    {
        return std::clamp(value, valueRange.getStart(), valueRange.getEnd());
    };

    const float minV = valueRange.getStart();
    const float maxV = valueRange.getEnd();
    const float xStep = area.getWidth() / static_cast<float>(history.size() - 1);
    bool started = false;

    for (size_t i = 0; i < history.size(); ++i)
    {
        if (!std::isfinite(history[i]))
        {
            started = false;
            continue;
        }

        const float x = area.getX() + xStep * static_cast<float>(i);
        const float clamped = clampToRange(history[i]);
        const float y = juce::jmap(clamped, minV, maxV, area.getBottom(), area.getY());

        if (!started)
        {
            trace.startNewSubPath(x, y);
            started = true;
        }
        else
        {
            trace.lineTo(x, y);
        }
    }

    g.setColour(colour.withAlpha(0.95f));
    g.strokePath(trace, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

class FeatureStackContent final : public juce::Component
{
public:
    FeatureStackContent() = default;

    void setData(const std::array<std::vector<float>, kFeatureCount>& sourceHistory,
                 const std::array<float, kFeatureCount>& sourceCurrent,
                 const std::array<float, kFeatureCount>& sourceMins,
                 const std::array<float, kFeatureCount>& sourceMaxs,
                 const std::array<bool, kFeatureCount>& sourceHasBounds)
    {
        history = sourceHistory;
        current = sourceCurrent;
        mins = sourceMins;
        maxs = sourceMaxs;
        hasBounds = sourceHasBounds;
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff0b0d10));

        auto area = getLocalBounds().reduced(16);
        const int rowGap = 10;
        const int rowHeight = std::max(74, (area.getHeight() - rowGap * (kFeatureCount - 1)) / kFeatureCount);

        for (int i = 0; i < kFeatureCount; ++i)
        {
            auto row = area.removeFromTop(rowHeight);
            if (i < kFeatureCount - 1)
                area.removeFromTop(rowGap);

            g.setColour(juce::Colour(0x201f2937));
            g.fillRoundedRectangle(row.toFloat(), 7.0f);
            g.setColour(juce::Colour(0x40ffffff));
            g.drawRoundedRectangle(row.toFloat(), 7.0f, 1.0f);

            auto title = row.removeFromTop(22).reduced(10, 0);
            g.setColour(juce::Colour(0xffdbeafe));
            g.setFont(14.0f);
            g.drawText(kFeatureNames[static_cast<size_t>(i)], title.removeFromLeft(280), juce::Justification::centredLeft, false);

            g.setColour(kFeatureColours[static_cast<size_t>(i)]);
            g.setFont(14.0f);
            g.drawText(formatFeatureValueText(i, current[static_cast<size_t>(i)]), title, juce::Justification::centredRight, false);

            drawTrace(g,
                      row.reduced(10, 6).toFloat(),
                      history[static_cast<size_t>(i)],
                      makeAutoscaledRange(i,
                                          mins[static_cast<size_t>(i)],
                                          maxs[static_cast<size_t>(i)],
                                          hasBounds[static_cast<size_t>(i)]),
                      kFeatureColours[static_cast<size_t>(i)]);
        }
    }

private:
    std::array<std::vector<float>, kFeatureCount> history;
    std::array<float, kFeatureCount> current {};
    std::array<float, kFeatureCount> mins {};
    std::array<float, kFeatureCount> maxs {};
    std::array<bool, kFeatureCount> hasBounds {};
};

class FeatureStackWindow final : public juce::DocumentWindow
{
public:
    FeatureStackWindow(FeatureStackContent* content, std::function<void()> onClose)
        : juce::DocumentWindow("Feature Trace Stack",
                               juce::Colour(0xff111827),
                               juce::DocumentWindow::allButtons),
          onCloseCallback(std::move(onClose))
    {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        setResizeLimits(640, 360, 2400, 1800);
        setContentOwned(content, true);
        centreWithSize(980, 760);
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        if (onCloseCallback)
            onCloseCallback();
    }

private:
    std::function<void()> onCloseCallback;
};
}

MainComponent::MainComponent()
{
    audioSettingsButton.onClick = [this] { openAudioSettings(); };
    featureStackButton.onClick = [this] { openFeatureStackWindow(); };
    pauseButton.onClick = [this] { toggleScrollingPause(); };
    captureButton.onClick = [this] { toggleCapture(); };
    exportCsvButton.onClick = [this] { exportCaptureCsv(); };

    exportCsvButton.setEnabled(false);

    featureFreqRangeLabel.setText("Feature Range", juce::dontSendNotification);
    featureFreqRangeLabel.setJustificationType(juce::Justification::centredLeft);
    featureFreqRangeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd1d5db));

    featureFreqRangeSlider.setSliderStyle(juce::Slider::TwoValueHorizontal);
    featureFreqRangeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    featureFreqRangeSlider.onValueChange = [this] { applyFeatureFrequencyRangeFromUi(); };

    flatnessPowerFloorLabel.setText("Flatness Floor", juce::dontSendNotification);
    flatnessPowerFloorLabel.setJustificationType(juce::Justification::centredLeft);
    flatnessPowerFloorLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd1d5db));

    flatnessPowerFloorSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    flatnessPowerFloorSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    flatnessPowerFloorSlider.setRange(-180.0, -30.0, 0.1);
    flatnessPowerFloorSlider.setValue(-120.0, juce::dontSendNotification);
    flatnessPowerFloorSlider.onValueChange = [this] { applyFlatnessPowerFloorFromUi(); };

    flatnessPowerFloorValueLabel.setJustificationType(juce::Justification::centredRight);
    flatnessPowerFloorValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xff94a3b8));

    gainLabel.setText("Pre-gain", juce::dontSendNotification);
    gainLabel.setJustificationType(juce::Justification::centredLeft);
    gainLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd1d5db));

    gainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    gainSlider.setRange(-12.0, 12.0, 0.1);
    gainSlider.setValue(0.0, juce::dontSendNotification);
    gainSlider.onValueChange = [this]
    {
        preAnalysisGainLinear.store(
            juce::Decibels::decibelsToGain(static_cast<float>(gainSlider.getValue())),
            std::memory_order_relaxed);
        updateGainLabel();
    };

    gainValueLabel.setJustificationType(juce::Justification::centredRight);
    gainValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xff94a3b8));

    showAverageEnvelopeToggle.setButtonText("Show Avg Envelope");
    showAverageEnvelopeToggle.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffd1d5db));
    showAverageEnvelopeToggle.onClick = [this] { applyShowAverageEnvelopeFromUi(); };

    clearAverageEnvelopeButton.onClick = [this] { clearAverageEnvelope(); };

    showRegressionToggle.setButtonText("Show Regression Fit");
    showRegressionToggle.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffd1d5db));
    showRegressionToggle.onClick = [this] { applyRegressionFromUi(); };

    regressionValueLabel.setJustificationType(juce::Justification::centredLeft);
    regressionValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xff94a3b8));

    minFeatureFreqValueLabel.setJustificationType(juce::Justification::centredRight);
    minFeatureFreqValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xff94a3b8));
    maxFeatureFreqValueLabel.setJustificationType(juce::Justification::centredRight);
    maxFeatureFreqValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xff94a3b8));

    addAndMakeVisible(audioSettingsButton);
    addAndMakeVisible(featureStackButton);
    addAndMakeVisible(pauseButton);
    addAndMakeVisible(captureButton);
    addAndMakeVisible(exportCsvButton);
    addAndMakeVisible(featureFreqRangeLabel);
    addAndMakeVisible(featureFreqRangeSlider);
    addAndMakeVisible(minFeatureFreqValueLabel);
    addAndMakeVisible(maxFeatureFreqValueLabel);
    addAndMakeVisible(flatnessPowerFloorLabel);
    addAndMakeVisible(flatnessPowerFloorSlider);
    addAndMakeVisible(flatnessPowerFloorValueLabel);
    addAndMakeVisible(gainLabel);
    addAndMakeVisible(gainSlider);
    addAndMakeVisible(gainValueLabel);
    addAndMakeVisible(showAverageEnvelopeToggle);
    addAndMakeVisible(clearAverageEnvelopeButton);
    addAndMakeVisible(showRegressionToggle);
    addAndMakeVisible(regressionValueLabel);
    addAndMakeVisible(spectralDisplay);

    for (auto& history : featureHistory)
        history.reserve(featureHistoryLength + 16);

    resetAutoscaleBounds();

    featureFreqRangeSlider.setRange(0.0, currentNyquistHz, 1.0);
    featureFreqRangeSlider.setMinValue(0.0, juce::dontSendNotification, false);
    featureFreqRangeSlider.setMaxValue(currentNyquistHz, juce::dontSendNotification, false);
    featureFreqRangeSlider.setSkewFactorFromMidPoint(std::sqrt(currentNyquistHz));
    applyFeatureFrequencyRangeFromUi();
    applyFlatnessPowerFloorFromUi();
    updateGainLabel();
    applyShowAverageEnvelopeFromUi();
    applyRegressionFromUi();
    updateRegressionLabel();

    setSize(1320, 760);

    // 2 input channels (analysis source), 2 output channels (kept silent).
    setAudioChannels(2, 2);
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    featureStackWindow.reset();
    shutdownAudio();
}

void MainComponent::prepareToPlay(int /*samplesPerBlockExpected*/, double sampleRate)
{
    spectralDisplay.setSampleRate(static_cast<float>(sampleRate));

    const float newNyquist = std::max(1.0f, static_cast<float>(sampleRate) * 0.5f);
    const bool wasFullRange = featureFreqRangeSlider.getMaxValue() >= (currentNyquistHz - 1.0f);
    const double previousMin = featureFreqRangeSlider.getMinValue();
    const double previousMax = featureFreqRangeSlider.getMaxValue();

    currentNyquistHz = newNyquist;
    featureFreqRangeSlider.setRange(0.0, currentNyquistHz, 1.0);
    featureFreqRangeSlider.setMinValue(std::clamp(previousMin, 0.0, static_cast<double>(currentNyquistHz)),
                                       juce::dontSendNotification,
                                       false);
    featureFreqRangeSlider.setMaxValue(wasFullRange
                                           ? static_cast<double>(currentNyquistHz)
                                           : std::clamp(previousMax, 0.0, static_cast<double>(currentNyquistHz)),
                                       juce::dontSendNotification,
                                       false);
    featureFreqRangeSlider.setSkewFactorFromMidPoint(std::sqrt(currentNyquistHz));

    applyFeatureFrequencyRangeFromUi();
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    auto* buffer = bufferToFill.buffer;
    const int startSample = bufferToFill.startSample;
    const int numSamples  = bufferToFill.numSamples;
    const int numInputs   = buffer->getNumChannels();

    if (numInputs > 0 && numSamples > 0)
    {
        // Mono-sum the available input channels into a scratch buffer, then
        // feed the spectral display.
        monoScratch.resize(static_cast<size_t>(numSamples));

        const float* first = buffer->getReadPointer(0, startSample);
        for (int i = 0; i < numSamples; ++i)
            monoScratch[static_cast<size_t>(i)] = first[i];

        for (int ch = 1; ch < numInputs; ++ch)
        {
            const float* in = buffer->getReadPointer(ch, startSample);
            for (int i = 0; i < numSamples; ++i)
                monoScratch[static_cast<size_t>(i)] += in[i];
        }

        if (numInputs > 1)
        {
            const float scale = 1.0f / static_cast<float>(numInputs);
            for (int i = 0; i < numSamples; ++i)
                monoScratch[static_cast<size_t>(i)] *= scale;
        }

        const float gain = preAnalysisGainLinear.load(std::memory_order_relaxed);
        if (gain != 1.0f)
        {
            for (int i = 0; i < numSamples; ++i)
                monoScratch[static_cast<size_t>(i)] *= gain;
        }

        spectralDisplay.pushSamples(monoScratch.data(), numSamples);
    }

    // Do not echo the input to the output; keep the device silent.
    bufferToFill.clearActiveBufferRegion();
}

void MainComponent::releaseResources()
{
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0c0c0c));

    g.setColour(juce::Colour(0x88ffffff));
    g.setFont(14.0f);
    g.drawText("Realtime audio spectrum", 16, 12, 280, 24, juce::Justification::centredLeft, false);

    g.setColour(juce::Colour(0x30ffffff));
    g.drawVerticalLine(featurePanelBounds.getX() - 8,
                       static_cast<float>(featurePanelBounds.getY()),
                       static_cast<float>(featurePanelBounds.getBottom()));

    paintFeaturePanel(g, featurePanelBounds);
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds().reduced(12);
    auto header = bounds.removeFromTop(40);

    exportCsvButton.setBounds(header.removeFromRight(124));
    header.removeFromRight(8);
    captureButton.setBounds(header.removeFromRight(124));
    header.removeFromRight(8);
    pauseButton.setBounds(header.removeFromRight(88));
    header.removeFromRight(8);
    featureStackButton.setBounds(header.removeFromRight(132));
    header.removeFromRight(8);
    audioSettingsButton.setBounds(header.removeFromRight(148));

    auto rangeRow = bounds.removeFromTop(34);
    featureFreqRangeLabel.setBounds(rangeRow.removeFromLeft(108));
    minFeatureFreqValueLabel.setBounds(rangeRow.removeFromLeft(72));
    rangeRow.removeFromLeft(12);
    featureFreqRangeSlider.setBounds(rangeRow.removeFromLeft(410));
    rangeRow.removeFromLeft(12);
    maxFeatureFreqValueLabel.setBounds(rangeRow.removeFromLeft(72));

    auto floorRow = bounds.removeFromTop(34);
    flatnessPowerFloorLabel.setBounds(floorRow.removeFromLeft(108));
    flatnessPowerFloorValueLabel.setBounds(floorRow.removeFromLeft(72));
    floorRow.removeFromLeft(12);
    flatnessPowerFloorSlider.setBounds(floorRow.removeFromLeft(410));

    auto gainRow = bounds.removeFromTop(34);
    gainLabel.setBounds(gainRow.removeFromLeft(108));
    gainValueLabel.setBounds(gainRow.removeFromLeft(72));
    gainRow.removeFromLeft(12);
    gainSlider.setBounds(gainRow.removeFromLeft(410));

    auto analysisRow = bounds.removeFromTop(34);
    showAverageEnvelopeToggle.setBounds(analysisRow.removeFromLeft(170));
    analysisRow.removeFromLeft(8);
    clearAverageEnvelopeButton.setBounds(analysisRow.removeFromLeft(100));
    analysisRow.removeFromLeft(12);
    showRegressionToggle.setBounds(analysisRow.removeFromLeft(160));
    analysisRow.removeFromLeft(12);
    regressionValueLabel.setBounds(analysisRow);

    bounds.removeFromTop(6);

    featurePanelBounds = bounds.removeFromRight(360);
    spectralDisplay.setBounds(bounds.reduced(0, 0));
}

void MainComponent::applyFeatureFrequencyRangeFromUi()
{
    const float appliedMin = static_cast<float>(featureFreqRangeSlider.getMinValue());
    const float appliedMax = static_cast<float>(featureFreqRangeSlider.getMaxValue());
    spectralDisplay.setFeatureFrequencyRange(appliedMin, appliedMax);
    updateFrequencyRangeLabels();
    updateRegressionLabel();
}

void MainComponent::applyFlatnessPowerFloorFromUi()
{
    const float floorDb = static_cast<float>(flatnessPowerFloorSlider.getValue());
    spectralDisplay.setFlatnessPowerFloorDb(floorDb);
    updateFlatnessPowerFloorLabel();
}

void MainComponent::applyShowAverageEnvelopeFromUi()
{
    spectralDisplay.setShowAverageEnvelope(showAverageEnvelopeToggle.getToggleState());
}

void MainComponent::clearAverageEnvelope()
{
    spectralDisplay.clearAverageEnvelope();
    updateRegressionLabel();
}

void MainComponent::applyRegressionFromUi()
{
    spectralDisplay.setShowRegressionLine(showRegressionToggle.getToggleState());
    updateRegressionLabel();
}

void MainComponent::updateFrequencyRangeLabels()
{
    const auto formatHz = [](float hz) -> juce::String
    {
        if (hz >= 1000.0f)
            return juce::String(hz / 1000.0f, 2) + " kHz";
        return juce::String(hz, 0) + " Hz";
    };

    minFeatureFreqValueLabel.setText(formatHz(static_cast<float>(featureFreqRangeSlider.getMinValue())),
                                     juce::dontSendNotification);
    maxFeatureFreqValueLabel.setText(formatHz(static_cast<float>(featureFreqRangeSlider.getMaxValue())),
                                     juce::dontSendNotification);
}

void MainComponent::updateFlatnessPowerFloorLabel()
{
    flatnessPowerFloorValueLabel.setText(juce::String(static_cast<float>(flatnessPowerFloorSlider.getValue()), 1) + " dB",
                                         juce::dontSendNotification);
}

void MainComponent::updateRegressionLabel()
{
    const auto formatHz = [](float hz) -> juce::String
    {
        if (hz >= 1000.0f)
            return juce::String(hz / 1000.0f, 2) + " kHz";
        return juce::String(hz, 0) + " Hz";
    };

    if (!showRegressionToggle.getToggleState())
    {
        regressionValueLabel.setText("Regression: off", juce::dontSendNotification);
        return;
    }

    const auto fit = spectralDisplay.getLatestRegressionResult();
    if (!fit.valid)
    {
        regressionValueLabel.setText("Regression: collecting avg envelope...", juce::dontSendNotification);
        return;
    }

    juce::String text;
    text << "Fit slope " << juce::String(fit.slopeDbPerDecade, 2)
         << " dB/dec, R^2 " << juce::String(fit.rSquared, 3)
            << " [" << formatHz(fit.minHz)
            << " to " << formatHz(fit.maxHz)
         << "]";
    regressionValueLabel.setText(text, juce::dontSendNotification);
}

void MainComponent::openAudioSettings()
{
    if (audioSettingsWindow != nullptr)
    {
        audioSettingsWindow->toFront(true);
        return;
    }

    auto* selector = new juce::AudioDeviceSelectorComponent(deviceManager,
                                                            0,
                                                            256,
                                                            0,
                                                            256,
                                                            true,
                                                            true,
                                                            true,
                                                            false);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector);
    options.dialogTitle = "Audio Settings";
    options.dialogBackgroundColour = juce::Colour(0xff1a1a1a);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;

    if (auto* topLevel = getTopLevelComponent())
        options.componentToCentreAround = topLevel;
    else
        options.componentToCentreAround = this;

    audioSettingsWindow.reset(options.launchAsync());

    if (audioSettingsWindow != nullptr)
    {
        audioSettingsWindow->setAlwaysOnTop(true);
        audioSettingsWindow->setSize(520, 460);
        audioSettingsWindow->enterModalState(true,
                                             juce::ModalCallbackFunction::create([this](int)
                                             {
                                                 audioSettingsWindow.reset();
                                             }),
                                             true);
    }
}

void MainComponent::openFeatureStackWindow()
{
    if (featureStackWindow != nullptr)
    {
        featureStackWindow->toFront(true);
        return;
    }

    auto* content = new FeatureStackContent();
    content->setData(featureHistory, displayedValues, runningMinValues, runningMaxValues, hasAutoscaleBounds);
    featureStackContent = content;

    featureStackWindow = std::make_unique<FeatureStackWindow>(content, [this]
    {
        featureStackContent = nullptr;
        featureStackWindow.reset();
    });
}

void MainComponent::toggleCapture()
{
    captureEnabled = !captureEnabled;
    captureButton.setButtonText(captureEnabled ? "Stop Capture" : "Start Capture");

    if (captureEnabled)
    {
        captureRows.clear();
        resetAutoscaleBounds();
        captureStartMs = juce::Time::getMillisecondCounterHiRes();
        exportCsvButton.setEnabled(false);
    }
    else
    {
        exportCsvButton.setEnabled(!captureRows.empty());
    }
}

void MainComponent::exportCaptureCsv()
{
    if (captureRows.empty())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                               "Export CSV",
                                               "No captured data is available yet. Start capture first.");
        return;
    }

    exportChooser = std::make_unique<juce::FileChooser>("Export feature capture CSV",
                                                         juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                                             .getChildFile("spex_feature_capture.csv"),
                                                         "*.csv");

    const int flags = juce::FileBrowserComponent::saveMode
                    | juce::FileBrowserComponent::canSelectFiles
                    | juce::FileBrowserComponent::warnAboutOverwriting;

    exportChooser->launchAsync(flags, [this](const juce::FileChooser& chooser)
    {
        const auto target = chooser.getResult();
        if (target == juce::File())
            return;

        juce::StringArray lines;
        juce::String header = "time_seconds";
        for (int i = 0; i < featureCount; ++i)
            header << "," << csvHeaderName(i);
        lines.add(header);

        for (const auto& row : captureRows)
        {
            juce::String line;
            line << juce::String(row.timeSeconds, 6);

            for (int i = 0; i < featureCount; ++i)
            {
                const float value = row.values[static_cast<size_t>(i)];
                if (std::isfinite(value))
                    line << "," << juce::String(value, 6);
                else
                    line << ",nan";
            }

            lines.add(line);
        }

        if (!target.replaceWithText(lines.joinIntoString("\n")))
        {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                   "Export CSV",
                                                   "Failed to write CSV file.");
        }

        exportChooser.reset();
    });
}

void MainComponent::updateFeatureState(const spex::SpectralFeatureSnapshot& snapshot)
{
    for (int i = 0; i < featureCount; ++i)
    {
        const size_t index = static_cast<size_t>(i);
        const float raw = snapshot.values[index];

        if (!std::isfinite(raw))
        {
            displayedValues[index] = raw;
            auto& history = featureHistory[index];
            history.push_back(raw);

            if (history.size() > static_cast<size_t>(featureHistoryLength))
                history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(history.size() - featureHistoryLength));

            continue;
        }

        if (!hasDisplayedValues[index])
        {
            hasDisplayedValues[index] = true;
            filteredValues[index] = raw;
            displayedValues[index] = raw;
        }
        else
        {
            filteredValues[index] = kSmoothingAlpha[index] * filteredValues[index]
                                  + (1.0f - kSmoothingAlpha[index]) * raw;

            if (std::abs(filteredValues[index] - displayedValues[index]) >= kHysteresis[index])
                displayedValues[index] = filteredValues[index];
        }

        auto& history = featureHistory[index];
        history.push_back(displayedValues[index]);

        if (!hasAutoscaleBounds[index])
        {
            hasAutoscaleBounds[index] = true;
            runningMinValues[index] = displayedValues[index];
            runningMaxValues[index] = displayedValues[index];
        }
        else
        {
            runningMinValues[index] = std::min(runningMinValues[index], displayedValues[index]);
            runningMaxValues[index] = std::max(runningMaxValues[index], displayedValues[index]);
        }

        if (history.size() > static_cast<size_t>(featureHistoryLength))
            history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(history.size() - featureHistoryLength));
    }
}

void MainComponent::resetAutoscaleBounds()
{
    hasAutoscaleBounds.fill(false);
    runningMinValues.fill(0.0f);
    runningMaxValues.fill(0.0f);
}

juce::String MainComponent::formatFeatureValue(int featureIndex, float value) const
{
    return formatFeatureValueText(featureIndex, value);
}

void MainComponent::paintFeaturePanel(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    auto panel = bounds.reduced(8);
    g.setColour(juce::Colour(0x14111827));
    g.fillRoundedRectangle(panel.toFloat(), 8.0f);

    g.setColour(juce::Colour(0x40ffffff));
    g.drawRoundedRectangle(panel.toFloat(), 8.0f, 1.0f);

    panel.reduce(10, 10);
    const int rowGap = 8;
    const int rowHeight = std::max(62, (panel.getHeight() - rowGap * (featureCount - 1)) / featureCount);

    for (int i = 0; i < featureCount; ++i)
    {
        auto row = panel.removeFromTop(rowHeight);
        if (i < featureCount - 1)
            panel.removeFromTop(rowGap);

        g.setColour(juce::Colour(0x1cffffff));
        g.fillRoundedRectangle(row.toFloat(), 6.0f);

        auto title = row.removeFromTop(20).reduced(8, 0);
        g.setColour(juce::Colour(0xffe5e7eb));
        g.setFont(13.0f);
        g.drawText(kFeatureNames[static_cast<size_t>(i)], title.removeFromLeft(220), juce::Justification::centredLeft, false);

        g.setColour(kFeatureColours[static_cast<size_t>(i)]);
        g.setFont(13.0f);
        g.drawText(formatFeatureValue(i, displayedValues[static_cast<size_t>(i)]),
                   title,
                   juce::Justification::centredRight,
                   false);

        drawTrace(g,
                  row.reduced(8, 5).toFloat(),
                  featureHistory[static_cast<size_t>(i)],
                  makeAutoscaledRange(i,
                                      runningMinValues[static_cast<size_t>(i)],
                                      runningMaxValues[static_cast<size_t>(i)],
                                      hasAutoscaleBounds[static_cast<size_t>(i)]),
                  kFeatureColours[static_cast<size_t>(i)]);
    }
}

void MainComponent::toggleScrollingPause()
{
    scrollingPaused = !scrollingPaused;
    spectralDisplay.setScrollingPaused(scrollingPaused);
    pauseButton.setButtonText(scrollingPaused ? "Resume" : "Pause");
}

void MainComponent::updateGainLabel()
{
    const float db = static_cast<float>(gainSlider.getValue());
    const juce::String text = (db >= 0.0f ? "+" : "") + juce::String(db, 1) + " dB";
    gainValueLabel.setText(text, juce::dontSendNotification);
}

void MainComponent::timerCallback()
{
    if (!spectralDisplay.update())
        return;

    updateRegressionLabel();

    if (!scrollingPaused)
    {
        const auto snapshot = spectralDisplay.getLatestFeatureSnapshot();
        updateFeatureState(snapshot);

        if (captureEnabled)
        {
            CaptureRow row;
            row.timeSeconds = (juce::Time::getMillisecondCounterHiRes() - captureStartMs) * 0.001;
            row.values = displayedValues;
            captureRows.push_back(row);
        }

        if (!captureEnabled)
            exportCsvButton.setEnabled(!captureRows.empty());

        if (featureStackContent != nullptr)
        {
            static_cast<FeatureStackContent*>(featureStackContent)->setData(featureHistory,
                                                                           displayedValues,
                                                                           runningMinValues,
                                                                           runningMaxValues,
                                                                           hasAutoscaleBounds);
            featureStackContent->repaint();
        }

        repaint();
    }
}
