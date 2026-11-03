#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "DynEqDsp.h"

// Alle parameter-ID-er samlet ett sted
namespace ids
{
    inline juce::String band (int b, const char* name) { return "b" + juce::String (b + 1) + "_" + name; }
    inline const char* on = "on";  inline const char* type = "type"; inline const char* freq = "freq";
    inline const char* gain = "gain"; inline const char* q = "q"; inline const char* range = "range";
    inline const char* thresh = "thresh"; inline const char* attack = "attack"; inline const char* release = "release";

    inline const juce::String resOn = "res_on", resDepth = "res_depth", resSelect = "res_select",
                              resSharp = "res_sharp", resAttack = "res_attack", resRelease = "res_release",
                              resLow = "res_low", resHigh = "res_high", output = "output";
}

// Låsfri ringbuffer (én skriver = lydtråden, én leser = GUI). Fullt = nye samples kastes.
class AnalyserFifo
{
public:
    static constexpr int kSize = 1 << 15;
    void push (const float* data, int n) noexcept
    {
        const int w = write.load (std::memory_order_relaxed), r = read.load (std::memory_order_acquire);
        const int free = kSize - 1 - ((w - r + kSize) & (kSize - 1));
        n = juce::jmin (n, free);
        for (int i = 0; i < n; ++i) buf[(size_t) ((w + i) & (kSize - 1))] = data[i];
        write.store ((w + n) & (kSize - 1), std::memory_order_release);
    }
    int pop (float* dest, int maxN) noexcept
    {
        const int r = read.load (std::memory_order_relaxed), w = write.load (std::memory_order_acquire);
        const int n = juce::jmin (maxN, (w - r + kSize) & (kSize - 1));
        for (int i = 0; i < n; ++i) dest[i] = buf[(size_t) ((r + i) & (kSize - 1))];
        read.store ((r + n) & (kSize - 1), std::memory_order_release);
        return n;
    }
private:
    std::array<float, kSize> buf {};
    std::atomic<int> write { 0 }, read { 0 };
};

class FjordAudioProcessor : public juce::AudioProcessor
{
public:
    FjordAudioProcessor();
    ~FjordAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Fjord DynEQ"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState apvts;

    // Til GUI
    AnalyserFifo preFifo, postFifo;
    std::array<std::atomic<float>, fjord::kNumBands> bandGainDb {};
    std::array<std::atomic<float>, fjord::kNumResBands> resReductionDb {};
    std::atomic<double> currentSampleRate { 48000.0 };
    float getResCentreHz (int b) const noexcept { return res.getCentreHz (b); }
    float getResCutQ() const noexcept { return res.getCutQ(); }

private:
    struct BandPtrs { std::atomic<float>* p[9]; };
    std::array<BandPtrs, fjord::kNumBands> bandPtrs {};
    std::atomic<float>* resPtrs[8] {};
    std::atomic<float>* outputPtr = nullptr;

    std::array<fjord::DynamicBand, fjord::kNumBands> bands;
    fjord::ResonanceSuppressor res;
    juce::dsp::Gain<float> outGain;
    juce::HeapBlock<float> monoScratch;
    int scratchSize = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FjordAudioProcessor)
};
