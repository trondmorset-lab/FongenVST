#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "PluginProcessor.h"

namespace ui
{
    const juce::Colour bg      { 0xff0f1215 };
    const juce::Colour panel   { 0xff171b1f };
    const juce::Colour grid    { 0xff252b31 };
    const juce::Colour gridHi  { 0xff323a42 };
    const juce::Colour text    { 0xffc5cdd4 };
    const juce::Colour dim     { 0xff6d7781 };
    const juce::Colour curve   { 0xffeef3f6 };
    const juce::Colour post    { 0xff4fb7c9 };
    const juce::Colour resCol  { 0xffe8923a };
    inline juce::Colour band (int b)
    {
        static const juce::uint32 c[] = { 0xffe25c5c, 0xffe6a33e, 0xffd7d44a, 0xff59c97b, 0xff4f9de0, 0xffb07ae6 };
        return juce::Colour (c[b % 6]);
    }
}

class FongenLookAndFeel : public juce::LookAndFeel_V4
{
public:
    FongenLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
};

//==============================================================================
// Sanntids-RTA + EQ-kurver + dragbare noder
class SpectrumDisplay : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumDisplay (FongenAudioProcessor&);
    ~SpectrumDisplay() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMove (const juce::MouseEvent&) override;

    std::function<void (int)> onBandSelected;
    void setSelectedBand (int b) { selected = b; repaint(); }

    static constexpr float kEqRangeDb = 18.0f;
    static constexpr float kRtaTop = 0.0f, kRtaBottom = -96.0f;

private:
    void timerCallback() override;
    void pullAnalyser (AnalyserFifo&, std::vector<float>& ring, int& writePos);
    void computeSpectrum (const std::vector<float>& ring, int writePos, std::vector<float>& outDb);

    float xToFreq (float x) const;
    float freqToX (float f) const;
    float eqDbToY (float db) const;
    float yToEqDb (float y) const;
    float rtaDbToY (float db) const;
    juce::Rectangle<float> plot() const;

    juce::RangedAudioParameter* param (int band, const char* name) const;
    float value (int band, const char* name) const;
    int hitNode (juce::Point<float>) const;
    juce::Point<float> nodePos (int band) const;

    FongenAudioProcessor& proc;
    static constexpr int kOrder = 12, kSize = 1 << kOrder;
    juce::dsp::FFT fft { kOrder };
    std::vector<float> window, fftBuf, preRing, postRing, scratch, preDb, postDb, binDbTmp;
    int preWrite = 0, postWrite = 0;
    std::vector<float> preDisp, postDisp;   // per x-punkt, glattet i tid
    std::vector<float> xFreqs;

    int selected = 0, dragging = -1, hover = -1;
    juce::Point<float> dragStart;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumDisplay)
};

//==============================================================================
class FongenAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit FongenAudioProcessorEditor (FongenAudioProcessor&);
    ~FongenAudioProcessorEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttachment  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label  label;
        std::unique_ptr<SliderAttachment> att;
    };

    void timerCallback() override;
    void selectBand (int b);
    void setupKnob (Knob&, const juce::String& name);
    void layoutKnobs (juce::Rectangle<int> area, Knob* const* knobs, int n);

    FongenAudioProcessor& proc;
    FongenLookAndFeel lnf;
    SpectrumDisplay display;

    juce::TextButton bandButtons[fongen::kNumBands];
    juce::ToggleButton bandOn { "On" };
    juce::ComboBox bandType;
    Knob kFreq, kGain, kQ, kRange, kThresh, kAttack, kRelease;
    std::unique_ptr<ButtonAttachment> bandOnAtt;
    std::unique_ptr<ComboAttachment> bandTypeAtt;

    juce::ToggleButton resOn { "Resonance" };
    Knob rDepth, rSelect, rSharp, rAttack, rRelease, rLow, rHigh, kOutput;
    std::unique_ptr<ButtonAttachment> resOnAtt;

    int selected = 0;
    juce::Rectangle<int> bandPanel, resPanel;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FongenAudioProcessorEditor)
};
