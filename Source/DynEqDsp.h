#pragma once
// Fjord DynEQ – DSP-kjerne (ingen JUCE-avhengighet, testes separat i Tests/DspTest.cpp)
//
// Alt er IIR og null latens:
//  * Seks dynamiske EQ-bånd basert på Simpers trapesintegrerte SVF. Gainen oppdateres
//    per sample, så den dynamiske responsen er like rask som detektoren (ned mot 0,1 ms).
//  * En "soothe"-aktig resonansdemper: 64 log-fordelte detektorbånd sammenligner hvert
//    bånd med naboene sine og senker bare det som stikker opp over den glatte spekterkurven.
//  * Ingen allokering etter prepare(), ingen låser.
#include <array>
#include <atomic>
#include <cmath>
#include <algorithm>

#if defined(__clang__) || defined(__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wsign-conversion"
 #pragma GCC diagnostic ignored "-Wswitch-enum"
#endif

namespace fjord
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kNumBands = 6;
constexpr int kMaxChannels = 2;
constexpr int kNumResBands = 64;
constexpr float kResLowHz = 50.0f, kResHighHz = 18000.0f;

enum class BandType { Bell = 0, LowShelf, HighShelf, LowCut, HighCut, Notch };

inline float dbToA (float db) noexcept { return std::pow (10.0f, db / 40.0f); }   // A^2 = lineær gain
inline float fastDb (float lin) noexcept { return 20.0f * std::log10 (lin + 1.0e-12f); }

//==============================================================================
// Simper SVF (Cytomic, "SvfLinearTrapOptimised2"). Stabil ved modulasjon per sample.
struct SvfCoeffs
{
    float a1 = 1, a2 = 0, a3 = 0, k = 1, m0 = 1, m1 = 0, m2 = 0;

    void set (float g, float kk) noexcept
    {
        k = kk;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
};

struct SvfState
{
    float ic1 = 0, ic2 = 0;
    void reset() noexcept { ic1 = ic2 = 0; }

    // Returnerer m0*v0 + m1*v1 + m2*v2
    inline float process (float v0, const SvfCoeffs& c) noexcept
    {
        const float v3 = v0 - ic2;
        const float v1 = c.a1 * ic1 + c.a2 * v3;
        const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
    }
};

// Analytisk respons (brukes av GUI og tester). SVF-en er en bilineær transform med
// forvrenging ved fc, så den digitale responsen = analog prototype ved s = j*tan(pi f/fs)/tan(pi fc/fs).
inline float magnitudeDb (BandType type, float fc, float gainDb, float q, float f, double fs) noexcept
{
    const double w  = std::tan (kPi * std::min ((double) f,  fs * 0.4999) / fs)
                    / std::tan (kPi * std::min ((double) fc, fs * 0.4999) / fs);
    const double A  = std::pow (10.0, gainDb / 40.0);
    const double k  = 1.0 / q;
    auto mag = [] (double re1, double im1, double re2, double im2)
    { return std::sqrt ((re1 * re1 + im1 * im1) / (re2 * re2 + im2 * im2)); };

    double m = 1.0;
    switch (type)
    {
        case BandType::Bell:      m = mag (1 - w * w, w * A * k, 1 - w * w, w * k / A); break;
        case BandType::Notch:     m = mag (1 - w * w, 0.0,       1 - w * w, w * k);     break;
        case BandType::LowShelf:
        {
            const double s = w * std::sqrt (A);
            m = mag (A * A - s * s, k * A * s, 1 - s * s, k * s); break;
        }
        case BandType::HighShelf:
        {
            const double s = w / std::sqrt (A);
            m = mag (1 - A * A * s * s, k * A * s, 1 - s * s, k * s); break;
        }
        case BandType::LowCut:   // 24 dB/okt: to SVF-trinn
        case BandType::HighCut:
        {
            const double qs[2] = { 0.5412 * q / 0.7071, 1.3066 * q / 0.7071 };
            for (double qq : qs)
            {
                const double kk = 1.0 / qq;
                m *= (type == BandType::LowCut) ? mag (-w * w, 0.0, 1 - w * w, w * kk)
                                                : mag (1.0,    0.0, 1 - w * w, w * kk);
            }
            break;
        }
    }
    return (float) (20.0 * std::log10 (std::max (m, 1.0e-9)));
}

//==============================================================================
struct BandParams
{
    bool  on = false;
    BandType type = BandType::Bell;
    float freq = 1000, gainDb = 0, q = 1;
    float rangeDb = 0;          // dynamisk område: negativ = kutt når signalet går over terskel, positiv = løft
    float thresholdDb = -24;
    float attackMs = 1, releaseMs = 80;
};

class DynamicBand
{
public:
    void prepare (double sampleRate) noexcept
    {
        fs = sampleRate;
        staticCoef = std::exp (-1.0f / (0.020f * (float) fs));    // 20 ms glatting av statisk gain
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : state) for (auto& st : s) st.reset();
        for (auto& d : det) d.reset();
        env = 0; smoothedStatic = p.gainDb; currentGainDb = p.gainDb; first = true;
    }

    // Kalles én gang per blokk med nye parametere
    void setParams (const BandParams& np) noexcept
    {
        if (np.type != p.type || np.on != p.on) { for (auto& s : state) for (auto& st : s) st.reset(); for (auto& d : det) d.reset(); env = 0; }
        p = np;
        if (first) { smoothedStatic = p.gainDb; first = false; }

        const float fc = std::clamp (p.freq, 10.0f, (float) (fs * 0.49));
        g0 = (float) std::tan (kPi * fc / fs);
        k  = 1.0f / std::max (0.05f, p.q);

        attCoef = std::exp (-1.0f / (std::max (0.05f, p.attackMs)  * 0.001f * (float) fs));
        relCoef = std::exp (-1.0f / (std::max (1.0f,  p.releaseMs) * 0.001f * (float) fs));

        // Detektor: båndpass for bell/notch, lavpass/høypass for hyller
        detC.set (g0, k);
        detC.m0 = 0; detC.m1 = 0; detC.m2 = 0;
        switch (p.type)
        {
            case BandType::LowShelf:  detC.m2 = 1; break;                                     // lavpass
            case BandType::HighShelf: detC.m0 = 1; detC.m1 = -k; detC.m2 = -1; break;         // høypass
            default:                  detC.m1 = k; break;                                     // båndpass, 0 dB på topp
        }

        // Kuttfiltre (statiske, 24 dB/okt)
        if (p.type == BandType::LowCut || p.type == BandType::HighCut)
        {
            const float qs[2] = { 0.5412f * p.q / 0.7071f, 1.3066f * p.q / 0.7071f };
            for (int i = 0; i < 2; ++i)
            {
                cutC[i].set (g0, 1.0f / std::max (0.05f, qs[i]));
                if (p.type == BandType::LowCut) { cutC[i].m0 = 1; cutC[i].m1 = -cutC[i].k; cutC[i].m2 = -1; }
                else                            { cutC[i].m0 = 0; cutC[i].m1 = 0;          cutC[i].m2 = 1; }
            }
        }
        if (p.type == BandType::Notch)
        {
            mainC.set (g0, k); mainC.m0 = 1; mainC.m1 = -k; mainC.m2 = 0;
        }
    }

    bool isDynamic() const noexcept
    {
        return p.on && std::abs (p.rangeDb) > 0.01f
            && (p.type == BandType::Bell || p.type == BandType::LowShelf || p.type == BandType::HighShelf);
    }

    // Prosesserer én sample-ramme for alle kanaler (linket detektor)
    inline void processFrame (float* x, int numCh) noexcept
    {
        if (! p.on) return;

        if (p.type == BandType::LowCut || p.type == BandType::HighCut)
        {
            for (int c = 0; c < numCh; ++c)
                x[c] = state[c][1].process (state[c][0].process (x[c], cutC[0]), cutC[1]);
            return;
        }
        if (p.type == BandType::Notch)
        {
            for (int c = 0; c < numCh; ++c) x[c] = state[c][0].process (x[c], mainC);
            return;
        }

        smoothedStatic = p.gainDb + staticCoef * (smoothedStatic - p.gainDb);
        float gainDb = smoothedStatic;

        if (isDynamic())
        {
            float peak = 0;
            for (int c = 0; c < numCh; ++c) peak = std::max (peak, std::abs (det[c].process (x[c], detC)));
            env = peak > env ? peak + attCoef * (env - peak) : peak + relCoef * (env - peak);
            const float over = fastDb (env) - p.thresholdDb;
            if (over > 0)
            {
                // Myk overgang: fullt område nås når signalet er max(6, |range|) dB over terskel
                const float span = std::max (6.0f, std::abs (p.rangeDb));
                const float t = std::min (1.0f, over / span);
                gainDb += p.rangeDb * t * t * (3.0f - 2.0f * t);   // smoothstep
            }
        }
        currentGainDb = gainDb;

        // Koeffisienter for denne samplen (billig: ingen tan, bare én pow og én divisjon)
        SvfCoeffs c;
        switch (p.type)
        {
            case BandType::Bell:
            {
                const float A = dbToA (gainDb);
                c.set (g0, k / A);
                c.m0 = 1; c.m1 = c.k * (A * A - 1); c.m2 = 0;
                break;
            }
            case BandType::LowShelf:
            {
                const float sA = std::pow (10.0f, gainDb / 80.0f), A = sA * sA;
                c.set (g0 / sA, k);
                c.m0 = 1; c.m1 = k * (A - 1); c.m2 = A * A - 1;
                break;
            }
            case BandType::HighShelf:
            {
                const float sA = std::pow (10.0f, gainDb / 80.0f), A = sA * sA;
                c.set (g0 * sA, k);
                c.m0 = A * A; c.m1 = k * (1 - A) * A; c.m2 = 1 - A * A;
                break;
            }
            default: break;
        }
        for (int ch = 0; ch < numCh; ++ch) x[ch] = state[ch][0].process (x[ch], c);
    }

    float getCurrentGainDb() const noexcept { return currentGainDb; }
    const BandParams& params() const noexcept { return p; }

private:
    double fs = 48000;
    BandParams p;
    float g0 = 0.1f, k = 1;
    SvfCoeffs detC, mainC, cutC[2];
    std::array<std::array<SvfState, 2>, kMaxChannels> state {};
    std::array<SvfState, kMaxChannels> det {};
    float env = 0, attCoef = 0, relCoef = 0, staticCoef = 0, smoothedStatic = 0, currentGainDb = 0;
    bool first = true;
};

//==============================================================================
struct ResParams
{
    bool  on = false;
    float depthDb = 6;        // maks kutt per bånd
    float selectivityDb = 3;  // hvor mye et bånd må stikke opp over naboene før det kuttes
    float sharpness = 0.5f;   // 0..1: bredere eller smalere kutt
    float attackMs = 2, releaseMs = 60;
    float lowHz = 200, highHz = 12000;
};

class ResonanceSuppressor
{
public:
    static constexpr int kUpdate = 4;   // koeffisientoppdatering hvert 4. sample (~0,08 ms ved 48 kHz)

    void prepare (double sampleRate) noexcept
    {
        fs = sampleRate;
        for (int b = 0; b < kNumResBands; ++b)
        {
            const float t = (float) b / (kNumResBands - 1);
            centre[b] = kResLowHz * std::pow (kResHighHz / kResLowHz, t);
            const float fc = std::min (centre[b], (float) (fs * 0.45));
            valid[b] = centre[b] < fs * 0.45;
            g[b] = (float) std::tan (kPi * fc / fs);
            detC[b].set (g[b], 1.0f / kDetQ);
            detC[b].m0 = 0; detC[b].m1 = 1.0f / kDetQ; detC[b].m2 = 0;
        }
        reset();
    }

    void reset() noexcept
    {
        for (auto& d : det) d.reset();
        for (auto& ch : cut) for (auto& s : ch) s.reset();
        ms.fill (0); redDb.fill (0); envDb.fill (-120);
        counter = 0;
        for (int b = 0; b < kNumResBands; ++b) setCut (b, 0);
    }

    void setParams (const ResParams& np) noexcept
    {
        if (np.on && ! p.on) reset();
        p = np;
        cutK = 1.0f / (4.0f + 12.0f * p.sharpness);                // Q 4..16
        halfWindow = 6 - (int) std::lround (3.0f * p.sharpness);   // 6..3 bånd på hver side
        // Middelverdien av kvadratet glattes; tidskonstant minst to perioder av båndet
        for (int b = 0; b < kNumResBands; ++b)
        {
            const float minT = 2.0f / centre[b];
            attC[b] = std::exp (-1.0f / (std::max (p.attackMs * 0.001f, minT) * (float) fs));
            relC[b] = std::exp (-1.0f / (std::max (p.releaseMs * 0.001f, minT * 2) * (float) fs));
        }
    }

    inline void processFrame (float* x, int numCh) noexcept
    {
        if (! p.on) return;

        // 1) Detektor (feed-forward på inngangen, linket mellom kanalene)
        const float mono = numCh > 1 ? 0.5f * (x[0] + x[1]) : x[0];
        const float side = numCh > 1 ? 0.5f * (x[0] - x[1]) : 0.0f;
        for (int b = 0; b < kNumResBands; ++b)
        {
            if (! valid[b]) continue;
            const float y = det[b].process (mono, detC[b]);
            const float y2 = numCh > 1 ? detS[b].process (side, detC[b]) : 0.0f;
            const float e = y * y + y2 * y2;
            ms[b] = e > ms[b] ? e + attC[b] * (ms[b] - e) : e + relC[b] * (ms[b] - e);
        }

        // 2) Hvert kUpdate-sample: finn bånd som stikker opp over naboene og sett kutt
        if (++counter >= kUpdate)
        {
            counter = 0;
            for (int b = 0; b < kNumResBands; ++b) envDb[b] = 10.0f * std::log10 (ms[b] + 1.0e-14f);

            for (int b = 0; b < kNumResBands; ++b)
            {
                float target = 0;
                if (valid[b] && envDb[b] > kFloorDb)
                {
                    float sum = 0; int n = 0;
                    for (int j = b - halfWindow; j <= b + halfWindow; ++j)
                        if (j >= 0 && j < kNumResBands && std::abs (j - b) >= 2 && valid[j]) { sum += envDb[j]; ++n; }
                    if (n > 0)
                    {
                        const float prominence = envDb[b] - sum / (float) n;
                        target = std::clamp (1.5f * (prominence - kLeakDb - p.selectivityDb), 0.0f, p.depthDb);
                        target *= rangeWeight (centre[b]);
                    }
                }
                // Ballistikk ligger allerede i detektoren; her bare lett glatting mot klikk
                redDb[b] += 0.5f * (target - redDb[b]);
                setCut (b, -redDb[b]);
            }
        }

        // 3) Kuttfiltre i serie (bell-filtre, minimum fase)
        for (int ch = 0; ch < numCh; ++ch)
        {
            float v = x[ch];
            for (int b = 0; b < kNumResBands; ++b)
                if (valid[b]) v = cut[ch][b].process (v, cutC[b]);   // 0 dB = identitet, prosesseres alltid (ingen klikk)
            x[ch] = v;
        }
    }

    float getReductionDb (int b) const noexcept { return redDb[(size_t) b]; }
    float getCentreHz (int b) const noexcept { return centre[(size_t) b]; }
    float getCutQ() const noexcept { return 1.0f / cutK; }

private:
    static constexpr float kDetQ = 12.0f;
    static constexpr float kFloorDb = -80.0f;
    // Lekkasje fra nabobåndene gjør at et rent sinussignal "løfter" naboene; trekk fra litt
    static constexpr float kLeakDb = 1.0f;

    float rangeWeight (float f) const noexcept
    {
        // Myk overgang over en halv oktav i hver ende av frekvensområdet
        const float lo = std::log2 (f / std::max (20.0f, p.lowHz)) * 2.0f + 1.0f;
        const float hi = std::log2 (std::max (p.lowHz * 1.01f, p.highHz) / f) * 2.0f + 1.0f;
        return std::clamp (std::min (lo, hi), 0.0f, 1.0f);
    }

    void setCut (int b, float gainDb) noexcept
    {
        active[b] = valid[b] && gainDb < -0.01f;
        const float A = dbToA (gainDb);
        cutC[b].set (g[b], cutK / A);
        cutC[b].m0 = 1; cutC[b].m1 = cutC[b].k * (A * A - 1); cutC[b].m2 = 0;
    }

    double fs = 48000;
    ResParams p;
    float cutK = 0.2f; int halfWindow = 3;
    std::array<float, kNumResBands> centre {}, g {}, attC {}, relC {}, ms {}, envDb {}, redDb {};
    std::array<bool, kNumResBands> valid {}, active {};
    std::array<SvfCoeffs, kNumResBands> detC {}, cutC {};
    std::array<SvfState, kNumResBands> det {}, detS {};
    std::array<std::array<SvfState, kNumResBands>, kMaxChannels> cut {};
    int counter = 0;
};

} // namespace fjord

#if defined(__clang__) || defined(__GNUC__)
 #pragma GCC diagnostic pop
#endif
