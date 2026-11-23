#pragma once

#include "TapeProcessor.h"
#include <memory>
#include <vector>

namespace selo
{
inline const juce::Colour kBg      { 0xff101216 };
inline const juce::Colour kPanel   { 0xff161a1f };
inline const juce::Colour kPanelHi { 0xff1e232a };
inline const juce::Colour kLine    { 0xff2b313a };
inline const juce::Colour kTrack   { 0xff2a2f38 };
inline const juce::Colour kText    { 0xffe9eaec };
inline const juce::Colour kMuted   { 0xff7e848f };
inline const juce::Colour kAccent  { 0xffffb454 };
} // namespace selo

// Flat, dark look: thin arc dials, a pill toggle for Tape Stop, a small switch
// for Auto Gain. All drawing is vector, no image assets.
class SelotapeLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SelotapeLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider&) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawButtonAsHighlighted,
                           bool shouldDrawButtonAsDown) override;

    juce::Label* createSliderTextBox (juce::Slider&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
};

class SelotapeEditor : public juce::AudioProcessorEditor,
                       private juce::Timer
{
public:
    explicit SelotapeEditor (TapeProcessor&);
    ~SelotapeEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };

    Knob* addKnob (const juce::String& paramId, const juce::String& text, bool big, bool bipolar);

    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttachment    = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    TapeProcessor& proc;
    SelotapeLookAndFeel lnf;

    std::vector<std::unique_ptr<Knob>> knobs;
    Knob* speedKnob = nullptr;

    juce::ToggleButton stopButton, autoGainButton;
    juce::ComboBox osBox;

    std::unique_ptr<ButtonAttachment> stopAtt, autoGainAtt;
    std::unique_ptr<ComboAttachment> osAtt;

    juce::Rectangle<int> leftPanel, rightPanel, osText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SelotapeEditor)
};
