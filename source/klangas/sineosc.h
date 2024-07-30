#pragma once

// #include <juce_audio_processors/juce_audio_processors.h>
// #include <juce_dsp/juce_dsp.h>
#include "Tunings.h"
#include <pmmintrin.h>
#include "sst/basic-blocks/modulators/SimpleLFO.h"
#include "sst/basic-blocks/modulators/ADSREnvelope.h"
#include <utility>
#include <optional>
#include "../xap_utils.h"
#include "audio/choc_SampleBuffers.h"
#include <mutex>
#include "sst/basic-blocks/dsp/FollowSlewAndSmooth.h"

namespace juce
{
template <typename T> inline T jlimit(T minval, T maxval, T val)
{
    return std::clamp(val, minval, maxval);
}
} // namespace juce

struct SRProvider
{
    static constexpr int BLOCK_SIZE = 32;
    static constexpr int BLOCK_SIZE_OS = BLOCK_SIZE * 2;
    SRProvider() { initTables(); }
    alignas(32) float table_envrate_linear[512];

    void initTables()
    {
        double dsamplerate_os = samplerate * 2;
        for (int i = 0; i < 512; ++i)
        {
            double k =
                dsamplerate_os * pow(2.0, (((double)i - 256.0) / 16.0)) / (double)BLOCK_SIZE_OS;
            table_envrate_linear[i] = (float)(1.f / k);
        }
    }
    float envelope_rate_linear_nowrap(float x)
    {
        x *= 16.f;
        x += 256.f;
        int e = std::clamp<int>((int)x, 0, 0x1ff - 1);

        float a = x - (float)e;

        return (1 - a) * table_envrate_linear[e & 0x1ff] +
               a * table_envrate_linear[(e + 1) & 0x1ff];
    }
    void setSampleRate(double sr)
    {
        samplerate = sr;
        initTables();
    }
    double samplerate = 44100.0;

  private:
};

class AdditiveSharedData
{
  public:
    enum MODSOURCES
    {
        MOS_LFO0,
        MOS_LFO1,
        MOS_LFO2,
        MOS_LFO3,
        MOS_EG0,
        MOS_EG1,
        MOS_BURST,
        MOS_POLYAT,
        MOS_CC_A,
        MOS_CC_B,
        MOS_CC_C,
        MOS_CC_D,
        MOS_LAST
    };
    enum MODTARGETS
    {
        MOT_PARTVOLS_MORPH,
        MOT_PARTPANS_MORPH,
        MOT_PITCH,
        MOT_VOLUME,
        MOT_PARTREMAPMORPH,
        MOT_FILTERMORPH,
        MOT_AUX_SEND_A,
        MOT_LAST
    };
    alignas(16) float modmatrix[AdditiveSharedData::MOS_LAST][AdditiveSharedData::MOT_LAST];
    AdditiveSharedData();
    int m_quantize_pitch_mod_mode = 1;
    alignas(32) static const int maxampframes = 16;
    alignas(32) static const int numamppresets = 9;
    alignas(32) static const int num_panpresets = 4;
    using MorphTableType = std::array<std::array<float, 64>, maxampframes + 1>;
    alignas(32) MorphTableType partialsmorphtable;
    alignas(32) std::array<MorphTableType, numamppresets> volumepresets;
    alignas(32) std::array<MorphTableType, num_panpresets> pan_morph_presets;
    static constexpr int maxpanframes = 16;
    alignas(32) std::array<std::array<float, 64>, maxpanframes + 1> partialspanmorphtable;
    Tunings::Tuning fundamental_tuning;
    Tunings::KeyboardMapping kbm;
    void initKeyMapEDO(double referenceFrequency, double pseudoOctave, int edo);
    void initKeyMapFromScala(double referenceFrequency, double pseudoOctave, std::string fn);
    void updateExtraMorphFrame();
    void generateAmplitudeMorphTablePresets();
    void generatePanMorphTablePresets();
    void setVolumeMorphPreset(int index);
    void setPanMorphPreset(int index);

    alignas(32) std::array<float, 64> voice_random_filter_gains;
    alignas(32) std::array<std::array<float, 512>, 2> pan_coefficients;
    //
    inline float remapKeyInMidiOnlyMode(float res)
    {
        // if (!isStandardTuning && tuningApplicationMode == RETUNE_MIDI_ONLY)
        {
            auto idx = (int)floor(res);
            float frac = res - idx; // frac is 0 means use idx; frac is 1 means use idx + 1
            float b0 = fundamental_tuning.logScaledFrequencyForMidiNote(idx) * 12;
            float b1 = fundamental_tuning.logScaledFrequencyForMidiNote(idx + 1) * 12;
            res = (1.f - frac) * b0 + frac * b1;
        }
        return res;
    }
    bool m_using_custom_kbm = false;
    int m_cur_pan_morph_preset = -1;

    int m_cur_vol_morph_preset = -1;
    SRProvider sst_provider;
    const MorphTableType &getUserMorphTableForRead() const { return partialsmorphtable_custom; }
    // always marks user morph table dirty, so best to actually make some changes into it
    MorphTableType &getUserMorphTableForWrite()
    {
        m_custom_morph_table_dirty = true;
        return partialsmorphtable_custom;
    }
    // static AdditiveSharedData::MorphTableType readFromFile(juce::File f);
    // static juce::String getTableAsText(const AdditiveSharedData::MorphTableType &tab,
    //                                   bool format_for_code);
    static constexpr float minpartialfrequency = Tunings::MIDI_0_FREQ;
    static constexpr float maxpartialfrequency = 20000.0f;
    static float frequencyAsOctave(float Hz)
    {
        // should probably use a look up table for this...
        return std::log2(Hz / minpartialfrequency);
    }
    static float octaveAsFrequency(float octave)
    {
        return minpartialfrequency * std::pow(2.0f, octave);
    }
    float low_cutoff = 64.0f;
    float high_cutoff = 10000.0;
    float getSafetyFilterCoefficient(float hz);
    alignas(32) std::array<float, 2048> safety_filter;

  private:
    alignas(32) MorphTableType partialsmorphtable_custom;
    bool m_custom_morph_table_dirty = true;
};

using VoiceEG = sst::basic_blocks::modulators::ADSREnvelope<SRProvider, SRProvider::BLOCK_SIZE>;
using SimpleLFO = sst::basic_blocks::modulators::SimpleLFO<SRProvider, SRProvider::BLOCK_SIZE>;

class BurstGenerator
{
  public:
    BurstGenerator(SRProvider *ss) : env(ss) { setParameters(1.0, 4, 1.0, 0.5); }
    void setSampleRate(double sr) { m_sr = sr; }
    void setParameters(double dur, int numbursts, double prob, double distrib)
    {
        m_burst_duration = dur;
        m_num_bursts = numbursts;
        m_probability = prob;
        m_distrib = distrib;
    }
    void begin()
    {
        m_phase = 0.0;
        m_counter = 0;
        cur_out = 0.0f;
    }
    float getBurstTime(int index);
    float process();
    float cur_out = 0.0f;
    bool m_auto_repeat = true;
    int m_num_bursts = 8;
    double m_burst_duration = 1.0;

  private:
    double m_phase = 0.0;
    double m_sr = 44100;
    int m_counter = 0;
    double m_probability = 0.5;
    double m_distrib = 0.5;
    std::minstd_rand m_rng;
    std::uniform_real_distribution<float> m_dist{0.0f, 1.0f};
    VoiceEG env;
};

class SignalSmoother
{
  public:
    SignalSmoother() {}
    inline double process(double in)
    {
        double result = in + m_slope * (m_history - in);
        m_history = result;
        return result;
    }
    void setSlope(double x) { m_slope = x; }
    double getSlope() const { return m_slope; }

  private:
    float m_history = 0.0f;
    float m_slope = 0.999f;
};

class AdditiveVoice
{
  public:
    alignas(16) int m_num_partials = 1;
    AdditiveSharedData *m_shared_data = nullptr;
    AdditiveVoice();
    AdditiveVoice(AdditiveSharedData *sd) : AdditiveVoice() { setSharedData(sd); }
    void setSharedData(AdditiveSharedData *d);

    void setSampleRate(float hz);

    void setFundamental(float hz) { m_fundamental_freq = juce::jlimit(20.0f, 6000.0f, hz); }
    void setNumPartials(int n);

    float m_partial_gain_compen = 1.0f;
    void setPartialsBalance(float b) { m_partials_bal = b; }
    float m_filter_morph = 0.0f;
    float m_filter_morph_mod = 0.0f;
    int m_filter_mode = 0;

    float m_cc_vals[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    SignalSmoother m_cc_smoothers[4];
    void setCC(int ccnum, float ccval)
    {
        if (ccnum >= 41 && ccnum < 45)
            m_cc_vals[ccnum - 41] = ccval;
    }

    float m_pitch_lfo_mod = 0.0f;

    float m_volume_lfo_mod = 0.0f; // dB
    std::atomic<bool> m_frequencies_ready_to_show{false};
    int m_update_throttle = 0;
    void setPseudoOctave(float po) { m_pseudo_octave = po; }
    int getNumPartials() { return m_num_partials; }
    float getPartialFrequency(int index, bool is_for_gui)
    {
        if (index >= 0 && index < m_num_partials)
        {
            if (!is_for_gui)
                return m_partial_freqs[index];
            return m_partial_freqs_vis[index];
        }
        return 1.0f;
    }
    float getPartialAmplitude(int index)
    {
        if (index >= 0 && index < m_num_partials)
        {
            return m_partial_amplitudes[index];
        }
        return 0.0f;
    }
    void setTuningMode(int m) { m_tuning_mode = m; }
    void setEDO(int edo) { m_edo = edo; }

    std::array<float, 4> m_lfo_rates;
    std::array<int, 4> m_lfo_types;
    std::array<float, 4> m_lfo_deforms;
    void updateState();
    void postProcessUpdate(int nframes);
    struct EGParams
    {
        float a = 0.1f;
        float d = 0.1f;
        float s = 1.0f;
        float r = 0.1;
    };
    EGParams m_eg0_params;
    EGParams m_eg1_params;
    void setADSRParameters(int which, float a, float d, float s, float r)
    {
        EGParams *pars = &m_eg0_params;
        if (which == 1)
            pars = &m_eg1_params;
        pars->a = a;
        pars->d = d;
        pars->s = s;
        pars->r = r;
        m_adsr_sustain_level = s;
    }
    float m_vel_respo = -48.0f;
    float m_base_volume = -6.0f;
    float m_adsr_sustain_level = 1.0f;
    bool m_eg_gate = false;
    void beginNote(int port_index, int channel, int key, int noteid, double velo);

    void endNote() { m_eg_gate = false; }
    void setPan(float p) { m_pan = p; }
    void setPartialsPanMorph(float m) { m_partials_pan_morph = m; }
    void setKeyShift(int s) { m_key_shift = s; }
    int m_cur_midi_note = -1;
    double m_cur_velo_gain = 0.0;
    void step(); // produce next output sample frame
    alignas(16) float output_frame[4];
    alignas(16) float block_output[4][256];
    float m_aux_send_a = 0.0f;
    static const int maxnumpartials = 64;
    std::optional<VoiceEG> m_eg0;
    std::optional<VoiceEG> m_eg1;
    float m_adsr_vis_amp = 0.0f;
    float m_amp_morph_vis = 0.0f;
    bool m_is_available = true;
    int m_start_time_stamp = 0;
    // in "keys", so straightforward for EDOs with standard keyboard
    // mapping, but more involved to deal with non-EDO and/or non-standard KBM scenarios...
    float m_pitch_bend_amount = 0.0f;
    float m_pitch_adjust_amount = 0.0f;
    float m_after_touch_amount = 0.0f; // unipolar 0-1

    std::optional<BurstGenerator> m_burst_gen;
    float m_adsr_burst_mix = 0.5f;
    float m_freq_tweaks_mix = 0.0f;
    float m_freq_tweaks_mix_mod = 0.0f;
    int m_freq_tweaks_mode = 0;
    float m_cur_lowest_freq = 0.0;  // should maybe be atomic...
    float m_cur_highest_freq = 0.0; // likewise
    alignas(32) bool modulator_unipolar[AdditiveSharedData::MOS_LAST];
    alignas(32) std::array<float, maxnumpartials + 4> m_output_samples;
    alignas(32) std::array<float, maxnumpartials> m_partial_pans;

  private:
    alignas(32) std::array<float, maxnumpartials> m_partial_freqs;
    alignas(32) std::array<float, maxnumpartials> m_partial_freqs_vis;
    alignas(32) std::array<std::array<float, maxnumpartials>, 6> m_partial_freq_tweak_ratios;

    alignas(32) std::array<float, maxnumpartials> m_partial_phases;
    alignas(32) std::array<float, maxnumpartials> m_partial_phaseincs;
    alignas(32) std::array<float, maxnumpartials> m_partial_amplitudes;
    alignas(32) std::array<float, maxnumpartials> m_partial_safetyfiltergains;
    alignas(32) std::array<float, maxnumpartials> m_partial_shapingfiltergains;
    alignas(32) std::array<float, maxnumpartials> m_partial_vol_smoothing_history;
    alignas(32) std::array<float, maxnumpartials> m_partial_pan_smoothing_history;
    float m_gain_smoothing_coeff = 0.999f;
    float m_pan_smoothing_coeff = 0.999;
    float getShapingFilterGain(float hz);
    float m_fundamental_freq = 1.0;

    float m_sr = 44100.0;
    float m_partials_bal = 0.5f;
    float m_pseudo_octave = 1200.0f;
    int m_tuning_mode = 0;
    int m_edo = 12;

    alignas(32) sst::basic_blocks::dsp::SlewLimiter m_pitch_bend_smoother;

    int m_key_shift = 0;
    float m_pan = 0.5f; // center
    float m_partials_pan_morph = 0.5;

    std::optional<SimpleLFO> surge_lfo[4];

    int surge_lfo_update_counter = 0;
    int voice_step_update_counter = 0;
};

class AdditiveSynth
{
  public:
    AdditiveSynth();
    void prepare(double sampleRate, int maxbufsize);
    void processBlock(choc::buffer::ChannelArrayView<float> destBuf);
    AdditiveSharedData m_shared_data;

    alignas(32)
        std::array<AdditiveVoice, 8> m_voices; // {constructFill<AdditiveVoice,8>(&m_shared_data)};

    // juce::String importScalaFile(juce::File file);
    // juce::String importKBMFile(juce::File file);
    // juce::String importKBMText(juce::String text);
    void setEDOParameters(double pseudoOctaveCents, int edo);

    double m_tuning_update_elapsed_ms = 0.0;
    // in key space
    void setPitchBendRange(double range);
    std::atomic<bool> m_has_error{false};
    std::string m_error_text;
    double getPseudoOctave() { return m_pseudo_octave; }
    int getEDO() { return m_edo; }
    void setModulationDepth(int source, int target, float amount)
    {
        m_shared_data.modmatrix[source][target] = amount;
    }
    std::atomic<int> m_num_active_voices{0};
    void handleNoteOn(int port_index, int channel, int key, int noteid, double velo);
    void handleNoteOff(int port_index, int channel, int key, int noteid);
    void handleCC(int port_index, int channel, int cc, int value);
    void handlePitchBend(int port_index, int channel, float value);
    void handlePolyAfterTouch(int port_index, int channel, int note, float value);

  private:
    choc::buffer::ChannelArrayBuffer<float> m_mixbuf;
    int m_note_counter = 0;
    int m_time_pos_counter = 0;
    int m_kbm_start_note = -1;
    int m_kbm_ref_note = -1;
    double m_kbm_freq = -1;
    double m_pseudo_octave = 1200.0;
    int m_edo = 0;
    bool m_sustain_pedal = false;
    double m_pitch_bend_range = 1.0;
    double m_cur_pitch_bend = 0.0;
    std::mutex m_cs;
};
