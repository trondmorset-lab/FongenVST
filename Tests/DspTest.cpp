// Måler DSP-kjernen direkte: filterrespons, dynamikk, reaksjonstid, resonansdemper og CPU.
#include "../Source/DynEqDsp.h"
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

using namespace fjord;
static int failures = 0;
static void check (bool ok, const char* what, double val = 0)
{
    std::printf ("  [%s] %s (%.2f)\n", ok ? "OK" : "FEIL", what, val);
    if (! ok) ++failures;
}

static const double fs = 48000.0;

// RMS-gain i dB for en sinus gjennom én band, målt etter innsvinging
static double sineGainDb (const BandParams& bp, float freq, float ampDb)
{
    DynamicBand band; band.prepare (fs); band.setParams (bp);
    const float amp = std::pow (10.0f, ampDb / 20.0f);
    double si = 0, so = 0;
    const int n = (int) fs;
    for (int i = 0; i < n; ++i)
    {
        float x[2]; x[0] = x[1] = amp * (float) std::sin (2 * kPi * freq * i / fs);
        const float in = x[0];
        band.processFrame (x, 2);
        if (i > n / 2) { si += in * in; so += x[0] * x[0]; }
    }
    return 10 * std::log10 (so / si);
}

int main()
{
    std::printf ("Statiske filtre (målt mot analytisk kurve som GUI-et tegner)\n");
    struct Case { BandType t; float fc, g, q, f; const char* name; };
    const Case cases[] = {
        { BandType::Bell,      1000, 6,  1.0f, 1000, "Bell +6 dB @1k, målt ved 1k" },
        { BandType::Bell,      1000, 6,  1.0f, 100,  "Bell +6 dB @1k, målt ved 100 Hz" },
        { BandType::Bell,      3000, -9, 4.0f, 3000, "Bell -9 dB @3k Q4" },
        { BandType::LowShelf,  200,  6,  0.7f, 40,   "Low shelf +6 dB @200, målt ved 40 Hz" },
        { BandType::HighShelf, 5000, -6, 0.7f, 16000,"High shelf -6 dB @5k, målt ved 16k" },
        { BandType::LowCut,    100,  0,  0.707f, 25, "Low cut 100 Hz, målt ved 25 Hz" },
        { BandType::HighCut,   2000, 0,  0.707f, 8000,"High cut 2k, målt ved 8k" },
        { BandType::Notch,     1000, 0,  4.0f, 1000, "Notch 1k" },
    };
    for (auto& c : cases)
    {
        BandParams bp; bp.on = true; bp.type = c.t; bp.freq = c.fc; bp.gainDb = c.g; bp.q = c.q;
        const double measured = sineGainDb (bp, c.f, -12);
        const double analytic = magnitudeDb (c.t, c.fc, c.g, c.q, c.f, fs);
        std::printf ("    %-40s målt %7.2f dB  analytisk %7.2f dB\n", c.name, measured, analytic);
        if (analytic < -60) check (measured < -60, "  dyp demping (begge under -60 dB)", measured);
        else                check (std::abs (measured - analytic) < 0.15, "  målt = analytisk", measured - analytic);
    }

    std::printf ("\nDynamisk bånd (bell 1 kHz, range -12 dB, terskel -30 dBFS)\n");
    {
        BandParams bp; bp.on = true; bp.freq = 1000; bp.q = 1; bp.rangeDb = -12; bp.thresholdDb = -30;
        const double loud  = sineGainDb (bp, 1000, -6);
        const double quiet = sineGainDb (bp, 1000, -50);
        const double off   = sineGainDb (bp, 200, -6);
        check (loud < -11.5 && loud > -12.5, "Høyt signal (-6 dBFS) får fullt kutt ~ -12 dB", loud);
        check (std::abs (quiet) < 0.1, "Lavt signal (-50 dBFS) passerer uendret", quiet);
        check (off > -2.0, "Høyt signal utenfor båndet (200 Hz) påvirkes lite", off);

        bp.rangeDb = +6; bp.thresholdDb = -40;
        const double up = sineGainDb (bp, 1000, -10);
        check (up > 5.5 && up < 6.5, "Positiv range løfter når signalet er over terskel", up);
    }

    std::printf ("\nReaksjonstid (attack 0,5 ms): tid fra signalstart til 90 %% av kuttet\n");
    {
        BandParams bp; bp.on = true; bp.freq = 2000; bp.q = 1; bp.rangeDb = -12; bp.thresholdDb = -40; bp.attackMs = 0.5f;
        DynamicBand band; band.prepare (fs); band.setParams (bp);
        int hit = -1;
        for (int i = 0; i < (int) (fs * 0.2); ++i)
        {
            float x[2]; x[0] = x[1] = (i < 4800 ? 0.0f : 0.5f) * (float) std::sin (2 * kPi * 2000 * i / fs);
            band.processFrame (x, 2);
            if (i >= 4800 && hit < 0 && band.getCurrentGainDb() < -10.8f) hit = i - 4800;
        }
        const double ms = hit / fs * 1000.0;
        check (hit > 0 && ms < 3.0, "Kuttet er på plass innen 3 ms", ms);
    }

    std::printf ("\nResonansdemper: rosa støy + resonans ved 2,5 kHz\n");
    {
        auto run = [] (bool withTone, bool on, double& toneOut, double& noiseBandOut, double& totalOut)
        {
            ResonanceSuppressor rs; rs.prepare (fs);
            ResParams rp; rp.on = on; rp.depthDb = 12; rp.selectivityDb = 3; rp.sharpness = 0.5f; rp.lowHz = 100; rp.highHz = 15000;
            rs.setParams (rp);
            // Analysefiltre: smalt bånd rundt tonen, og et bånd ved 600 Hz for å se om resten av spekteret er urørt
            DynamicBand meterT, meterN;
            BandParams mt; mt.on = true; mt.type = BandType::Bell; mt.freq = 2500; mt.q = 30; mt.gainDb = 24;  // brukes ikke til lyd
            std::mt19937 rng (3); std::normal_distribution<float> nd (0, 1);
            float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;   // Paul Kellet rosa støy
            SvfCoeffs bpT, bpN; bpT.set ((float) std::tan (kPi * 2500 / fs), 1 / 20.0f); bpT.m0 = 0; bpT.m1 = 1 / 20.0f; bpT.m2 = 0;
            bpN.set ((float) std::tan (kPi * 600 / fs), 1 / 4.0f); bpN.m0 = 0; bpN.m1 = 1 / 4.0f; bpN.m2 = 0;
            SvfState sT, sN; double eT = 0, eN = 0, eA = 0;
            for (int i = 0; i < (int) (fs * 3); ++i)
            {
                const float w = nd (rng) * 0.05f;
                b0 = 0.99886f * b0 + w * 0.0555179f; b1 = 0.99332f * b1 + w * 0.0750759f; b2 = 0.96900f * b2 + w * 0.1538520f;
                b3 = 0.86650f * b3 + w * 0.3104856f; b4 = 0.55000f * b4 + w * 0.5329522f; b5 = -0.7616f * b5 - w * 0.0168980f;
                float pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f; b6 = w * 0.115926f;
                float x[2]; x[0] = x[1] = pink + (withTone ? 0.3f * (float) std::sin (2 * kPi * 2500 * i / fs) : 0.0f);
                rs.processFrame (x, 2);
                const float t = sT.process (x[0], bpT), nn = sN.process (x[0], bpN);
                if (i > fs) { eT += t * t; eN += nn * nn; eA += x[0] * x[0]; }
            }
            toneOut = 10 * std::log10 (eT); noiseBandOut = 10 * std::log10 (eN); totalOut = 10 * std::log10 (eA);
            (void) mt; (void) meterT; (void) meterN;
        };
        double tOff, nOff, aOff, tOn, nOn, aOn, t2, n2, a2off, t3, n3, a2on;
        run (true, false, tOff, nOff, aOff);
        run (true, true,  tOn,  nOn,  aOn);
        run (false, false, t2, n2, a2off);
        run (false, true,  t3, n3, a2on);
        check (tOn - tOff < -6.0, "Resonansen dempes minst 6 dB", tOn - tOff);
        check (std::abs (nOn - nOff) < 1.0, "Støyen ved 600 Hz er tilnærmet urørt", nOn - nOff);
        check (std::abs (a2on - a2off) < 1.5, "Ren rosa støy: totalnivå endres under 1,5 dB", a2on - a2off);
    }

    std::printf ("\nCPU: 6 dynamiske bånd + resonansdemper, stereo 48 kHz\n");
    {
        DynamicBand bands[kNumBands]; ResonanceSuppressor rs;
        rs.prepare (fs);
        ResParams rp; rp.on = true; rs.setParams (rp);
        for (int b = 0; b < kNumBands; ++b)
        {
            BandParams bp; bp.on = true; bp.freq = 100.0f * (float) std::pow (2.0, b * 1.4); bp.q = 1.5f; bp.gainDb = 2;
            bp.rangeDb = -6; bp.thresholdDb = -30; bp.type = b == 0 ? BandType::LowShelf : (b == 5 ? BandType::HighShelf : BandType::Bell);
            bands[b].prepare (fs); bands[b].setParams (bp);
        }
        std::mt19937 rng (1); std::uniform_real_distribution<float> ud (-0.5f, 0.5f);
        const int n = (int) fs * 10;
        std::vector<float> noise (4096); for (auto& v : noise) v = ud (rng);
        const auto t0 = std::chrono::steady_clock::now();
        float acc = 0;
        for (int i = 0; i < n; ++i)
        {
            float x[2] = { noise[(size_t) (i & 4095)], noise[(size_t) ((i * 7) & 4095)] };
            rs.processFrame (x, 2);
            for (auto& b : bands) b.processFrame (x, 2);
            acc += x[0];
        }
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("    10 s lyd prosessert på %.3f s (%.1f %% av én kjerne)  [%f]\n", sec, sec * 10.0, acc * 0);
        check (sec < 2.0, "Under 20 % CPU på én kjerne", sec * 10);
    }

    std::printf (failures == 0 ? "\nALLE DSP-TESTER BESTÅTT\n" : "\nNOEN DSP-TESTER FEILET\n");
    return failures == 0 ? 0 : 1;
}
