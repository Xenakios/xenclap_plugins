#include <iostream>
#include "klangas/sineosc.h"
#include "audio/choc_AudioFileFormat_WAV.h"

inline void test_klangas()
{
    double sr = 44100.0;
    unsigned int bufsize = 64;
    choc::audio::AudioFileProperties props;
    props.bitDepth = choc::audio::BitDepth::float32;
    props.numChannels = 2;
    props.formatName = "WAV";
    props.sampleRate = sr;
    choc::audio::WAVAudioFileFormat<true> wavformat;
    auto writer = wavformat.createWriter("C:/develop/xen_clap_plugins/klangasnew01.wav", props);
    choc::buffer::ChannelArrayBuffer<float> procbuf{2, bufsize};
    procbuf.clear();
    auto as = std::make_unique<AdditiveSynth>();

    as->prepare(sr, bufsize);

    as->handleNoteOn(0, 0, 40, -1, 1.0);
    int outlen = 10 * sr;
    int outcount = 0;
    as->m_shared_data.setVolumeMorphPreset(1);
    as->m_shared_data.setPanMorphPreset(0);
    while (outcount < outlen)
    {
        double val = std::fmod(1.0 / 88200 * outcount, 1.0f);
        val = std::clamp(val, 0.0, 1.0);
        for (auto &v : as->m_voices)
        {
            v.setNumPartials(32);
            v.setADSRParameters(0, 0.2, 0.6, 0.5, 0.2);
            v.setADSRParameters(1, 0.2, 0.2, 0.5, 0.2);
            // v.setTuningMode(0);
            v.m_freq_tweaks_mode = 0;
            v.m_freq_tweaks_mix = 0;
            v.m_adsr_burst_mix = 0.0;
            v.m_base_volume = -9.0;
            v.m_filter_morph = val;
            v.m_filter_mode = 2;
            v.setPartialsBalance(1.0);
            v.setPartialsPanMorph(0.1);
        }

        as->processBlock(procbuf.getView());
        choc::buffer::applyGain(procbuf, 0.9);
        writer->appendFrames(procbuf.getView());
        outcount += bufsize;
    }
}

int main() { test_klangas(); }
