#include "TapeProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <algorithm>

namespace
{
constexpr double kPi = 3.14159265358979323846;

// 4-point Hermite interpolation from a power-of-two ring buffer.
inline float hermite (const float* b, int64_t i, float f, int64_t m)
{
    const float y0 = b[(i - 1) & m];
    const float y1 = b[ i      & m];
    const float y2 = b[(i + 1) & m];
    const float y3 = b[(i + 2) & m];

    const float c0 = y1;
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + c0;
}

inline float readTape (const float* b, double pos, int64_t m)
{
    const double fl = std::floor (pos);
    return hermite (b, (int64_t) fl, (float) (pos - fl), m);
}
} // namespace

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout TapeProcessor::createLayout()
{
    using P  = juce::AudioParameterFloat;
    using NR = juce::NormalisableRange<float>;
    using Attr = juce::AudioParameterFloatAttributes;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<P> (juce::ParameterID { "speed", 1 }, "Speed",
                                     NR (-24.0f, 12.0f, 0.01f), 0.0f,
                                     Attr().withLabel ("st")));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "stop", 1 },
                                                            "Tape Stop", false));
    layout.add (std::make_unique<P> (juce::ParameterID { "inertia", 1 }, "Motor Inertia",
                                     NR (5.0f, 3000.0f, 1.0f, 0.4f), 150.0f,
                                     Attr().withLabel ("ms")));
    layout.add (std::make_unique<P> (juce::ParameterID { "window", 1 }, "Window",
                                     NR (0.1f, 10.0f, 0.01f, 0.5f), 1.0f,
                                     Attr().withLabel ("s")));
    layout.add (std::make_unique<P> (juce::ParameterID { "drive", 1 }, "Drive",
                                     NR (0.0f, 24.0f, 0.01f), 6.0f,
                                     Attr().withLabel ("dB")));
    layout.add (std::make_unique<P> (juce::ParameterID { "warmth", 1 }, "Warmth",
                                     NR (0.0f, 1.0f, 0.001f), 0.3f));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "oversampling", 1 },
                                                              "Oversampling",
                                                              juce::StringArray { "Off", "2x", "4x", "8x" }, 2));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "autogain", 1 },
                                                            "Auto Gain", true));
    layout.add (std::make_unique<P> (juce::ParameterID { "tone", 1 }, "Tone",
                                     NR (1000.0f, 20000.0f, 1.0f, 0.35f), 12000.0f,
                                     Attr().withLabel ("Hz")));
    layout.add (std::make_unique<P> (juce::ParameterID { "wow", 1 }, "Wow",
                                     NR (0.0f, 1.0f, 0.001f), 0.15f));
    layout.add (std::make_unique<P> (juce::ParameterID { "flutter", 1 }, "Flutter",
                                     NR (0.0f, 1.0f, 0.001f), 0.1f));
    layout.add (std::make_unique<P> (juce::ParameterID { "hiss", 1 }, "Hiss",
                                     NR (0.0f, 1.0f, 0.001f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "mix", 1 }, "Mix",
                                     NR (0.0f, 1.0f, 0.001f), 1.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "output", 1 }, "Output",
                                     NR (-24.0f, 12.0f, 0.01f), 0.0f,
                                     Attr().withLabel ("dB")));
    return layout;
}

TapeProcessor::TapeProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
}

bool TapeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

juce::AudioProcessorEditor* TapeProcessor::createEditor()
{
    return new SelotapeEditor (*this);
}

//==============================================================================
void TapeProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    maxBlock = std::max (1, samplesPerBlock);

    xfadeSamples    = (int) (0.020 * sr);
    minDelaySamples = xfadeSamples + 16;
    latencySamples  = (int) (0.060 * sr);

    // Oversamplers: linear-phase FIR half-band stages, integer latency.
    osLatency[0] = 0;
    for (int i = 0; i < 3; ++i)
    {
        oversamplers[(size_t) i] = std::make_unique<juce::dsp::Oversampling<float>> (
            2, (size_t) (i + 1),
            juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
            true, true);
        oversamplers[(size_t) i]->initProcessing ((size_t) maxBlock);
        oversamplers[(size_t) i]->reset();
        osLatency[i + 1] = (int) std::lround (oversamplers[(size_t) i]->getLatencyInSamples());
    }
    osMaxLatency = osLatency[3];
    lastOrder = -1;

    totalLatency = latencySamples + osMaxLatency;

    satBuf.setSize (2, maxBlock);
    satBuf.clear();
    driveBuf.assign ((size_t) maxBlock, 1.0f);

    const int ringSize = juce::nextPowerOfTwo ((int) (12.0 * sr) + 16);
    const int drySize  = juce::nextPowerOfTwo (totalLatency + 16);
    ringMask = ringSize - 1;
    dryMask  = drySize - 1;

    for (int c = 0; c < 2; ++c)
    {
        ring[(size_t) c].assign ((size_t) ringSize, 0.0f);
        dryRing[(size_t) c].assign ((size_t) drySize, 0.0f);
        chan[(size_t) c] = {};
    }

    const int order = juce::jlimit (0, 3, (int) std::lround (apvts.getRawParameterValue ("oversampling")->load()));
    const double lockTarget = (double) (latencySamples + osMaxLatency - osLatency[order]);

    writeIndex = 0;
    headPos[0] = headPos[1] = -lockTarget;
    activeHead = 0;
    fading = false;
    fadeCount = 0;

    const float st = apvts.getRawParameterValue ("speed")->load();
    curSpeed = std::pow (2.0, (double) st / 12.0);
    liveSpeed.store ((float) curSpeed);

    wowPhase = flutterPhase = 0.0;
    wowNoise = flutterNoise = 0.0;

    // Smoothed random drift: a one-pole lowpassed noise, scaled to a sensible sigma.
    wowNoiseA      = 1.0 - std::exp (-2.0 * kPi * 1.0 / sr);
    flutterNoiseA  = 1.0 - std::exp (-2.0 * kPi * 15.0 / sr);
    wowNoiseScale     = 0.35 / (0.577 * std::sqrt (wowNoiseA / 2.0));
    flutterNoiseScale = 0.35 / (0.577 * std::sqrt (flutterNoiseA / 2.0));

    // Auto gain detectors: ~250 ms RMS, ~50 ms gain smoothing
    inEnv = outEnv = 0.0;
    autoGainSm = 1.0;
    envCoeff = 1.0 - std::exp (-1.0 / (0.250 * sr));
    agCoeff  = 1.0 - std::exp (-1.0 / (0.050 * sr));

    driveSm.reset (sr, 0.02);
    mixSm.reset   (sr, 0.02);
    outSm.reset   (sr, 0.02);
    driveSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("drive")->load()));
    mixSm.setCurrentAndTargetValue   (apvts.getRawParameterValue ("mix")->load());
    outSm.setCurrentAndTargetValue   (juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("output")->load()));

    setLatencySamples (totalLatency);
}

//==============================================================================
void TapeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int nch = std::min (buffer.getNumChannels(), 2);
    const int ns  = buffer.getNumSamples();

    for (int c = nch; c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, ns);

    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    BlockParams bp;

    const float speedSt   = get ("speed");
    const bool  stop      = get ("stop") > 0.5f;
    const float inertiaMs = get ("inertia");
    const float warmth    = get ("warmth");
    const float hiss      = get ("hiss");

    bp.windowSamples = (double) get ("window") * sr;
    bp.toneHz        = get ("tone");
    bp.wowDepth      = get ("wow");
    bp.flutDepth     = get ("flutter");
    bp.autoGain      = get ("autogain") > 0.5f;
    bp.order         = juce::jlimit (0, 3, (int) std::lround (get ("oversampling")));

    bp.targetSpeed  = stop ? 0.0 : std::pow (2.0, (double) speedSt / 12.0);
    bp.inertiaCoeff = 1.0 - std::exp (-1.0 / (std::max (1.0, (double) inertiaMs) * 0.001 * sr));

    bp.bias     = warmth * 0.6f;
    bp.tanhBias = std::tanh (bp.bias);
    bp.biasComp = 1.0f - bp.tanhBias * bp.tanhBias;
    bp.hissGain = hiss > 0.0001f ? juce::Decibels::decibelsToGain (-72.0f + hiss * 30.0f) : 0.0f;

    // Total latency is fixed; shorter oversamplers are padded with extra head delay.
    bp.lockTarget = (double) (latencySamples + osMaxLatency - osLatency[bp.order]);

    if (bp.order != lastOrder)
    {
        if (bp.order > 0)
            oversamplers[(size_t) (bp.order - 1)]->reset();
        lastOrder = bp.order;
    }

    driveSm.setTargetValue (juce::Decibels::decibelsToGain (get ("drive")));
    mixSm.setTargetValue   (get ("mix"));
    outSm.setTargetValue   (juce::Decibels::decibelsToGain (get ("output")));

    // Process in chunks no larger than the size the oversamplers were prepared for.
    int done = 0;
    while (done < ns)
    {
        const int n = std::min (ns - done, maxBlock);
        processChunk (buffer, done, n, bp);
        done += n;
    }

    liveSpeed.store ((float) curSpeed);
}

//==============================================================================
void TapeProcessor::processChunk (juce::AudioBuffer<float>& buffer, int start, int num, const BlockParams& bp)
{
    const int nch = std::min (buffer.getNumChannels(), 2);

    float* data[2] = { nullptr, nullptr };
    for (int c = 0; c < nch; ++c)
        data[c] = buffer.getWritePointer (c) + start;

    // ---------------------------------------------------------------------
    // Pass 1: saturation, optionally oversampled. Result goes to satBuf.
    // ---------------------------------------------------------------------
    for (int s = 0; s < num; ++s)
        driveBuf[(size_t) s] = driveSm.getNextValue();

    const float bias = bp.bias, tanhBias = bp.tanhBias, biasComp = bp.biasComp;

    auto shape = [bias, tanhBias, biasComp] (float x, float g)
    {
        const float comp = 1.0f / (std::max (g, 0.05f) * biasComp); // unity small-signal gain
        return (std::tanh (g * x + bias) - tanhBias) * comp;
    };

    float* satPtrs[2] = { satBuf.getWritePointer (0), satBuf.getWritePointer (1) };

    if (bp.order == 0)
    {
        for (int c = 0; c < nch; ++c)
            for (int s = 0; s < num; ++s)
                satPtrs[c][s] = shape (data[c][s], driveBuf[(size_t) s]);
    }
    else
    {
        auto& os = *oversamplers[(size_t) (bp.order - 1)];

        juce::dsp::AudioBlock<float> inBlock (data, (size_t) nch, (size_t) num);
        auto upBlock = os.processSamplesUp (inBlock);

        const size_t upNum = (size_t) num << bp.order;
        for (int c = 0; c < nch; ++c)
        {
            float* p = upBlock.getChannelPointer ((size_t) c);
            for (size_t j = 0; j < upNum; ++j)
                p[j] = shape (p[j], driveBuf[j >> bp.order]);
        }

        juce::dsp::AudioBlock<float> outBlock (satPtrs, (size_t) nch, (size_t) num);
        os.processSamplesDown (outBlock);
    }

    // ---------------------------------------------------------------------
    // Pass 2: DC block, auto gain, tape write, varispeed read, mix.
    // ---------------------------------------------------------------------
    for (int s = 0; s < num; ++s)
    {
        const float mixv    = mixSm.getNextValue();
        const float outGain = outSm.getNextValue();

        // ---- motor / varispeed with inertia
        curSpeed += bp.inertiaCoeff * (bp.targetSpeed - curSpeed);

        // ---- wow & flutter (modulate transport speed)
        wowNoise     += wowNoiseA     * ((double) (rng.nextFloat() * 2.0f - 1.0f) - wowNoise);
        flutterNoise += flutterNoiseA * ((double) (rng.nextFloat() * 2.0f - 1.0f) - flutterNoise);

        const double wowMod = (std::sin (wowPhase) * 0.7 + wowNoise * wowNoiseScale * 0.3);
        const double fltMod = (std::sin (flutterPhase) * 0.6 + flutterNoise * flutterNoiseScale * 0.4);

        wowPhase     += 2.0 * kPi * 0.8 / sr;
        flutterPhase += 2.0 * kPi * 9.0 / sr;
        if (wowPhase     > 2.0 * kPi) wowPhase     -= 2.0 * kPi;
        if (flutterPhase > 2.0 * kPi) flutterPhase -= 2.0 * kPi;

        const double wf = (double) bp.wowDepth * 0.006 * wowMod + (double) bp.flutDepth * 0.003 * fltMod;
        double speed = curSpeed * (1.0 + wf);

        // ---- record stage: DC block, then auto gain, then write to tape
        float yv[2] = { 0.0f, 0.0f };
        double inPow = 0.0, outPow = 0.0;

        for (int c = 0; c < nch; ++c)
        {
            const float in = data[c][s];
            dryRing[(size_t) c][(size_t) (writeIndex & dryMask)] = in;

            auto& cs = chan[(size_t) c];
            const float x = satPtrs[c][s];
            const float y = x - cs.dcX + 0.995f * cs.dcY;
            cs.dcX = x;
            cs.dcY = y;
            yv[c] = y;

            inPow  += (double) in * (double) in;
            outPow += (double) y  * (double) y;
        }

        inEnv  += envCoeff * (inPow  / (double) nch - inEnv);
        outEnv += envCoeff * (outPow / (double) nch - outEnv);

        double agTarget = 1.0;
        if (bp.autoGain && inEnv > 1.0e-8 && outEnv > 1.0e-10)
            agTarget = juce::jlimit (0.1, 10.0, std::sqrt (inEnv / outEnv));

        autoGainSm += agCoeff * (agTarget - autoGainSm);

        for (int c = 0; c < nch; ++c)
            ring[(size_t) c][(size_t) (writeIndex & ringMask)] = yv[c] * (float) autoGainSm;

        // ---- head management (jump when the read head reaches the window edge)
        auto jumpTo = [this] (double newDelay)
        {
            const int other = 1 - activeHead;
            headPos[other] = (double) writeIndex - newDelay;
            fading = true;
            fadeCount = 0;
        };

        if (! fading)
        {
            const double delay = (double) writeIndex - headPos[activeHead];

            if (curSpeed < 1.0 && delay > bp.windowSamples)
            {
                jumpTo ((double) minDelaySamples);
            }
            else if (curSpeed > 1.0 && delay < (double) minDelaySamples)
            {
                jumpTo (bp.windowSamples);
            }
            else if (std::abs (curSpeed - 1.0) < 0.002)
            {
                // Speed is back at 1x: bring the wet path back in time with the dry path.
                const double err = delay - bp.lockTarget;

                if (std::abs (err) > 0.05 * sr)
                    jumpTo (bp.lockTarget);
                else
                    speed += juce::jlimit (-0.002, 0.002, err / sr); // gentle slew
            }
        }

        // ---- read head(s)
        float w0 = 1.0f, w1 = 0.0f;
        if (fading)
        {
            const float t = (float) fadeCount / (float) xfadeSamples;
            w0 = std::cos (t * 1.5707963f);
            w1 = std::sin (t * 1.5707963f);
        }

        const int other = 1 - activeHead;

        // Level and tone follow transport speed (slower tape = quieter and darker)
        const double spd = juce::jlimit (0.05, 2.0, curSpeed);
        const float level = (float) juce::jlimit (0.0, 1.0, curSpeed * 4.0);
        const double fcLP = juce::jlimit (20.0, 0.45 * sr, (double) bp.toneHz * spd);
        const float aLP   = (float) (1.0 - std::exp (-2.0 * kPi * fcLP / sr));
        const double fcB  = 90.0 * spd;
        const float aB    = (float) (1.0 - std::exp (-2.0 * kPi * fcB / sr));

        for (int c = 0; c < nch; ++c)
        {
            const float* tape = ring[(size_t) c].data();
            float x = readTape (tape, headPos[activeHead], ringMask);
            if (fading)
                x = x * w0 + readTape (tape, headPos[other], ringMask) * w1;

            x *= level;

            auto& cs = chan[(size_t) c];

            // head bump (gentle low boost) and HF loss
            cs.bump += aB * (x - cs.bump);
            x += 0.25f * cs.bump;

            cs.lp1 += aLP * (x - cs.lp1);
            cs.lp2 += aLP * (cs.lp1 - cs.lp2);
            x = cs.lp2;

            // tape hiss (high-passed noise)
            if (bp.hissGain > 0.0f)
            {
                const float n = rng.nextFloat() * 2.0f - 1.0f;
                x += (n - cs.hissPrev) * 0.5f * bp.hissGain;
                cs.hissPrev = n;
            }

            const float dry = dryRing[(size_t) c][(size_t) ((writeIndex - totalLatency) & dryMask)];
            data[c][s] = (dry * (1.0f - mixv) + x * mixv) * outGain;
        }

        // ---- advance
        headPos[0] += speed;
        headPos[1] += speed;

        if (fading && ++fadeCount >= xfadeSamples)
        {
            activeHead = other;
            fading = false;
        }

        ++writeIndex;
    }
}

//==============================================================================
void TapeProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void TapeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TapeProcessor();
}
