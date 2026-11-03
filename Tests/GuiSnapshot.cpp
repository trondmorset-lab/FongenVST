// Rendrer plugin-editoren til PNG med realistisk innstilling og lyd i analysatoren
#include "../Source/PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::unique_ptr<juce::AudioProcessor> proc (createPluginFilter());
    auto* p = dynamic_cast<FjordAudioProcessor*> (proc.get());

    auto set = [&] (const juce::String& id, float v)
    {
        if (auto* prm = p->apvts.getParameter (id)) prm->setValueNotifyingHost (prm->convertTo0to1 (v));
    };
    // Et typisk vokal-oppsett
    set ("b1_on", 1); set ("b1_type", 3); set ("b1_freq", 70);
    set ("b2_on", 1); set ("b2_type", 0); set ("b2_freq", 280); set ("b2_gain", -2.5f); set ("b2_q", 1.2f);
    set ("b2_range", -6); set ("b2_thresh", -38);
    set ("b3_on", 1); set ("b3_type", 0); set ("b3_freq", 3200); set ("b3_gain", 3); set ("b3_q", 0.9f);
    set ("b4_on", 1); set ("b4_type", 0); set ("b4_freq", 6800); set ("b4_gain", 0); set ("b4_q", 3.5f);
    set ("b4_range", -9); set ("b4_thresh", -45); set ("b4_attack", 0.3f);
    set ("b6_on", 1); set ("b6_type", 2); set ("b6_freq", 11000); set ("b6_gain", 2.5f);
    set ("res_on", 1); set ("res_depth", 8);

    const double fs = 48000; const int bs = 512;
    proc->setPlayConfigDetails (2, 2, fs, bs);
    proc->prepareToPlay (fs, bs);
    juce::AudioBuffer<float> buf (2, bs); juce::MidiBuffer midi; juce::Random rng (5);
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0; long n = 0;
    auto feed = [&]
    {
        for (int i = 0; i < bs; ++i, ++n)
        {
            const float w = (rng.nextFloat() * 2 - 1) * 0.03f;
            b0 = 0.99886f * b0 + w * 0.0555179f; b1 = 0.99332f * b1 + w * 0.0750759f; b2 = 0.96900f * b2 + w * 0.1538520f;
            b3 = 0.86650f * b3 + w * 0.3104856f; b4 = 0.55000f * b4 + w * 0.5329522f; b5 = -0.7616f * b5 - w * 0.0168980f;
            float pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f; b6 = w * 0.115926f;
            const double t = (double) n / fs;
            // "Vokal": grunntone 180 Hz med overtoner + en skarp resonans ved 2,9 kHz og sibilans ved 6,8 kHz
            float v = pink;
            for (int h = 1; h <= 12; ++h) v += 0.08f / (float) h * (float) std::sin (2 * juce::MathConstants<double>::pi * 180 * h * t);
            v += 0.06f * (float) std::sin (2 * juce::MathConstants<double>::pi * 2900 * t);
            v += 0.05f * (float) std::sin (2 * juce::MathConstants<double>::pi * 6800 * t) * (float) (0.5 + 0.5 * std::sin (t * 9));
            buf.setSample (0, i, v); buf.setSample (1, i, v * 0.95f);
        }
        proc->processBlock (buf, midi);
    };
    for (int i = 0; i < 60; ++i) feed();

    std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditorIfNeeded());
    for (int i = 0; i < 120; ++i) { feed(); feed(); juce::MessageManager::getInstance()->runDispatchLoopUntil (16); }
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
    juce::File out (argc > 1 ? argv[1] : "snapshot.png");
    out.deleteFile();
    juce::FileOutputStream os (out);
    juce::PNGImageFormat().writeImageToStream (img, os);
    proc->editorBeingDeleted (ed.get());
    ed.reset();
    return 0;
}
