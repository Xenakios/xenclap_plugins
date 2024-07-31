#include "sineosc.h"
// #define _MSC_VERX 666
// #define __AVX2__ 1
#if KLANGAS_AVX
#include "avx_mathfun.h"
#endif
#include <sse_mathfun.h>
// #include "BinaryData.h"
#ifdef HAVEJUCE
AdditiveSharedData::MorphTableType AdditiveSharedData::readFromFile(juce::File f)
{
    AdditiveSharedData::MorphTableType result;
    for (int i = 0; i < result.size(); ++i)
        for (int j = 0; j < result[i].size(); ++j)
            result[i][j] = 0.0f;
    if (!f.exists())
        return result;
    juce::StringArray sarr;
    f.readLines(sarr);
    if (sarr.size() == 0)
        return result;
    int rows = sarr.size();
    if (rows > 16)
        rows = 16;
    for (int i = 0; i < rows; ++i)
    {
        auto line = sarr[i];
        auto tokens = juce::StringArray::fromTokens(line, ",", "");
        int numtoparse = tokens.size();
        if (numtoparse > 64)
            numtoparse = 64;
        for (int j = 0; j < numtoparse; ++j)
        {
            float gain = tokens[j].getFloatValue();
            gain = xenakios::jlimit(0.0f, 1.0f, gain);
            result[i][j] = gain;
        }
    }
    return result;
}
juce::String AdditiveSharedData::getTableAsText(const AdditiveSharedData::MorphTableType &tab,
                                                bool format_for_code)
{
    juce::String out;
    if (tab.empty())
        return out;
    int parts = tab.front().size();
    int frames = tab.size() - 1; // remember, we have the guard frame!
    if (format_for_code)
    {
        out << "static const char* partial_morph_table[16] = {\n";
        for (int i = 0; i < frames; ++i)
        {
            out << "{\"";
            for (int j = 0; j < parts; ++j)
            {
                float gain = tab[i][j];
                gain = xenakios::mapvalue(gain, 0.0f, 1.0f, 0.0f, 25.0f);
                char ch = 'A' + std::floor(gain);
                out << ch;
            }
            out << "\"}";
            out << "\n";
        }
        out << "\"}\n";
    }
    else
    {
        for (int i = 0; i < frames; ++i)
        {
            for (int j = 0; j < parts; ++j)
            {
                out << tab[i][j];
                if (j < parts - 1)
                    out << ",";
            }
            out << "\n";
        }
    }

    return out;
}
#endif

void AdditiveSharedData::setPanMorphPreset(int index)
{
    if (index < 0 || index >= num_panpresets)
        return;
    partialspanmorphtable = pan_morph_presets[index];
    partialspanmorphtable[maxpanframes] = partialspanmorphtable[maxpanframes - 1];
}

void AdditiveSharedData::setVolumeMorphPreset(int index)
{
    if (index < 0 || index > numamppresets)
        return;
    if (index == numamppresets)
    {
        partialsmorphtable = partialsmorphtable_custom;
        updateExtraMorphFrame();
        return;
    }
    partialsmorphtable = volumepresets[index];
    updateExtraMorphFrame();
}

float AdditiveSharedData::getSafetyFilterCoefficient(float hz)
{
    assert(!std::isnan(hz));
    if (hz <= minpartialfrequency || hz >= maxpartialfrequency)
        return 0.0f;
    int index = (int)xenakios::mapvalue<float>(hz, minpartialfrequency, maxpartialfrequency, 0.0f,
                                               safety_filter.size() - 1);
    assert(index >= 0 && index < safety_filter.size());
    // index = xenakios::jlimit<int>(0,safety_filter.size()-1,index);
    return safety_filter[index];
}

AdditiveSharedData::AdditiveSharedData()
{
    for (int i = 0; i < AdditiveSharedData::MOS_LAST; ++i)
        for (int j = 0; j < AdditiveSharedData::MOT_LAST; ++j)
            modmatrix[i][j] = 0.0f;
    low_cutoff = 20.0f;
    high_cutoff = 15000.0f;
    for (int i = 0; i < safety_filter.size(); ++i)
    {
        float hz = xenakios::mapvalue<float>(i, 0, safety_filter.size() - 1, minpartialfrequency,
                                             maxpartialfrequency);
        float gain = 1.0f;
        if (hz <= low_cutoff)
            gain = xenakios::mapvalue(hz, minpartialfrequency, low_cutoff, 0.0f, 1.0f);
        else if (hz >= high_cutoff)
            gain = xenakios::mapvalue(hz, high_cutoff, maxpartialfrequency, 1.0f, 0.0f);
        assert(gain >= 0.0f && gain <= 1.0f);
        safety_filter[i] = gain;
    }

    std::minstd_rand0 rng(99217);
    std::uniform_real_distribution<float> dist{0.0f, 1.0f};
    for (int i = 0; i < voice_random_filter_gains.size(); ++i)
    {
        float db = -30.0 + 30.0 * dist(rng);
        voice_random_filter_gains[i] = xenakios::decibelsToGain(db);
    }
    int pan_coeffs_size = pan_coefficients[0].size();
    for (int i = 0; i < pan_coeffs_size; ++i)
    {
        float normalisedPan = xenakios::mapvalue<float>(i, 0, pan_coeffs_size - 1, 0.0f, 1.0f);
        float leftValue =
            static_cast<float>(std::pow(std::sin(0.5 * M_PI * (1.0 - normalisedPan)), 2.0));
        float rightValue = static_cast<float>(std::pow(std::sin(0.5 * M_PI * normalisedPan), 2.0));
        float boostValue = static_cast<float>(2.0);
        pan_coefficients[0][i] = leftValue * boostValue;
        pan_coefficients[1][i] = rightValue * boostValue;
    }
    for (int i = 0; i < maxampframes; ++i)
    {
        for (int j = 0; j < 64; ++j)
        {
            partialsmorphtable[i][j] = 0.0f;
        }
    }
    generateAmplitudeMorphTablePresets();
    generatePanMorphTablePresets();
    // initKeyMapEDO(440.0,1200.0,12);
    try
    {
        // kbm = Tunings::readKBMFile("C:\\develop\\AdditiveSynth\\KBM\\31edo_hex.kbm");
        m_using_custom_kbm = false;
        // tuning = Tunings::Tuning(Tunings::evenTemperament12NoteScale(),kbm);
        fundamental_tuning = Tunings::Tuning(Tunings::evenDivisionOfCentsByM(1200.0, 31), kbm);

        /*
        juce::String txt;
        for (int i=0;i<Tunings::Tuning::N;++i)
        {
            txt+=juce::String(tuning.frequencyForMidiNoteScaledByMidi0(i),1)+" ";
        }
        DBG(txt);
        */
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
    }
    // initKeyMapFromScala(440.0,1200.0,"C:\\develop\\AdditiveSynth\\scala\\ED2-23_MOS-9_Ch-8_13o23_678.261.scl");
    //  init user custom volumes morph table to fundamentals at maximum
    for (int i = 0; i < maxampframes; ++i)
    {
        for (int j = 0; j < 64; ++j)
        {
            if (j > 0)
                partialsmorphtable_custom[i][j] = 0.0f;
            else
                partialsmorphtable_custom[i][j] = 1.0f;
        }
    }
    m_custom_morph_table_dirty = true;
}

void AdditiveSharedData::generatePanMorphTablePresets()
{
    std::minstd_rand0 rng{763};
    std::uniform_real_distribution<float> dist{0.0f, 1.0f};
    for (int i = 0; i < maxpanframes; ++i)
    {
        for (int j = 0; j < 64; ++j)
        {
            float panpos = 0.5f;

            panpos = 0.05 + dist(rng) * 0.9;
            if (j == 0 || i == 0)
                panpos = 0.5f;
            pan_morph_presets[0][i][j] = panpos;

            float phaseoffset = M_PI * 2 / maxpanframes * i;
            float rate = 2.0;
            panpos = 0.5 + 0.475 * std::sin(M_PI * 2 / 64 * rate * j + phaseoffset);
            pan_morph_presets[1][i][j] = panpos;

            phaseoffset = 0.0f;
            rate = std::pow(2.0, i * 0.25);
            panpos = 0.5 + 0.475 * std::sin(M_PI * 2 / 64 * rate * j + phaseoffset);
            pan_morph_presets[2][i][j] = panpos;

            float amp = xenakios::mapvalue<float>(i, 0, maxpanframes - 1, 0.0, 0.5);
            panpos = 0.5 + amp * std::sin(M_PI * 2 / 64 * 8 * j + 0);
            pan_morph_presets[3][i][j] = panpos;
        }
    }
}

void AdditiveSharedData::generateAmplitudeMorphTablePresets()
{
    std::default_random_engine rng(78901);
    std::uniform_real_distribution<float> uni(-48.0f, 0.0f);
    std::normal_distribution<float> gauss(-12.0f, 4.0f);
    std::exponential_distribution<float> expo(0.1f);
    std::cauchy_distribution<float> cauchy(-40.0f, 0.1f);

    for (int i = 0; i < 16; ++i)
    {
        int maxpart = xenakios::mapvalue<float>(i, 0, 15, 2, 63);
        for (int j = 0; j < 64; ++j)
        {
            if (j >= maxpart)
                volumepresets[0][i][j] = 0.0f;
            else
            {
                volumepresets[0][i][j] =
                    xenakios::mapvalue<float>(j, 0, maxpart, 1.0f, xenakios::decibelsToGain(-48.0));
            }
        }
    }
    // filtered "checkerboard"
    for (int i = 0; i < 16; ++i)
    {
        int offs = i % 2;
        float minattendb = xenakios::mapvalue<float>(i, 0, 15, -60.0, 0.0);
        float minatten = xenakios::decibelsToGain(minattendb);
        for (int j = 0; j < 64; ++j)
        {
            float basegain = 0.0f;
            if ((j % 2 == offs))
                basegain = 1.0f;
            if (j == 0)
                basegain = 1.0f;
            float attendb = xenakios::mapvalue<float>(j, 0, 63, 0.0, minattendb);
            float atten = xenakios::decibelsToGain(attendb);
            volumepresets[8][i][j] = basegain * atten;
        }
    }

    for (int i = 0; i < maxampframes; ++i)
    {
        for (int j = 0; j < 64; ++j)
        {
            float amp = -15.0 + 15.0 * std::cos(M_PI * 2 / 16 * i * 1.0f + (M_PI * 2 / 64 * j));
            float offs = xenakios::mapvalue<float>(j, 0, 63, 0.0, -9.0);
            amp += offs;
            amp = xenakios::decibelsToGain(amp);
            volumepresets[1][i][j] = amp;
        }
    }

    for (int i = 0; i < maxampframes; ++i)
    {
        for (int j = 0; j < 64; ++j)
        {
            float amp = -15.0 + 15.0 * std::cos(M_PI * 2 / 64 * j * 4.0f + i);
            amp = xenakios::decibelsToGain(amp);
            volumepresets[2][i][j] = amp;
        }
    }

    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 64; ++j)
            volumepresets[3][i][j] = 0.0f;
    for (int i = 0; i < 256; ++i)
    {
        float xcor = 32.0 + 32.0 * std::sin(M_PI * 2 / 256 * i * 2.63);
        xcor = std::clamp(xcor, 0.0f, 63.0f);
        float ycor = 7.5 + 8.0 * std::sin(M_PI * 2 / 256 * i * 3.5);
        ycor = std::clamp(ycor, 0.0f, 15.0f);
        float gain = xenakios::mapvalue<float>(xcor, 0, 63, 1.0, 0.25);
        volumepresets[3][(int)ycor][(int)xcor] = gain;
    }

    std::uniform_real_distribution<float> uni2(0.0f, 1.0f);
    for (int index = 0; index < 3; ++index)
    {
        for (int i = 0; i < maxampframes; ++i)
        {
            for (int j = 0; j < 64; ++j)
            {
                float dbval = 0.0;
                if (index == 0)
                    dbval = uni(rng);
                if (index == 1)
                    dbval = gauss(rng);
                if (index == 2)
                {
                    float prob = xenakios::mapvalue<float>(i, 0, maxampframes - 1, 0.1, 0.95);
                    if (uni2(rng) < prob)
                        dbval = 0.0f;
                    else
                        dbval = -40.f;
                }

                dbval = std::clamp(dbval, -60.0f, 0.0f);
                volumepresets[index + 4][i][j] = xenakios::decibelsToGain(dbval);
            }
        }
    }

    for (int i = 0; i < 16; ++i)
    {
        double slope = xenakios::mapvalue<double>(i, 0, 15, 0.01, 1.0);
        for (int j = 0; j < 64; ++j)
        {
            double gain = std::pow(j + 1, -slope);
            volumepresets[7][i][j] = gain;
        }
    }
}

void AdditiveSharedData::updateExtraMorphFrame()
{
    partialsmorphtable[maxampframes] = partialsmorphtable[maxampframes - 1];
}

void AdditiveSharedData::initKeyMapFromScala(double referenceFrequency, double pseudoOctave,
                                             std::string fn)
{
    double pseudoOctaveRatio = std::pow(2.0, (pseudoOctave / 1200.0));
    try
    {
        auto scale = Tunings::readSCLFile(fn);
        fundamental_tuning = Tunings::Tuning(scale);
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
    }
}

void AdditiveSharedData::initKeyMapEDO(double referenceFrequency, double pseudoOctave, int edo)
{
    /*
    double pseudoOctaveRatio = std::pow(2.0,(pseudoOctave / 1200.0));
    for (int i=0;i<128;++i)
    {
        double hz = referenceFrequency * std::pow (pseudoOctaveRatio, (i - 69) / (double)edo);
        miditohzmap[i]=hz;
    }
    */
}

float AdditiveVoice::getShapingFilterGain(float hz)
{
    float octave = AdditiveSharedData::frequencyAsOctave(hz);
    if (m_filter_mode == 0 || m_filter_mode == 1)
    {
        // the calculations might not be completely right, but it would be something like this
        float cutoff = 11.0f * m_filter_morph_mod;
        if (octave >= cutoff)
        {
            int order = (m_filter_mode + 1) * 2;
            float diff = octave - cutoff;
            float g = 1.0 / (std::pow(2.0f, diff * order));
            g = xenakios::jlimit(0.0f, 1.0f, g);
            return g;
        }
        return 1.0f;
    }
    else if (m_filter_mode == 2)
    {
        float offset = M_PI * 2 * m_filter_morph_mod;
        float g = 0.5f + 0.5f * std::sin(M_PI * 2 / 11 * octave * 8 + offset);
        g = xenakios::jlimit(0.0f, 1.0f, g);
        return g;
    }
    else if (m_filter_mode == 3)
    {
        float offset = m_filter_morph_mod;
        float g = std::fmod(octave + offset, 1.0f);
        g = xenakios::jlimit(0.0f, 1.0f, g);
        return g;
    }
    else if (m_filter_mode == 4)
    {
        float tablpos = octave * 2.0 + m_filter_morph_mod * 32;
        tablpos = std::fmod(tablpos, 62.0f);
        int i0 = tablpos;
        int i1 = i0 + 1;
        float frac = tablpos - i0;
        float v0 = m_shared_data->voice_random_filter_gains[i0];
        float v1 = m_shared_data->voice_random_filter_gains[i1];
        float g = v0 + (v1 - v0) * frac;
        g = xenakios::jlimit(0.0f, 1.0f, g);
        return g;
        float octave_min = 5.0 + m_filter_morph_mod * 2;
        float octave_max = 6.0 + m_filter_morph_mod * 2;
        float gain0 = 0.0f;
        if (octave >= octave_min && octave < octave_max)
            gain0 = 0.0f;
        else
            gain0 = 1.0f;
        float periods = 8.0; // 4.0+4.0*m_filter_morph_mod;
        float offset = M_PI * 2 * m_filter_morph_mod;
        float gain1 = 0.5f + 0.5f * std::sin(octave * periods + offset);
        return gain0 + (gain1 - gain0) * m_filter_morph_mod;
    }
    return 1.0f;
}

AdditiveVoice::AdditiveVoice()
{
    for (int i = 0; i < m_lfo_rates.size(); ++i)
    {
        m_lfo_rates[i] = 1.0;
        m_lfo_types[i] = 0;
        m_lfo_deforms[i] = 0.0;
    }
    // should calculate this with proper filter math based on sample rate and desired smoothing rate
    m_gain_smoothing_coeff = 0.990f;
    m_pan_smoothing_coeff = 0.999f; // smooth pans slower

    std::default_random_engine rng((int64_t)this);
    std::normal_distribution<float> norm(0.0, 12.0);
    float subharms[64];
    // 1   2   3   4   5 6 7 8 9
    // 1/5 1/4 1/3 1/2 1 2 3 4 5
    for (int i = 0; i < 64; ++i)
    {
        if (i < 6)
            subharms[i] = 1.0 / (6 - i);
        else
            subharms[i] = i - 4;
    }
    std::fill(m_output_samples.begin(), m_output_samples.end(), 0.0f);
    for (int i = 0; i < maxnumpartials; ++i)
    {
        m_partial_freqs[i] = 440.0;
        m_partial_freqs_vis[i] = 440.0f;
        m_partial_phases[i] = 0.0;
        m_partial_phaseincs[i] = 0.0;
        m_partial_amplitudes[i] = 0.0;

        m_partial_pans[i] = 0.5f;
        m_partial_safetyfiltergains[i] = 0.0f;
        m_partial_shapingfiltergains[i] = 0.0f;
        m_partial_vol_smoothing_history[i] = 0.0f;
        m_partial_pan_smoothing_history[i] = 0.0f;
        float orig = i + 1;
        float target = subharms[i];
        m_partial_freq_tweak_ratios[0][i] = target / orig;
        m_partial_freq_tweak_ratios[1][i] = std::pow(2.0, 1.0 / 12.0 * norm(rng));
        m_partial_freq_tweak_ratios[2][i] = 1.0 / (i + 1);
    }
    for (int i = 0; i < maxnumpartials; i += 2)
    {
        int partnum = i + 1;
        float orig0 = partnum;
        float target0 = partnum + 1;
        float orig1 = partnum + 1;
        float target1 = partnum;
        m_partial_freq_tweak_ratios[3][i + 0] = target0 / orig0;
        m_partial_freq_tweak_ratios[3][i + 1] = target1 / orig1;
    }
    for (int i = 0; i < maxnumpartials; ++i)
    {
        int partnum = i + 1;
        float orig = partnum;
        float target = 65 - partnum;
        m_partial_freq_tweak_ratios[4][i] = target / orig;
    }
    for (int i = 0; i < maxnumpartials; ++i)
    {
        int partnum = i + 1;
        if (partnum < 17)
        {
            // no changes for the lowerst 16 ones
            m_partial_freq_tweak_ratios[5][i] = 1.0f;
        }
        else
        {
            // let's extend...
            int extendnum = 17 + (partnum - 17) * 4;
            float orig = partnum;
            float target = extendnum;
            m_partial_freq_tweak_ratios[5][i] = target / orig;
        }
    }
    output_frame[0] = 0.0f;
    output_frame[1] = 0.0f;
    output_frame[2] = 0.0f;
    output_frame[3] = 0.0f;
    for (int i = 0; i < AdditiveSharedData::MOS_LAST; ++i)
    {
        modulator_unipolar[i] = false;
    }
    modulator_unipolar[AdditiveSharedData::MOS_POLYAT] = true;
    modulator_unipolar[AdditiveSharedData::MOS_CC_A] = true;
    modulator_unipolar[AdditiveSharedData::MOS_CC_B] = true;
    modulator_unipolar[AdditiveSharedData::MOS_CC_C] = true;
    modulator_unipolar[AdditiveSharedData::MOS_CC_D] = true;
}

void AdditiveVoice::postProcessUpdate(int nframes)
{
    // we only take 1 sample from the smoother in updateState, so we need to advance the smoother
    // m_pitch_bend_smoother.skip(nframes - 1);
}
#pragma float_control(precise, off, push)
void AdditiveVoice::updateState()
{
    // m_frequencies_ready_to_show = true;
    float pseudoOctaveRatio = std::pow(2.0, (m_pseudo_octave / 1200.0));
    // m_pitch_bend_smoother.setTargetValue(m_pitch_bend_amount);
    double pb = m_pitch_bend_amount;
    double pitch = m_cur_midi_note + pb + m_pitch_lfo_mod + m_pitch_adjust_amount;
    double mappedpitch = pitch;
    if (m_shared_data->m_quantize_pitch_mod_mode == 1)
        mappedpitch = m_shared_data->remapKeyInMidiOnlyMode(pitch);
    else if (m_shared_data->m_quantize_pitch_mod_mode == 2)
        mappedpitch = m_shared_data->remapKeyInMidiOnlyMode(std::round(pitch));
    else
        mappedpitch =
            m_shared_data->remapKeyInMidiOnlyMode(m_cur_midi_note + m_pitch_adjust_amount) + pb +
            m_pitch_lfo_mod;
    m_fundamental_freq = Tunings::MIDI_0_FREQ * std::pow(2.0, 1.0 / 12 * mappedpitch);
    if (m_tuning_mode == 0)
    {
        for (int i = 0; i < m_num_partials; ++i)
        {
            float hz = m_fundamental_freq * (std::pow(pseudoOctaveRatio, std::log2(i + 1)));
            m_partial_freqs[i] = hz;
        }
    }
    else
    {
#define OLD_QUANTIZED_PARTIALS 1
#ifdef OLD_QUANTIZED_PARTIALS
        int iter = 1;
        int numpartials = 1;
        m_partial_freqs[0] = m_fundamental_freq;
        m_partial_phaseincs[0] = M_PI * 2 / m_sr * m_fundamental_freq;
        // m_partial_amplitudes[0] = calculatePartialAmplitude(0);
        while (numpartials < m_num_partials)
        {
            // const frequency = fundamental * (pseudoOctaveRatio **
            // (Math.round(Math.log2(iteration) * edo) / edo))
            float hz = m_fundamental_freq *
                       (std::pow(pseudoOctaveRatio, std::round(std::log2(iter) * m_edo) / m_edo));
            if (hz != m_partial_freqs[numpartials - 1])
            {
                m_partial_freqs[numpartials] = hz;
                ++numpartials;
            }
            ++iter;
            if (iter == 1000)
                break;
            assert(iter < 1000);
        }
#else
        double last_freq = 0.0;
        int j = 0;
        int innercount = 0;
        m_partial_freqs[0] = m_fundamental_freq;
        m_partial_phaseincs[0] = mkd::twoPi / m_sr * m_fundamental_freq;
        // obviously this is a bit suspect in terms of algorithmic complexity,
        // but will have to work for now
        for (int i = 1; i < m_num_partials; ++i)
        {
            double partialfreq = m_fundamental_freq + m_fundamental_freq * i;
            while (true)
            {
                ++innercount;
                double quantizedfreq = m_shared_data->partial_tuning.frequencyForMidiNote(j);
                if (quantizedfreq != last_freq && quantizedfreq >= partialfreq)
                {
                    partialfreq = quantizedfreq;
                    last_freq = partialfreq;
                    m_partial_freqs[i] = partialfreq;
                    m_partial_phaseincs[i] = mkd::twoPi / m_sr * partialfreq;
                    // std::cout << partialfreq << " ";
                    break;
                }
                ++j;
                if (j > 256)
                    break;
            }
        }
#endif
    }
    float minf = std::numeric_limits<float>::max();
    float maxf = std::numeric_limits<float>::min();
    for (int i = 0; i < m_num_partials; ++i)
    {
        float targetratio = m_partial_freq_tweak_ratios[m_freq_tweaks_mode][i];
        float tweakratio =
            xenakios::mapvalue<float>(m_freq_tweaks_mix_mod, 0.0f, 1.0f, 1.0f, targetratio);
        float pf = m_partial_freqs[i] * tweakratio;
        pf = xenakios::jlimit(AdditiveSharedData::minpartialfrequency,
                          AdditiveSharedData::maxpartialfrequency, pf);
        assert(!std::isnan(pf));
        float phaseinc = M_PI * 2 / m_sr * pf;
        if (phaseinc >= M_PI * 2) // quick hack
            phaseinc = std::fmod(phaseinc, M_PI * 2);
        m_partial_freqs[i] = pf;
        m_partial_phaseincs[i] = phaseinc;
        m_partial_freqs_vis[i] = pf;
        float sfgain = m_shared_data->getSafetyFilterCoefficient(pf);
        assert(sfgain >= 0.0f && sfgain <= 1.0f);
        m_partial_safetyfiltergains[i] = sfgain;
        sfgain = getShapingFilterGain(pf);
        assert(sfgain >= 0.0f && sfgain <= 1.0f);
        m_partial_shapingfiltergains[i] = sfgain;
        minf = std::min(minf, pf);
        maxf = std::max(maxf, pf);

        assert(m_partial_phaseincs[i] >= 0.0 && m_partial_phaseincs[i] < M_PI * 2);
    }
    m_cur_lowest_freq = minf;
    m_cur_highest_freq = maxf;
    // m_frequencies_ready_to_show = true;
}
#pragma float_control(pop)

void AdditiveVoice::beginNote(int port_index, int channel, int key, int noteid, double velo)
{
    state_update_counter = 0;
    m_cur_midi_note = key;
    m_note_id = noteid;
    m_note_channel = channel;
    m_note_port = port_index;
    velo = xenakios::mapvalue<float>(velo, 0.0f, 1.0f, m_vel_respo, 0.0f);
    m_cur_velo_gain = xenakios::decibelsToGain(velo);
    m_eg_gate = true;
    m_burst_gen->begin();
    m_is_available = false;
    m_eg0->attackFrom(0.0f, 0.0f, 0, true);
    m_eg1->attackFrom(0.0f, 0.0f, 0, true);
    m_pitch_bend_smoother.reset();

    for (int i = 0; i < maxnumpartials; ++i)
    {
        m_partial_phases[i] = 0.0;
    }
}

void AdditiveVoice::setSampleRate(float hz) { m_sr = hz; }

void AdditiveVoice::setNumPartials(int n)
{
    m_num_partials = xenakios::jlimit(2, maxnumpartials, n);
    float maxatten = -20.0f;
    float attendb = xenakios::mapvalue<float>(m_num_partials, 2, 64, -6.0, maxatten);
    attendb = xenakios::jlimit(maxatten, -6.0f, attendb);
    m_partial_gain_compen = xenakios::decibelsToGain(attendb);
}

void AdditiveVoice::setSharedData(AdditiveSharedData *d)
{
    m_shared_data = d;
    for (int i = 0; i < 4; ++i)
        surge_lfo[i].emplace(&d->sst_provider);
    m_burst_gen.emplace(&d->sst_provider);
    m_eg0.emplace(&d->sst_provider);
    m_eg1.emplace(&d->sst_provider);
}

#pragma float_control(precise, off, push)

void AdditiveVoice::process(choc::buffer::ChannelArrayView<float> destBuf)
{
    static const int shapetranslate[7] = {0, 1, 3, 4, 5, 6, 7};
    const int voicestepgranul = 64;
    alignas(16) const int numpartslocal = m_num_partials;
    alignas(32) float partial_outputs[maxnumpartials + 4];
    alignas(16) float outputs[2] = {0.0f, 0.0f};
    alignas(32) float lfo_destinations[AdditiveSharedData::MOT_LAST];
    alignas(32) float modulator_outs[AdditiveSharedData::MOS_LAST];
    int nframes = destBuf.getNumFrames();
    for (int outbufpos = 0; outbufpos < nframes; ++outbufpos)
    {
        for (int i = 0; i < AdditiveSharedData::MOT_LAST; ++i)
            lfo_destinations[i] = 0.0f;
        if (state_update_counter == 0)
        {
            m_eg0->processBlock(m_eg0_params.a, m_eg0_params.d, m_eg0_params.s, m_eg0_params.r, 1,
                                1, 1, m_eg_gate);
            m_eg1->processBlock(m_eg1_params.a, m_eg1_params.d, m_eg1_params.s, m_eg1_params.r, 1,
                                1, 1, m_eg_gate);
            // The SST LFO for some reason has a separate type for downward ramp
            // but we don't need that since the modulation can be applied negatively.
            // So we map our choice parameter with 7 shapes to the 8 shapes of the SST LFO
            // by skipping the downward ramp shape

            for (int i = 0; i < 4; ++i)
            {
                int translated = shapetranslate[m_lfo_types[i]];
                surge_lfo[i]->process_block(m_lfo_rates[i], m_lfo_deforms[i], translated, false);
            }
        }

        for (int i = 0; i < 4; ++i)
        {
            modulator_outs[i] = surge_lfo[i]->outputBlock[state_update_counter];
            if (modulator_unipolar[i])
                modulator_outs[i] = 1.0f + modulator_outs[i];
        }

        float envgain = m_eg0->outputCache[state_update_counter];
        modulator_outs[AdditiveSharedData::MOS_EG0] = envgain - m_adsr_sustain_level;
        float auxegval = m_eg1->outputCache[state_update_counter];
        modulator_outs[AdditiveSharedData::MOS_EG1] = auxegval;
        float burst_eg = 0.0; // m_burst_gen->process();
        modulator_outs[AdditiveSharedData::MOS_BURST] = burst_eg;
        modulator_outs[AdditiveSharedData::MOS_POLYAT] = m_after_touch_amount * 2.0f;
        for (int i = 0; i < 4; ++i)
            modulator_outs[AdditiveSharedData::MOS_CC_A + i] =
                m_cc_smoothers[i].process(m_cc_vals[i]) * 2.0f;
        alignas(32) float lfo_destinations[AdditiveSharedData::MOT_LAST] = {0.0f, 0.0f, 0.0f, 0.0f,
                                                                            0.0f, 0.0f, 0.0f};
        for (int i = 0; i < AdditiveSharedData::MOS_LAST; ++i) // sources
        {
            float modout = modulator_outs[i];
            assert((!std::isnan(modout)) && modout >= -100.0f && modout <= 100.0f);
            for (int j = 0; j < AdditiveSharedData::MOT_LAST; ++j) // destinations
            {
                lfo_destinations[j] += modout * m_shared_data->modmatrix[i][j];
            }
        }

        m_pitch_lfo_mod = lfo_destinations[AdditiveSharedData::MOT_PITCH] * 12.0;

        m_freq_tweaks_mix_mod =
            m_freq_tweaks_mix + lfo_destinations[AdditiveSharedData::MOT_PARTREMAPMORPH] * 0.5f;

        m_freq_tweaks_mix_mod = xenakios::jlimit(0.0f, 1.0f, m_freq_tweaks_mix_mod);
        // if (m_freq_tweaks_mode!=3)
        //     m_freq_tweaks_mix_mod = 1.0-std::pow(1.0-m_freq_tweaks_mix_mod,2.0f);

        m_filter_morph_mod =
            m_filter_morph + lfo_destinations[AdditiveSharedData::MOT_FILTERMORPH] * 0.5f;
        m_filter_morph_mod = xenakios::jlimit(0.0f, 1.0f, m_filter_morph_mod);

        m_volume_lfo_mod = 24.0 * lfo_destinations[AdditiveSharedData::MOT_VOLUME];
        m_volume_lfo_mod = std::clamp(m_volume_lfo_mod + m_base_volume, -96.0f, 0.0f);
        float volmodgain = xenakios::decibelsToGain(m_volume_lfo_mod);
        if (state_update_counter == 0)
        {
            // let's see how this goes...
            updateState();
        }

        // calculate SIMD sines with SSE
        float phaseslocal[64];
        for (int i = 0; i < 64; ++i)
            phaseslocal[i] = m_partial_phases[i];
        for (alignas(16) int i = 0; i < numpartslocal; i += 4)
        {

            alignas(32) __m128 temp = _mm_loadu_ps(&phaseslocal[i]);
            temp = sse_mathfun_sin_ps(temp);
            _mm_storeu_ps(&partial_outputs[i], temp);
        }

        float amp_morph =
            m_partials_bal + lfo_destinations[AdditiveSharedData::MOT_PARTVOLS_MORPH] * 0.5f;
        amp_morph = xenakios::jlimit<float>(0.0f, 1.0f, amp_morph);
        m_amp_morph_vis = amp_morph;
        amp_morph = amp_morph * (AdditiveSharedData::maxampframes - 1);
        int amp_morph_i0 = amp_morph;
        int amp_morph_i1 = amp_morph_i0 + 1;
        float amp_morph_frac = amp_morph - amp_morph_i0;

        float pan_morph =
            m_partials_pan_morph + lfo_destinations[AdditiveSharedData::MOT_PARTPANS_MORPH] * 0.5f;
        pan_morph = xenakios::jlimit<float>(0.0f, 1.0f, pan_morph);
        pan_morph = pan_morph * (AdditiveSharedData::maxpanframes - 1);
        int pan_morph_i0 = pan_morph;
        int pan_morph_i1 = pan_morph_i0 + 1;
        float pan_morph_frac = pan_morph - pan_morph_i0;

        // sum sines and advance phases
        // might be possible to do this as SIMD too, but won't bother for now
        float p0freq = m_partial_freqs[0];
        float minatten = xenakios::mapvalue(p0freq, 256.0f, 10000.0f, 1.0f, 0.0f);
        minatten = xenakios::jlimit(0.0f, 1.0f, minatten);
        minatten = minatten * minatten;
        outputs[0] = 0.0f;
        outputs[1] = 0.0f;
        for (int i = 0; i < numpartslocal; ++i)
        {
            // partials may sometimes go beyond reasonable limits, so only sum the ones within
            // frequency limits
            float pfreq = m_partial_freqs[i];
            if (pfreq >= AdditiveSharedData::minpartialfrequency &&
                pfreq < AdditiveSharedData::maxpartialfrequency)
            {
                float gain0 = m_shared_data->partialsmorphtable[amp_morph_i0][i];
                float gain1 = m_shared_data->partialsmorphtable[amp_morph_i1][i];
                // main partial frequency morphing
                float interp_gain = gain0 + (gain1 - gain0) * amp_morph_frac;
                // creative filter
                interp_gain *= m_partial_shapingfiltergains[i];
                // extreme low and high frequency cutoffs
                interp_gain *= m_partial_safetyfiltergains[i];
                // one pole lowpass filtering for the gain changes
                float &old = m_partial_vol_smoothing_history[i];
                float smoothed = interp_gain + m_gain_smoothing_coeff * (old - interp_gain);
                interp_gain = smoothed;
                old = smoothed;

                m_partial_amplitudes[i] = interp_gain; // for visualization

                float pan0 = m_shared_data->partialspanmorphtable[pan_morph_i0][i];
                float pan1 = m_shared_data->partialspanmorphtable[pan_morph_i1][i];
                float interp_pan = pan0 + (pan1 - pan0) * pan_morph_frac;
                interp_pan -= 0.5f;  // is now -0.5 to 0.5
                interp_pan += m_pan; // if voice pan at 0.5, back to 0.0 to 1.0
                // however, if voice pan is at say 0.75, adding 0.5 would make 1.25 which we
                // can't handle so reflect back inside the 0 to 1 range this could have at least
                // 3 modes : clamp, reflect and wrap which we might make a parameter/option
                // later
                if (interp_pan >= 1.0f)
                    interp_pan = 1.0f - (interp_pan - 1.0f);
                else if (interp_pan < 0.0f)
                    interp_pan = -interp_pan;
                // we have the valid pan value
                // and now smooth with lowpass filters...
                float &oldpan = m_partial_pan_smoothing_history[i];
                smoothed = interp_pan + m_pan_smoothing_coeff * (oldpan - interp_pan);
                oldpan = smoothed;
                interp_pan = smoothed;
                m_partial_pans[i] = interp_pan; // for visualization
                int panCoeffIndex = (m_shared_data->pan_coefficients[0].size() - 1) * interp_pan;
                // we both jassert and clamp, so we can catch in debug builds
                assert(panCoeffIndex >= 0 &&
                       panCoeffIndex < m_shared_data->pan_coefficients[0].size());
                // panCoeffIndex =
                // xenakios::jlimit<int>(0,(int)m_shared_data->pan_coefficients[0].size()-1,panCoeffIndex);

                float leftGain = m_shared_data->pan_coefficients[0][panCoeffIndex];
                float rightGain = m_shared_data->pan_coefficients[1][panCoeffIndex];
                float po = partial_outputs[i] * interp_gain;
                outputs[1] += po * rightGain;
                outputs[0] += po * leftGain;
            }
            else
                m_partial_amplitudes[i] = 0.0f; // for visualization

            float phase = m_partial_phases[i];
            phase += m_partial_phaseincs[i];
            if (phase >= M_PI * 2)
                phase -= M_PI * 2;
            m_partial_phases[i] = phase;
        }

        m_adsr_burst_mix = 0.0f;
        float finalgain = (1.0 - m_adsr_burst_mix) * envgain + m_adsr_burst_mix * burst_eg;
        finalgain *= volmodgain;
        // might want to visualize velocity too, but let's not, for now
        m_adsr_vis_amp = finalgain;
        // output_frame[0] = output*envgain*m_cur_velo*m_pan;
        // output_frame[1] = output*envgain*m_cur_velo*(1.0-m_pan);
        // float send_gain = m_aux_send_a + lfo_destinations[AdditiveSharedData::MOT_AUX_SEND_A];
        // send_gain = xenakios::jlimit(0.0f, 1.0f, send_gain);
        // send_gain = -48.0f + 48.0f * send_gain;
        // send_gain = xenakios::decibelsToGain(send_gain);

        destBuf.getSample(0, outbufpos) +=
            outputs[0] * m_cur_velo_gain * finalgain * m_partial_gain_compen;
        destBuf.getSample(1, outbufpos) +=
            outputs[1] * m_cur_velo_gain * finalgain * m_partial_gain_compen;
        // output_frame[0] = outputs[0] * m_cur_velo_gain * finalgain * m_partial_gain_compen;
        // output_frame[1] = outputs[1] * m_cur_velo_gain * finalgain * m_partial_gain_compen;
        // output_frame[2] = output_frame[0] * send_gain;
        // output_frame[3] = output_frame[1] * send_gain;
        // deactivate voice when ADSR finished
        if (m_eg0->stage == VoiceEG::s_eoc)
        {
            m_is_available = true;
        }
        ++state_update_counter;
        if (state_update_counter == SRProvider::BLOCK_SIZE)
            state_update_counter = 0;
    }
}

#pragma float_control(pop)

AdditiveSynth::AdditiveSynth() {}

void AdditiveSynth::prepare(double sampleRate, int maxbufsize)
{
    m_mixbuf = choc::buffer::ChannelArrayBuffer<float>(2, (unsigned int)maxbufsize);
    m_shared_data.sst_provider.setSampleRate(sampleRate);
    for (auto &e : m_voices)
    {
        e.setSampleRate(sampleRate);
        e.setSharedData(&m_shared_data);
    }
}

void AdditiveSynth::processBlock(choc::buffer::ChannelArrayView<float> destBuf)
{
    // yes, we know...no locks should be used, but this won't be contested much
    std::lock_guard<std::mutex> locker(m_cs);
    auto mixbufView =
        m_mixbuf.getSection(choc::buffer::ChannelRange{0, 2}, {0, destBuf.getNumFrames()});
    mixbufView.clear();
    // might want to avoid doing this for voices that are not active or
    // if parameters have not changed, but this will have to do for now
    int voicecount = 0;
    for (auto &v : m_voices)
    {
        // v.updateState();
        if (v.m_is_available == false)
            ++voicecount;
    }
    m_num_active_voices = voicecount;
    m_mixbuf.clear();
    for (auto &v : m_voices)
    {
        if (!v.m_is_available)
        {
            v.process(mixbufView);
        }
    }
    for (int i = 0; i < mixbufView.getNumFrames(); ++i)
    {
        mixbufView.getSample(0, i) = std::tanh(mixbufView.getSample(0, i));
        mixbufView.getSample(1, i) = std::tanh(mixbufView.getSample(1, i));
    }
    choc::buffer::applyGain(mixbufView, 1.0);
    choc::buffer::copy(destBuf, mixbufView);
    m_time_pos_counter += destBuf.getNumFrames();
}

void AdditiveSynth::setPitchBendRange(double range) { m_pitch_bend_range = range; }

void AdditiveSynth::handleParameterValue(int port, int ch, int key, int note_id, clap_id parid,
                                         double value)
{
    for (auto &v : m_voices)
    {
        if ((key == -1 || v.m_cur_midi_note == key) && (note_id == -1 || v.m_note_id == note_id) &&
            (port == -1 || v.m_note_port == port) && (ch == -1 || v.m_note_channel == ch))
        {
            if (parid == 0)
                v.m_filter_morph = value;
            if (parid == 1)
                v.setPan(value);
        }
    }
}

void AdditiveSynth::handlePolyAfterTouch(int port_index, int channel, int note, float value)
{
    // uuf, we have to find the active voices playing the key
    for (auto &v : m_voices)
    {
        if (!v.m_is_available && v.m_cur_midi_note == note)
        {
            v.m_after_touch_amount = value;
            // DBG(value);
        }
    }
}

void AdditiveSynth::handleNoteOn(int port_index, int channel, int key, int noteid, double velo)
{
    bool found = false;
    auto startnotefunc = [this](AdditiveVoice &v, int port_index, int channel, int key, int noteid,
                                double velo) {
        v.beginNote(port_index, channel, key, noteid, velo);
        v.m_pitch_bend_amount = m_cur_pitch_bend;
        v.updateState();
        v.m_start_time_stamp = m_time_pos_counter;
        // if we want to do something like this, should be a voice on triggered modulator or
        // something
        /*
        if (m_note_counter == 0)
            v.setPan(0.1);
        else if (m_note_counter == 1)
            v.setPan(0.5);
        else v.setPan(0.9);
        */
        ++m_note_counter;
        if (m_note_counter == 3)
            m_note_counter = 0;
    };
    for (auto &v : m_voices)
    {
        if (v.m_is_available)
        {
            startnotefunc(v, port_index, channel, key, noteid, velo);
            found = true;
            break;
        }
    }
    if (!found)
    {
        // Find oldest playing voice and unceremoniously restart it
        // Not a very good voice stealing method, but we need to have something
        AdditiveVoice *stealfrom = nullptr;
        int mintime = 100000000; // should be intmax
        for (auto &v : m_voices)
        {
            if (v.m_start_time_stamp < mintime)
            {
                mintime = v.m_start_time_stamp;
                stealfrom = &v;
            }
        }
        if (stealfrom)
        {
            startnotefunc(*stealfrom, port_index, channel, key, noteid, velo);
        }
    }
}

void AdditiveSynth::handleNoteOff(int port_index, int channel, int key, int noteid)
{
    if (m_sustain_pedal)
        return;
    for (auto &v : m_voices)
    {
        if (v.m_is_available == false && (v.m_cur_midi_note == key || key == -1))
        {
            v.endNote();
            v.m_after_touch_amount = 0.0f;
        }
    }
}

void AdditiveSynth::handleCC(int port_index, int channel, int cc, int value)
{
    if (cc == 64)
    {
        bool old = m_sustain_pedal;
        m_sustain_pedal = value >= 64;
        if (old == true && m_sustain_pedal == false)
        {
            handleNoteOff(-1, -1, -1, -1);
        }
    }
    else if (cc >= 41 && cc < 45)
    {
        for (auto &v : m_voices)
        {
            // CCs can change even if notes not playing, so just update all voices here
            v.setCC(cc, 1.0 / 127 * value);
        }
    }
}

void AdditiveSynth::handlePitchBend(int port_index, int channel, float value)
{
    m_cur_pitch_bend = value;
    for (auto &v : m_voices)
    {
        // if (!v.m_is_available)
        {
            v.m_pitch_bend_amount = value;
            v.updateState();
        }
    }
}
#ifdef HAVEJUCE
juce::String AdditiveSynth::importKBMText(juce::String text)
{
    try
    {
        auto kbm = Tunings::parseKBMData(text.toStdString());
        auto scale = m_shared_data.fundamental_tuning.scale;
        Tunings::Tuning tuning = Tunings::Tuning(scale, kbm);
        juce::SpinLock::ScopedLockType locker(m_cs);
        m_shared_data.fundamental_tuning = tuning;
    }
    catch (const std::exception &e)
    {
        return e.what();
    }
    return "";
}

juce::String AdditiveSynth::importKBMFile(juce::File file)
{
    std::string fn = file.getFullPathName().toStdString();
    try
    {
        auto kbm = Tunings::readKBMFile(fn);
        auto scale = m_shared_data.fundamental_tuning.scale;
        Tunings::Tuning tuning = Tunings::Tuning(scale, kbm);
        juce::SpinLock::ScopedLockType locker(m_cs);
        m_shared_data.fundamental_tuning = tuning;
    }
    catch (const std::exception &e)
    {
        return e.what();
    }
    return "";
}
#endif
#ifdef HAVEJUCE
juce::String AdditiveSynth::importScalaFile(juce::File file)
{
    std::string fn = file.getFullPathName().toStdString();
    try
    {
        auto scale = Tunings::readSCLFile(fn);
        auto kbm = Tunings::startScaleOnAndTuneNoteTo(m_kbm_start_note, m_kbm_ref_note, m_kbm_freq);
        Tunings::Tuning tuning = Tunings::Tuning(scale, kbm);
        juce::SpinLock::ScopedLockType locker(m_cs);
        m_shared_data.fundamental_tuning = tuning;
    }
    catch (const std::exception &e)
    {
        return e.what();
    }
    return "";
}
#endif
void AdditiveSynth::setEDOParameters(double pseudoOctaveCents, int edo)
{

    if (pseudoOctaveCents != m_pseudo_octave || edo != m_edo)
    {
        try
        {

            // double t0 = juce::Time::getMillisecondCounterHiRes();
            auto scale = Tunings::evenDivisionOfCentsByM(pseudoOctaveCents, edo);
            auto kbm = m_shared_data.fundamental_tuning.keyboardMapping;
            auto tuning = Tunings::Tuning(scale, kbm);
            m_shared_data.fundamental_tuning = tuning;
            // double t1 = juce::Time::getMillisecondCounterHiRes();
            m_tuning_update_elapsed_ms = 0.0f;
            m_pseudo_octave = pseudoOctaveCents;
            m_edo = edo;
            m_has_error = false;
            m_error_text = "";
        }
        catch (const std::exception &e)
        {
            m_error_text = e.what();
            m_has_error = true;
        }
    }
}

float BurstGenerator::process()
{
    float output = 0.0;
    // ok, obviously this isn't as efficient as it should be, but will do for now
    float tposnorm = getBurstTime(m_counter);
    if (m_counter < m_num_bursts && std::abs(m_phase - tposnorm * m_sr) <= 1.0)
    {
        if (m_dist(m_rng) < m_probability)
        {
            env.attackFrom(cur_out, 0.0f, 0, true);
        }
        ++m_counter;
    }
    m_phase += 1.0;
    env.process(0.01, 0.3, 0.00, 0.6, 0, 0, 0, m_counter <= m_num_bursts);
    output = env.output;
    // jassert(output>=0.0 && output<=1.0);
    cur_out = output;
    if (m_auto_repeat && m_phase >= m_burst_duration * m_sr)
    {
        begin();
    }

    return output;
}

float BurstGenerator::getBurstTime(int index)
{
    float tposnorm = 1.0 / m_num_bursts * index;
    tposnorm = xenakios::jlimit(0.0f, 1.0f, tposnorm);
    if (m_distrib < 0.5)
    {
        double raise_to = xenakios::mapvalue(m_distrib, 0.0, 0.5, 3.0, 1.0);
        tposnorm = std::pow(tposnorm, raise_to) * m_burst_duration;
    }
    else if (m_distrib > 0.5)
    {
        double raise_to = xenakios::mapvalue(m_distrib, 0.5, 1.0, 1.0, 3.0);
        tposnorm = (1.0 - std::pow(1.0 - tposnorm, raise_to)) * m_burst_duration;
    }
    else
        tposnorm *= m_burst_duration;
    return tposnorm;
}
