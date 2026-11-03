#include "PluginEditor.h"
#include "BinaryData.h"

using namespace juce;

static Font uiFont (float size, bool bold = false)
{
    // Standard sans-serif (ikke spesifikke fontnavn: manglende fonter kan krasje tekstformingen på Linux)
    return Font (FontOptions (size, bold ? Font::bold : Font::plain));
}

//==============================================================================
FongenLookAndFeel::FongenLookAndFeel()
{
    setColour (ResizableWindow::backgroundColourId, ui::bg);
    setColour (Slider::textBoxTextColourId, ui::text);
    setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour (Slider::rotarySliderFillColourId, ui::post);
    setColour (Label::textColourId, ui::text);
    setColour (ToggleButton::textColourId, ui::text);
    setColour (ToggleButton::tickColourId, ui::post);
    setColour (ToggleButton::tickDisabledColourId, ui::dim);
    setColour (ComboBox::backgroundColourId, ui::bg);
    setColour (ComboBox::outlineColourId, ui::gridHi);
    setColour (ComboBox::textColourId, ui::text);
    setColour (ComboBox::arrowColourId, ui::dim);
    setColour (PopupMenu::backgroundColourId, ui::panel);
    setColour (PopupMenu::textColourId, ui::text);
    setColour (PopupMenu::highlightedBackgroundColourId, ui::gridHi);
    setColour (TextButton::buttonColourId, ui::bg);
    setColour (TextButton::textColourOffId, ui::dim);
    setColour (TextButton::textColourOnId, ui::bg);
}

Label* FongenLookAndFeel::createSliderTextBox (Slider& s)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (s);
    l->setFont (uiFont (12.0f));
    l->setColour (Label::backgroundColourId, Colours::transparentBlack);
    l->setColour (Label::outlineColourId, Colours::transparentBlack);
    l->setColour (Label::textColourId, ui::text);
    return l;
}

void FongenLookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos, float start, float end, Slider& s)
{
    const auto bounds = Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4);
    const float r = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto c = bounds.getCentre();
    const float angle = start + pos * (end - start);
    const auto fill = s.findColour (Slider::rotarySliderFillColourId);

    g.setColour (ui::bg);
    g.fillEllipse (c.x - r, c.y - r, r * 2, r * 2);

    Path track; track.addCentredArc (c.x, c.y, r - 3, r - 3, 0, start, end, true);
    g.setColour (ui::gridHi);
    g.strokePath (track, PathStrokeType (3.0f, PathStrokeType::curved, PathStrokeType::rounded));

    const bool bipolar = s.getMinimum() < 0 && s.getMaximum() > 0;
    const float from = bipolar ? start + (float) s.valueToProportionOfLength (0.0) * (end - start) : start;
    Path arc; arc.addCentredArc (c.x, c.y, r - 3, r - 3, 0, jmin (from, angle), jmax (from, angle), true);
    g.setColour (s.isEnabled() ? fill : ui::dim);
    g.strokePath (arc, PathStrokeType (3.0f, PathStrokeType::curved, PathStrokeType::rounded));

    g.setColour (ui::panel.brighter (0.15f));
    g.fillEllipse (c.x - r * 0.62f, c.y - r * 0.62f, r * 1.24f, r * 1.24f);
    const Point<float> tip = c.getPointOnCircumference (r * 0.5f, angle);
    g.setColour (ui::curve);
    g.drawLine ({ c.getPointOnCircumference (r * 0.15f, angle), tip }, 2.0f);
}

//==============================================================================
SpectrumDisplay::SpectrumDisplay (FongenAudioProcessor& p) : proc (p)
{
    window.resize (kSize);
    for (int i = 0; i < kSize; ++i) window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * MathConstants<float>::pi * (float) i / (float) kSize);
    fftBuf.resize (kSize * 2);
    preRing.assign (kSize, 0.0f);
    postRing.assign (kSize, 0.0f);
    scratch.resize (AnalyserFifo::kSize);
    preDb.assign (kSize / 2 + 1, -120.0f);
    postDb.assign (kSize / 2 + 1, -120.0f);
    setOpaque (true);
    startTimerHz (60);
}

SpectrumDisplay::~SpectrumDisplay() { stopTimer(); }

Rectangle<float> SpectrumDisplay::plot() const { return getLocalBounds().toFloat().reduced (34, 10).withTrimmedBottom (14); }

float SpectrumDisplay::freqToX (float f) const
{
    auto r = plot();
    return r.getX() + r.getWidth() * std::log (f / 20.0f) / std::log (1000.0f);
}
float SpectrumDisplay::xToFreq (float x) const
{
    auto r = plot();
    return 20.0f * std::pow (1000.0f, jlimit (0.0f, 1.0f, (x - r.getX()) / r.getWidth()));
}
float SpectrumDisplay::eqDbToY (float db) const
{
    auto r = plot();
    return r.getCentreY() - db / kEqRangeDb * r.getHeight() * 0.5f;
}
float SpectrumDisplay::yToEqDb (float y) const
{
    auto r = plot();
    return (r.getCentreY() - y) / (r.getHeight() * 0.5f) * kEqRangeDb;
}
float SpectrumDisplay::rtaDbToY (float db) const
{
    auto r = plot();
    return r.getY() + (kRtaTop - db) / (kRtaTop - kRtaBottom) * r.getHeight();
}

void SpectrumDisplay::resized()
{
    auto r = plot();
    const int n = jmax (16, (int) r.getWidth() / 2);
    xFreqs.resize ((size_t) n);
    for (int i = 0; i < n; ++i) xFreqs[(size_t) i] = 20.0f * std::pow (1000.0f, (float) i / (float) (n - 1));
    preDisp.assign ((size_t) n, -120.0f);
    postDisp.assign ((size_t) n, -120.0f);
}

RangedAudioParameter* SpectrumDisplay::param (int band, const char* name) const
{
    return proc.apvts.getParameter (ids::band (band, name));
}
float SpectrumDisplay::value (int band, const char* name) const
{
    return proc.apvts.getRawParameterValue (ids::band (band, name))->load();
}

//==============================================================================
void SpectrumDisplay::pullAnalyser (AnalyserFifo& fifo, std::vector<float>& ring, int& writePos)
{
    const int n = fifo.pop (scratch.data(), (int) scratch.size());
    const int start = jmax (0, n - kSize);    // bare de siste kSize samplene er interessante
    for (int i = start; i < n; ++i)
    {
        ring[(size_t) writePos] = scratch[(size_t) i];
        writePos = (writePos + 1) & (kSize - 1);
    }
}

void SpectrumDisplay::computeSpectrum (const std::vector<float>& ring, int writePos, std::vector<float>& outDb)
{
    for (int i = 0; i < kSize; ++i) fftBuf[(size_t) i] = ring[(size_t) ((writePos + i) & (kSize - 1))] * window[(size_t) i];
    std::fill (fftBuf.begin() + kSize, fftBuf.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (fftBuf.data(), true);
    const float norm = 2.0f / (kSize * 0.5f);   // Hann: koherent gain 0,5 → sinus med amplitude 1 = 0 dB
    for (int i = 0; i <= kSize / 2; ++i) outDb[(size_t) i] = fftBuf[(size_t) i] * norm;   // lineær amplitude foreløpig
}

void SpectrumDisplay::timerCallback()
{
    pullAnalyser (proc.preFifo, preRing, preWrite);
    pullAnalyser (proc.postFifo, postRing, postWrite);

    const double fs = proc.currentSampleRate.load();
    const float binHz = (float) (fs / kSize);

    auto toDisplay = [&] (const std::vector<float>& ring, int writePos, std::vector<float>& lin, std::vector<float>& disp)
    {
        computeSpectrum (ring, writePos, lin);
        for (size_t i = 0; i < xFreqs.size(); ++i)
        {
            const float f = xFreqs[i];
            // 1/6-oktav glatting: snitt av effekt i bin-området rundt frekvensen
            const float lo = f * 0.944f / binHz, hi = f * 1.059f / binHz;
            float power = 0, peak = 0;
            int b0 = (int) std::floor (lo), b1 = (int) std::ceil (hi);
            b0 = jlimit (1, kSize / 2, b0); b1 = jlimit (b0, kSize / 2, b1);
            if (b1 - b0 < 2)
            {
                // Lavt i spekteret: interpoler mellom to bins
                const float pos = jlimit (1.0f, (float) (kSize / 2 - 1), f / binHz);
                const int i0 = (int) pos; const float t = pos - (float) i0;
                const float a = lin[(size_t) i0] * (1 - t) + lin[(size_t) i0 + 1] * t;
                power = a * a;
            }
            else
            {
                for (int b = b0; b <= b1; ++b) { const float e = lin[(size_t) b] * lin[(size_t) b]; power += e; peak = jmax (peak, e); }
                power /= (float) (b1 - b0 + 1);
                // Blanding av snitt og topp (geometrisk): toner vises med riktig nivå, støy blir ikke oppblåst
                power = std::sqrt (power * peak);
            }
            const float tilt = 4.5f * std::log2 (f / 1000.0f);   // 4,5 dB/okt helning, som i Pro-Q
            const float db = 10.0f * std::log10 (power + 1.0e-20f) + tilt;
            float& d = disp[i];
            d = db > d ? d + 0.7f * (db - d) : d + 0.12f * (db - d);
            d = jmax (d, -150.0f);
        }
    };
    toDisplay (preRing, preWrite, preDb, preDisp);
    toDisplay (postRing, postWrite, postDb, postDisp);
    repaint();
}

//==============================================================================
Point<float> SpectrumDisplay::nodePos (int b) const
{
    const auto type = (fongen::BandType) (int) value (b, ids::type);
    const bool hasGain = type == fongen::BandType::Bell || type == fongen::BandType::LowShelf || type == fongen::BandType::HighShelf;
    return { freqToX (value (b, ids::freq)), eqDbToY (hasGain ? value (b, ids::gain) : 0.0f) };
}

int SpectrumDisplay::hitNode (Point<float> p) const
{
    int best = -1; float bestD = 12.0f;
    for (int b = 0; b < fongen::kNumBands; ++b)
    {
        if (value (b, ids::on) < 0.5f) continue;
        const float d = nodePos (b).getDistanceFrom (p);
        if (d < bestD) { bestD = d; best = b; }
    }
    return best;
}

void SpectrumDisplay::paint (Graphics& g)
{
    g.fillAll (ui::bg);
    const auto r = plot();
    const double fs = proc.currentSampleRate.load();

    // Rutenett
    g.setFont (uiFont (10.5f));
    for (int fi : { 20, 30, 40, 50, 60, 80, 100, 200, 300, 400, 500, 600, 800,
                    1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 20000 })
    {
        const float f = (float) fi;
        const bool major = fi == 100 || fi == 1000 || fi == 10000;
        g.setColour (major ? ui::gridHi : ui::grid);
        const float x = freqToX (f);
        g.drawVerticalLine (roundToInt (x), r.getY(), r.getBottom());
        if (fi == 20 || fi == 50 || fi == 100 || fi == 200 || fi == 500 || fi == 1000 || fi == 2000 || fi == 5000 || fi == 10000 || fi == 20000)
        {
            g.setColour (ui::dim);
            const String s = fi >= 1000 ? String (fi / 1000) + "k" : String (fi);
            g.drawText (s, Rectangle<float> (x - 20, r.getBottom() + 1, 40, 13), Justification::centred);
        }
    }
    for (int dbi = -18; dbi <= 18; dbi += 6)
    {
        const float db = (float) dbi;
        g.setColour (dbi == 0 ? ui::gridHi : ui::grid);
        const float y = eqDbToY (db);
        g.drawHorizontalLine (roundToInt (y), r.getX(), r.getRight());
        g.setColour (ui::dim);
        g.drawText ((dbi > 0 ? "+" : "") + String (dbi), Rectangle<float> (r.getX() - 32, y - 7, 28, 14), Justification::centredRight);
    }
    for (float db = -12; db >= kRtaBottom + 1; db -= 12)
    {
        g.setColour (ui::dim.withAlpha (0.7f));
        g.drawText (String ((int) db), Rectangle<float> (r.getRight() + 3, rtaDbToY (db) - 7, 30, 14), Justification::centredLeft);
    }

    g.saveState();
    g.reduceClipRegion (r.toNearestInt());

    // RTA: inngang (grå fyll) og utgang (blå linje + fyll)
    auto spectrumPath = [&] (const std::vector<float>& disp, bool closed)
    {
        Path p;
        for (size_t i = 0; i < xFreqs.size(); ++i)
        {
            const float x = freqToX (xFreqs[i]);
            const float y = jlimit (r.getY() - 2, r.getBottom() + 2, rtaDbToY (disp[i]));
            if (i == 0) { if (closed) { p.startNewSubPath (x, r.getBottom() + 2); p.lineTo (x, y); } else p.startNewSubPath (x, y); }
            else p.lineTo (x, y);
        }
        if (closed) { p.lineTo (r.getRight(), r.getBottom() + 2); p.closeSubPath(); }
        return p;
    };
    if (! preDisp.empty())
    {
        g.setColour (Colour (0xff8a949e).withAlpha (0.16f));
        g.fillPath (spectrumPath (preDisp, true));
        g.setGradientFill (ColourGradient (ui::post.withAlpha (0.28f), 0, r.getY(), ui::post.withAlpha (0.04f), 0, r.getBottom(), false));
        g.fillPath (spectrumPath (postDisp, true));
        g.setColour (ui::post.withAlpha (0.85f));
        g.strokePath (spectrumPath (postDisp, false), PathStrokeType (1.2f));
    }

    // Resonansdemper: summert kutt (oransje), hengende ned fra 0 dB-linjen
    if (proc.apvts.getRawParameterValue (ids::resOn)->load() > 0.5f)
    {
        float red[fongen::kNumResBands]; float maxRed = 0;
        for (int b = 0; b < fongen::kNumResBands; ++b) { red[b] = proc.resReductionDb[(size_t) b].load(); maxRed = jmax (maxRed, red[b]); }
        if (maxRed > 0.05f)
        {
            Path p; p.startNewSubPath (r.getX(), eqDbToY (0));
            const float q = proc.getResCutQ();
            for (size_t i = 0; i < xFreqs.size(); i += 2)
            {
                float sum = 0;
                for (int b = 0; b < fongen::kNumResBands; ++b)
                    if (red[b] > 0.05f) sum += fongen::magnitudeDb (fongen::BandType::Bell, proc.getResCentreHz (b), -red[b], q, xFreqs[i], fs);
                p.lineTo (freqToX (xFreqs[i]), eqDbToY (sum));
            }
            p.lineTo (r.getRight(), eqDbToY (0)); p.closeSubPath();
            g.setColour (ui::resCol.withAlpha (0.35f)); g.fillPath (p);
            g.setColour (ui::resCol); g.strokePath (p, PathStrokeType (1.0f));
        }
    }

    // EQ-kurver
    struct B { bool on; fongen::BandType type; float f, gStatic, gNow, q; };
    B bands[fongen::kNumBands];
    for (int b = 0; b < fongen::kNumBands; ++b)
    {
        auto& x = bands[b];
        x.on = value (b, ids::on) > 0.5f; x.type = (fongen::BandType) (int) value (b, ids::type);
        x.f = value (b, ids::freq); x.gStatic = value (b, ids::gain); x.q = value (b, ids::q);
        const bool dyn = std::abs (value (b, ids::range)) > 0.01f;
        x.gNow = dyn ? proc.bandGainDb[(size_t) b].load() : x.gStatic;
    }
    auto bandCurve = [&] (int b, bool useNow)
    {
        Path p;
        for (size_t i = 0; i < xFreqs.size(); ++i)
        {
            const auto& x = bands[b];
            const float db = fongen::magnitudeDb (x.type, x.f, useNow ? x.gNow : x.gStatic, x.q, xFreqs[i], fs);
            const float y = jlimit (r.getY() - 4, r.getBottom() + 4, eqDbToY (db));
            if (i == 0) p.startNewSubPath (freqToX (xFreqs[i]), y); else p.lineTo (freqToX (xFreqs[i]), y);
        }
        return p;
    };
    // Valgt bånd: farget fyll mot 0 dB
    if (selected >= 0 && bands[selected].on)
    {
        Path p = bandCurve (selected, true);
        p.lineTo (r.getRight(), eqDbToY (0)); p.lineTo (r.getX(), eqDbToY (0)); p.closeSubPath();
        g.setColour (ui::band (selected).withAlpha (0.18f)); g.fillPath (p);
    }
    for (int b = 0; b < fongen::kNumBands; ++b)
        if (bands[b].on)
        {
            g.setColour (ui::band (b).withAlpha (b == selected ? 0.9f : 0.45f));
            g.strokePath (bandCurve (b, true), PathStrokeType (1.0f));
        }

    // Totalkurve: statisk (stiplet) og faktisk med dynamikk (hel)
    auto totalCurve = [&] (bool useNow)
    {
        Path p;
        for (size_t i = 0; i < xFreqs.size(); ++i)
        {
            float sum = 0;
            for (int b = 0; b < fongen::kNumBands; ++b)
                if (bands[b].on) sum += fongen::magnitudeDb (bands[b].type, bands[b].f, useNow ? bands[b].gNow : bands[b].gStatic, bands[b].q, xFreqs[i], fs);
            const float y = jlimit (r.getY() - 4, r.getBottom() + 4, eqDbToY (sum));
            if (i == 0) p.startNewSubPath (freqToX (xFreqs[i]), y); else p.lineTo (freqToX (xFreqs[i]), y);
        }
        return p;
    };
    {
        Path stat = totalCurve (false), dashed;
        const float dash[] = { 4.0f, 4.0f };
        PathStrokeType (1.2f).createDashedStroke (dashed, stat, dash, 2);
        g.setColour (ui::curve.withAlpha (0.35f)); g.fillPath (dashed);
        g.setColour (ui::curve); g.strokePath (totalCurve (true), PathStrokeType (2.0f));
    }

    // Noder
    for (int b = 0; b < fongen::kNumBands; ++b)
    {
        if (! bands[b].on) continue;
        const auto c = nodePos (b);
        const bool hasGain = bands[b].type == fongen::BandType::Bell || bands[b].type == fongen::BandType::LowShelf || bands[b].type == fongen::BandType::HighShelf;
        if (hasGain && std::abs (bands[b].gNow - bands[b].gStatic) > 0.05f)
        {
            // Dynamisk utslag: linje fra statisk til faktisk gain
            const float yNow = eqDbToY (bands[b].gNow);
            g.setColour (ui::band (b).withAlpha (0.8f));
            g.drawLine (c.x, c.y, c.x, yNow, 3.0f);
            g.fillEllipse (c.x - 3, yNow - 3, 6, 6);
        }
        const float rad = (b == selected || b == hover) ? 8.0f : 6.5f;
        g.setColour (ui::band (b));
        g.fillEllipse (c.x - rad, c.y - rad, rad * 2, rad * 2);
        g.setColour (ui::bg);
        g.setFont (uiFont (10.0f, true));
        g.drawText (String (b + 1), Rectangle<float> (c.x - rad, c.y - rad, rad * 2, rad * 2), Justification::centred);
        if (b == selected) { g.setColour (ui::curve); g.drawEllipse (c.x - rad - 2, c.y - rad - 2, rad * 2 + 4, rad * 2 + 4, 1.2f); }
    }
    g.restoreState();

    // Hjelpetekst
    g.setColour (ui::dim);
    g.setFont (uiFont (10.5f));
    g.drawText (String (CharPointer_UTF8 ("Dobbeltklikk: nytt b\xc3\xa5nd  |  Dra node: frekvens/gain  |  Hjul: Q  |  Dobbeltklikk node: av")),
                r.withHeight (14).translated (6, 2), Justification::centredLeft);
}

//==============================================================================
static void setParamValue (RangedAudioParameter* p, float plainValue)
{
    if (p != nullptr) p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
}

void SpectrumDisplay::mouseMove (const MouseEvent& e)
{
    const int h = hitNode (e.position);
    if (h != hover) { hover = h; repaint(); }
    setMouseCursor (h >= 0 ? MouseCursor::DraggingHandCursor : MouseCursor::NormalCursor);
}

void SpectrumDisplay::mouseDown (const MouseEvent& e)
{
    dragging = hitNode (e.position);
    if (dragging >= 0)
    {
        selected = dragging;
        if (onBandSelected) onBandSelected (dragging);
        param (dragging, ids::freq)->beginChangeGesture();
        param (dragging, ids::gain)->beginChangeGesture();
    }
}

void SpectrumDisplay::mouseDrag (const MouseEvent& e)
{
    if (dragging < 0) return;
    setParamValue (param (dragging, ids::freq), xToFreq (e.position.x));
    const auto type = (fongen::BandType) (int) value (dragging, ids::type);
    if (type == fongen::BandType::Bell || type == fongen::BandType::LowShelf || type == fongen::BandType::HighShelf)
        setParamValue (param (dragging, ids::gain), jlimit (-24.0f, 24.0f, yToEqDb (e.position.y)));
}

void SpectrumDisplay::mouseUp (const MouseEvent&)
{
    if (dragging >= 0)
    {
        param (dragging, ids::freq)->endChangeGesture();
        param (dragging, ids::gain)->endChangeGesture();
    }
    dragging = -1;
}

void SpectrumDisplay::mouseDoubleClick (const MouseEvent& e)
{
    const int h = hitNode (e.position);
    if (h >= 0)
    {
        auto* on = param (h, ids::on);
        on->beginChangeGesture(); setParamValue (on, 0.0f); on->endChangeGesture();
        return;
    }
    for (int b = 0; b < fongen::kNumBands; ++b)
    {
        if (value (b, ids::on) > 0.5f) continue;
        const float f = xToFreq (e.position.x);
        auto* type = param (b, ids::type);
        // Nær kantene foreslås hyller, ellers bell
        const int t = f < 60 ? (int) fongen::BandType::LowShelf : (f > 12000 ? (int) fongen::BandType::HighShelf : (int) fongen::BandType::Bell);
        for (auto* p : { type, param (b, ids::freq), param (b, ids::gain), param (b, ids::on) }) p->beginChangeGesture();
        setParamValue (type, (float) t);
        setParamValue (param (b, ids::freq), f);
        setParamValue (param (b, ids::gain), jlimit (-24.0f, 24.0f, yToEqDb (e.position.y)));
        setParamValue (param (b, ids::on), 1.0f);
        for (auto* p : { type, param (b, ids::freq), param (b, ids::gain), param (b, ids::on) }) p->endChangeGesture();
        selected = b;
        if (onBandSelected) onBandSelected (b);
        return;
    }
}

void SpectrumDisplay::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    int b = hitNode (e.position);
    if (b < 0) b = selected;
    if (b < 0 || value (b, ids::on) < 0.5f) return;
    auto* q = param (b, ids::q);
    q->beginChangeGesture();
    setParamValue (q, jlimit (0.1f, 18.0f, value (b, ids::q) * std::pow (1.2f, w.deltaY * 5.0f)));
    q->endChangeGesture();
}

//==============================================================================
FongenAudioProcessorEditor::FongenAudioProcessorEditor (FongenAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p), display (p)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (display);
    display.onBandSelected = [this] (int b) { selectBand (b); };

    for (int b = 0; b < fongen::kNumBands; ++b)
    {
        auto& btn = bandButtons[b];
        btn.setButtonText (String (b + 1));
        btn.setClickingTogglesState (true);
        btn.setRadioGroupId (1);
        btn.setColour (TextButton::buttonOnColourId, ui::band (b));
        btn.setColour (TextButton::textColourOffId, ui::band (b));
        btn.onClick = [this, b] { if (bandButtons[b].getToggleState()) selectBand (b); };
        addAndMakeVisible (btn);
    }
    addAndMakeVisible (bandOn);
    bandType.addItemList ({ "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch" }, 1);
    addAndMakeVisible (bandType);

    setupKnob (kFreq, "FREQ"); setupKnob (kGain, "GAIN"); setupKnob (kQ, "Q");
    setupKnob (kRange, "DYN RANGE"); setupKnob (kThresh, "THRESHOLD"); setupKnob (kAttack, "ATTACK"); setupKnob (kRelease, "RELEASE");

    addAndMakeVisible (resOn);
    resOn.setColour (ToggleButton::tickColourId, ui::resCol);
    resOnAtt = std::make_unique<ButtonAttachment> (proc.apvts, ids::resOn, resOn);
    struct R { Knob* k; const char* name; String id; };
    for (auto& r : { R { &rDepth, "DEPTH", ids::resDepth }, R { &rSelect, "SELECTIVITY", ids::resSelect },
                     R { &rSharp, "SHARPNESS", ids::resSharp }, R { &rAttack, "ATTACK", ids::resAttack },
                     R { &rRelease, "RELEASE", ids::resRelease }, R { &rLow, "LOW", ids::resLow },
                     R { &rHigh, "HIGH", ids::resHigh } })
    {
        setupKnob (*r.k, r.name);
        r.k->slider.setColour (Slider::rotarySliderFillColourId, ui::resCol);
        r.k->att = std::make_unique<SliderAttachment> (proc.apvts, r.id, r.k->slider);
    }
    setupKnob (kOutput, "OUTPUT");
    kOutput.slider.setColour (Slider::rotarySliderFillColourId, ui::curve);
    kOutput.att = std::make_unique<SliderAttachment> (proc.apvts, ids::output, kOutput.slider);

    // Velg første aktive bånd
    int first = 0;
    for (int b = 0; b < fongen::kNumBands; ++b)
        if (proc.apvts.getRawParameterValue (ids::band (b, ids::on))->load() > 0.5f) { first = b; break; }
    selectBand (first);

    setResizable (true, true);
    setResizeLimits (900, 580, 1800, 1100);
    setSize (1060, 660);
    startTimerHz (10);
}

FongenAudioProcessorEditor::~FongenAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void FongenAudioProcessorEditor::setupKnob (Knob& k, const String& name)
{
    k.slider.setTextBoxStyle (Slider::TextBoxBelow, false, 80, 16);
    k.slider.setPopupDisplayEnabled (false, false, nullptr);
    k.label.setText (name, dontSendNotification);
    k.label.setJustificationType (Justification::centred);
    k.label.setFont (uiFont (10.5f, true));
    k.label.setColour (Label::textColourId, ui::dim);
    addAndMakeVisible (k.slider);
    addAndMakeVisible (k.label);
}

void FongenAudioProcessorEditor::selectBand (int b)
{
    selected = b;
    bandButtons[b].setToggleState (true, dontSendNotification);
    display.setSelectedBand (b);

    // Koble kontrollene om til det valgte båndets parametere
    bandOnAtt.reset(); bandTypeAtt.reset();
    for (auto* k : { &kFreq, &kGain, &kQ, &kRange, &kThresh, &kAttack, &kRelease }) k->att.reset();
    bandOnAtt   = std::make_unique<ButtonAttachment> (proc.apvts, ids::band (b, ids::on), bandOn);
    bandTypeAtt = std::make_unique<ComboAttachment>  (proc.apvts, ids::band (b, ids::type), bandType);
    kFreq.att    = std::make_unique<SliderAttachment> (proc.apvts, ids::band (b, ids::freq), kFreq.slider);
    kGain.att    = std::make_unique<SliderAttachment> (proc.apvts, ids::band (b, ids::gain), kGain.slider);
    kQ.att       = std::make_unique<SliderAttachment> (proc.apvts, ids::band (b, ids::q), kQ.slider);
    kRange.att   = std::make_unique<SliderAttachment> (proc.apvts, ids::band (b, ids::range), kRange.slider);
    kThresh.att  = std::make_unique<SliderAttachment> (proc.apvts, ids::band (b, ids::thresh), kThresh.slider);
    kAttack.att  = std::make_unique<SliderAttachment> (proc.apvts, ids::band (b, ids::attack), kAttack.slider);
    kRelease.att = std::make_unique<SliderAttachment> (proc.apvts, ids::band (b, ids::release), kRelease.slider);
    for (auto* k : { &kFreq, &kGain, &kQ, &kRange, &kThresh, &kAttack, &kRelease })
        k->slider.setColour (Slider::rotarySliderFillColourId, ui::band (b));
    bandOn.setColour (ToggleButton::tickColourId, ui::band (b));
    timerCallback();
    repaint();
}

void FongenAudioProcessorEditor::timerCallback()
{
    // Dim knappene til bånd som er av, og grå ut kontroller som ikke gjelder båndtypen
    for (int b = 0; b < fongen::kNumBands; ++b)
        bandButtons[b].setAlpha (proc.apvts.getRawParameterValue (ids::band (b, ids::on))->load() > 0.5f ? 1.0f : 0.45f);
    const auto type = (fongen::BandType) (int) proc.apvts.getRawParameterValue (ids::band (selected, ids::type))->load();
    const bool hasGain = type == fongen::BandType::Bell || type == fongen::BandType::LowShelf || type == fongen::BandType::HighShelf;
    for (auto* k : { &kGain, &kRange, &kThresh, &kAttack, &kRelease }) { k->slider.setEnabled (hasGain); k->label.setAlpha (hasGain ? 1.0f : 0.4f); }
}

void FongenAudioProcessorEditor::paint (Graphics& g)
{
    g.fillAll (ui::bg);
    auto top = getLocalBounds().removeFromTop (34);
    g.setColour (ui::text);
    g.setFont (uiFont (16.0f, true));
    static const Image logo = ImageCache::getFromMemory (BinaryData::fongenlogo256_png, BinaryData::fongenlogo256_pngSize);
    g.drawImage (logo, Rectangle<float> (12, 4, 26, 26), RectanglePlacement::centred);
    g.drawText ("FONGEN", top.withTrimmedLeft (46), Justification::centredLeft);
    g.setColour (ui::post);
    g.drawText ("DynEQ", top.withTrimmedLeft (114), Justification::centredLeft);
    g.setColour (ui::dim);
    g.setFont (uiFont (11.0f));
    g.drawText (String (CharPointer_UTF8 ("Dynamisk EQ  \xc2\xb7  Resonansdemper  \xc2\xb7  Sanntids-RTA  \xc2\xb7  0 samples latens")), top.withTrimmedRight (16), Justification::centredRight);

    for (auto panelRect : { bandPanel, resPanel })
    {
        g.setColour (ui::panel);
        g.fillRoundedRectangle (panelRect.toFloat(), 6.0f);
    }
    g.setColour (ui::dim);
    g.setFont (uiFont (10.5f, true));
    g.drawText ("BAND", bandPanel.reduced (12, 8).removeFromTop (14), Justification::centredLeft);
}

void FongenAudioProcessorEditor::layoutKnobs (Rectangle<int> area, Knob* const* knobs, int n)
{
    const int w = area.getWidth() / n;
    for (int i = 0; i < n; ++i)
    {
        auto cell = area.removeFromLeft (w).reduced (3, 0);
        knobs[i]->label.setBounds (cell.removeFromTop (14));
        knobs[i]->slider.setBounds (cell);
    }
}

void FongenAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop (34);
    auto bottom = area.removeFromBottom (jmax (200, getHeight() / 3)).reduced (10, 8);
    display.setBounds (area.reduced (8, 0));

    bandPanel = bottom.removeFromLeft ((int) (bottom.getWidth() * 0.58f));
    bottom.removeFromLeft (10);
    resPanel = bottom;

    auto bp = bandPanel.reduced (12, 8);
    auto row = bp.removeFromTop (28).withTrimmedLeft (44);
    for (auto& btn : bandButtons) { btn.setBounds (row.removeFromLeft (30).reduced (2)); }
    row.removeFromLeft (12);
    bandOn.setBounds (row.removeFromLeft (56));
    bandType.setBounds (row.removeFromLeft (120).reduced (0, 2));
    bp.removeFromTop (8);
    Knob* bandKnobs[] = { &kFreq, &kGain, &kQ, &kRange, &kThresh, &kAttack, &kRelease };
    layoutKnobs (bp, bandKnobs, 7);

    auto rp = resPanel.reduced (12, 8);
    resOn.setBounds (rp.removeFromTop (28).removeFromLeft (130));
    rp.removeFromTop (4);
    const int half = rp.getHeight() / 2;
    Knob* row1[] = { &rDepth, &rSelect, &rSharp, &kOutput };
    Knob* row2[] = { &rAttack, &rRelease, &rLow, &rHigh };
    layoutKnobs (rp.removeFromTop (half), row1, 4);
    layoutKnobs (rp, row2, 4);
}
