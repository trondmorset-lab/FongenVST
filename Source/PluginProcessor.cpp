#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;

static NormalisableRange<float> logRange (float lo, float hi)
{
    return { lo, hi,
             [] (float s, float e, float t) { return s * std::pow (e / s, t); },
             [] (float s, float e, float v) { return std::log (v / s) / std::log (e / s); },
             [] (float s, float e, float v) { return jlimit (s, e, v); } };
}

AudioProcessorValueTreeState::ParameterLayout FongenAudioProcessor::createLayout()
{
    AudioProcessorValueTreeState::ParameterLayout layout;
    const StringArray types { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch" };
    const int   defType[] = { 3, 1, 0, 0, 0, 2 };
    const float defFreq[] = { 40, 120, 400, 1500, 4000, 10000 };
    const float defQ[]    = { 0.71f, 0.71f, 1, 1, 1, 0.71f };

    auto ms = [] (float v, int) { return v < 10 ? String (v, 1) + " ms" : String (roundToInt (v)) + " ms"; };
    auto hz = [] (float v, int) { return v < 1000 ? String (roundToInt (v)) + " Hz" : String (v / 1000.0f, 2) + " kHz"; };
    auto db = [] (float v, int) { return String (v, 1) + " dB"; };

    for (int b = 0; b < fongen::kNumBands; ++b)
    {
        auto grp = std::make_unique<AudioProcessorParameterGroup> ("band" + String (b + 1), "Band " + String (b + 1), "|");
        auto pid = [b] (const char* n) { return ParameterID { ids::band (b, n), 1 }; };
        const String pre = "B" + String (b + 1) + " ";
        grp->addChild (std::make_unique<AudioParameterBool>   (pid (ids::on),   pre + "On", false));
        grp->addChild (std::make_unique<AudioParameterChoice> (pid (ids::type), pre + "Type", types, defType[b]));
        grp->addChild (std::make_unique<AudioParameterFloat>  (pid (ids::freq), pre + "Freq", logRange (20, 20000), defFreq[b],
                                                               AudioParameterFloatAttributes().withStringFromValueFunction (hz)));
        grp->addChild (std::make_unique<AudioParameterFloat>  (pid (ids::gain), pre + "Gain", NormalisableRange<float> (-24, 24, 0.01f), 0,
                                                               AudioParameterFloatAttributes().withStringFromValueFunction (db)));
        grp->addChild (std::make_unique<AudioParameterFloat>  (pid (ids::q),    pre + "Q", logRange (0.1f, 18), defQ[b],
                                                               AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 2); })));
        grp->addChild (std::make_unique<AudioParameterFloat>  (pid (ids::range), pre + "Dyn Range", NormalisableRange<float> (-24, 24, 0.01f), 0,
                                                               AudioParameterFloatAttributes().withStringFromValueFunction (db)));
        grp->addChild (std::make_unique<AudioParameterFloat>  (pid (ids::thresh), pre + "Threshold", NormalisableRange<float> (-80, 0, 0.1f), -30,
                                                               AudioParameterFloatAttributes().withStringFromValueFunction (db)));
        grp->addChild (std::make_unique<AudioParameterFloat>  (pid (ids::attack), pre + "Attack", logRange (0.1f, 200), 1,
                                                               AudioParameterFloatAttributes().withStringFromValueFunction (ms)));
        grp->addChild (std::make_unique<AudioParameterFloat>  (pid (ids::release), pre + "Release", logRange (5, 2000), 80,
                                                               AudioParameterFloatAttributes().withStringFromValueFunction (ms)));
        layout.add (std::move (grp));
    }

    auto res = std::make_unique<AudioProcessorParameterGroup> ("res", "Resonance", "|");
    res->addChild (std::make_unique<AudioParameterBool>  (ParameterID { ids::resOn, 1 }, "Res On", false));
    res->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ids::resDepth, 1 }, "Res Depth", NormalisableRange<float> (0, 18, 0.01f), 6,
                                                          AudioParameterFloatAttributes().withStringFromValueFunction (db)));
    res->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ids::resSelect, 1 }, "Res Selectivity", NormalisableRange<float> (0, 12, 0.01f), 3,
                                                          AudioParameterFloatAttributes().withStringFromValueFunction (db)));
    res->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ids::resSharp, 1 }, "Res Sharpness", NormalisableRange<float> (0, 100, 0.1f), 50,
                                                          AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " %"; })));
    res->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ids::resAttack, 1 }, "Res Attack", logRange (0.2f, 50), 2,
                                                          AudioParameterFloatAttributes().withStringFromValueFunction (ms)));
    res->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ids::resRelease, 1 }, "Res Release", logRange (5, 500), 60,
                                                          AudioParameterFloatAttributes().withStringFromValueFunction (ms)));
    res->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ids::resLow, 1 }, "Res Low", logRange (50, 18000), 200,
                                                          AudioParameterFloatAttributes().withStringFromValueFunction (hz)));
    res->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ids::resHigh, 1 }, "Res High", logRange (50, 18000), 12000,
                                                          AudioParameterFloatAttributes().withStringFromValueFunction (hz)));
    layout.add (std::move (res));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ids::output, 1 }, "Output", NormalisableRange<float> (-24, 24, 0.01f), 0,
                                                       AudioParameterFloatAttributes().withStringFromValueFunction (db)));
    return layout;
}

FongenAudioProcessor::FongenAudioProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "FongenDynEQ", createLayout())
{
    const char* names[9] = { ids::on, ids::type, ids::freq, ids::gain, ids::q, ids::range, ids::thresh, ids::attack, ids::release };
    for (int b = 0; b < fongen::kNumBands; ++b)
        for (int i = 0; i < 9; ++i)
            bandPtrs[(size_t) b].p[i] = apvts.getRawParameterValue (ids::band (b, names[i]));

    const String resIds[8] = { ids::resOn, ids::resDepth, ids::resSelect, ids::resSharp, ids::resAttack, ids::resRelease, ids::resLow, ids::resHigh };
    for (int i = 0; i < 8; ++i) resPtrs[i] = apvts.getRawParameterValue (resIds[i]);
    outputPtr = apvts.getRawParameterValue (ids::output);

    for (auto& g : bandGainDb) g.store (0);
    for (auto& r : resReductionDb) r.store (0);
}

bool FongenAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == AudioChannelSet::mono() || out == AudioChannelSet::stereo())
        && out == layouts.getMainInputChannelSet();
}

void FongenAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate.store (sampleRate);
    for (auto& b : bands) b.prepare (sampleRate);
    res.prepare (sampleRate);

    outGain.prepare ({ sampleRate, (uint32) jmax (1, samplesPerBlock), 2 });
    outGain.setRampDurationSeconds (0.05);
    outGain.setGainDecibels (outputPtr->load());
    outGain.reset();

    scratchSize = jmax (512, samplesPerBlock);
    monoScratch.allocate ((size_t) scratchSize, true);
    setLatencySamples (0);   // alt er IIR: ingen latens, ingen bypass-forsinkelse nødvendig
}

void FongenAudioProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int numCh = jmin (2, getTotalNumInputChannels());
    const int n = buffer.getNumSamples();
    for (int c = numCh; c < getTotalNumOutputChannels(); ++c) buffer.clear (c, 0, n);
    if (n == 0 || numCh == 0) return;

    // Parametere én gang per blokk
    for (int b = 0; b < fongen::kNumBands; ++b)
    {
        auto& pp = bandPtrs[(size_t) b].p;
        fongen::BandParams bp;
        bp.on = pp[0]->load() > 0.5f;
        bp.type = (fongen::BandType) jlimit (0, 5, (int) pp[1]->load());
        bp.freq = pp[2]->load(); bp.gainDb = pp[3]->load(); bp.q = pp[4]->load();
        bp.rangeDb = pp[5]->load(); bp.thresholdDb = pp[6]->load();
        bp.attackMs = pp[7]->load(); bp.releaseMs = pp[8]->load();
        bands[(size_t) b].setParams (bp);
    }
    {
        fongen::ResParams rp;
        rp.on = resPtrs[0]->load() > 0.5f; rp.depthDb = resPtrs[1]->load(); rp.selectivityDb = resPtrs[2]->load();
        rp.sharpness = resPtrs[3]->load() * 0.01f; rp.attackMs = resPtrs[4]->load(); rp.releaseMs = resPtrs[5]->load();
        rp.lowHz = resPtrs[6]->load(); rp.highHz = resPtrs[7]->load();
        res.setParams (rp);
    }

    auto pushMono = [&] (AnalyserFifo& fifo)
    {
        for (int start = 0; start < n; start += scratchSize)
        {
            const int len = jmin (scratchSize, n - start);
            FloatVectorOperations::copy (monoScratch.get(), buffer.getReadPointer (0, start), len);
            if (numCh > 1)
            {
                FloatVectorOperations::add (monoScratch.get(), buffer.getReadPointer (1, start), len);
                FloatVectorOperations::multiply (monoScratch.get(), 0.5f, len);
            }
            fifo.push (monoScratch.get(), len);
        }
    };
    pushMono (preFifo);

    float* ch[2] = { buffer.getWritePointer (0), numCh > 1 ? buffer.getWritePointer (1) : nullptr };
    for (int i = 0; i < n; ++i)
    {
        float x[2] = { ch[0][i], numCh > 1 ? ch[1][i] : 0.0f };
        res.processFrame (x, numCh);
        for (auto& b : bands) b.processFrame (x, numCh);
        ch[0][i] = x[0];
        if (numCh > 1) ch[1][i] = x[1];
    }

    outGain.setGainDecibels (outputPtr->load());
    dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), (size_t) numCh, (size_t) n);
    outGain.process (dsp::ProcessContextReplacing<float> (block));

    pushMono (postFifo);

    for (int b = 0; b < fongen::kNumBands; ++b) bandGainDb[(size_t) b].store (bands[(size_t) b].getCurrentGainDb(), std::memory_order_relaxed);
    for (int b = 0; b < fongen::kNumResBands; ++b) resReductionDb[(size_t) b].store (res.getReductionDb (b), std::memory_order_relaxed);
}

AudioProcessorEditor* FongenAudioProcessor::createEditor() { return new FongenAudioProcessorEditor (*this); }

void FongenAudioProcessor::getStateInformation (MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("stateVersion", 1, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void FongenAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (ValueTree::fromXml (*xml));
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new FongenAudioProcessor(); }
