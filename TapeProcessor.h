#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include <cstdint>

// Selotape (Songcraft Audio): tape saturation + varispeed.
//
// Signal flow:
//   input -> [oversampled] drive/saturation -> DC block -> auto gain
//         -> tape ring buffer
//         -> varispeed read head (wow/flutter modulated, two heads crossfaded)
//         -> speed-dependent head bump + HF loss -> hiss -> mix with dry
//
// Oversampling wraps only the nonlinear stage (the saturator), which is where
// aliasing comes from. Its filter latency is hidden: the plugin always reports
// one fixed latency (head delay + the 8x oversampler's latency) and pads the
// shorter ones, so switching the factor never changes host latency.
//
// Auto gain compares RMS going into the saturator with RMS coming out and
// applies the (slow, smoothed) ratio before the tape, so Drive changes tone
// and compression without changing perceived level.
//
// Varispeed: the read head moves at `speed` samples per input sample. Below 1x
// it falls behind the write head (pitch down), above 1x it catches up (pitch
// up). At the edge of the "Window" it jumps to the other end through a short
// equal-power crossfade.
class TapeProcessor : public juce::AudioProcessor
{
public:
    TapeProcessor();
    ~TapeProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Selotape"; }
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

    juce::AudioProcessorValueTreeState apvts;

    // Current transport speed ratio (1.0 = normal), read by the editor for display.
    std::atomic<float> liveSpeed { 1.0f };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    struct BlockParams
    {
        double targetSpeed = 1.0;
        double inertiaCoeff = 0.0;
        double windowSamples = 0.0;
        double lockTarget = 0.0;
        float bias = 0.0f, tanhBias = 0.0f, biasComp = 1.0f;
        float toneHz = 12000.0f;
        float wowDepth = 0.0f, flutDepth = 0.0f;
        float hissGain = 0.0f;
        bool autoGain = true;
        int order = 0; // oversampling stages: 0 = off, 1 = 2x, 2 = 4x, 3 = 8x
    };

    void processChunk (juce::AudioBuffer<float>& buffer, int start, int num, const BlockParams& bp);

    struct ChannelState
    {
        float dcX = 0.0f, dcY = 0.0f;      // DC blocker
        float lp1 = 0.0f, lp2 = 0.0f;      // HF loss (2 x one-pole)
        float bump = 0.0f;                 // head bump low band
        float hissPrev = 0.0f;
    };

    std::array<std::vector<float>, 2> ring;    // tape
    std::array<std::vector<float>, 2> dryRing; // latency-matched dry
    std::array<ChannelState, 2> chan;

    // Oversampling (index 0 = 2x, 1 = 4x, 2 = 8x)
    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 3> oversamplers;
    int osLatency[4] { 0, 0, 0, 0 };   // base-rate samples, indexed by order
    int osMaxLatency = 0;
    int lastOrder = -1;
    int maxBlock = 512;
    juce::AudioBuffer<float> satBuf;
    std::vector<float> driveBuf;

    // Auto gain
    double inEnv = 0.0, outEnv = 0.0;
    double envCoeff = 0.0, agCoeff = 0.0;
    double autoGainSm = 1.0;

    int64_t ringMask = 0, dryMask = 0;
    int64_t writeIndex = 0;

    double headPos[2] { 0.0, 0.0 };
    int activeHead = 0;
    bool fading = false;
    int fadeCount = 0;

    double sr = 44100.0;
    int xfadeSamples = 0;
    int minDelaySamples = 0;
    int latencySamples = 0;  // head delay at 1x, excluding oversampler latency
    int totalLatency = 0;    // reported to host; also the dry delay

    double curSpeed = 1.0;

    double wowPhase = 0.0, flutterPhase = 0.0;
    double wowNoise = 0.0, flutterNoise = 0.0;
    double wowNoiseA = 0.0, flutterNoiseA = 0.0;
    double wowNoiseScale = 1.0, flutterNoiseScale = 1.0;

    juce::Random rng;

    juce::SmoothedValue<float> driveSm, mixSm, outSm;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TapeProcessor)
};
