#include <iostream>
#include <memory>
#include "klangas/sineosc.h"
#include "audio/choc_AudioFileFormat_WAV.h"
#include "noiseplethora/noiseplethoraengine.h"

inline void test_klangas()
{
    double sr = 44100.0;
    unsigned int bufsize = 441;
    choc::audio::AudioFileProperties props;
    props.bitDepth = choc::audio::BitDepth::float32;
    props.numChannels = 2;
    props.formatName = "WAV";
    props.sampleRate = sr;
    choc::audio::WAVAudioFileFormat<true> wavformat;
    auto writer = wavformat.createWriter("C:/develop/xen_clap_plugins/klangasnew02.wav", props);
    choc::buffer::ChannelArrayBuffer<float> procbuf{2, bufsize};
    procbuf.clear();
    auto as = std::make_unique<AdditiveSynth>();

    as->prepare(sr, bufsize);

    as->handleNoteOn(0, 0, 40, -1, 1.0);
    int outlen = 10 * sr;
    int outcount = 0;
    as->m_shared_data.setVolumeMorphPreset(1);
    as->m_shared_data.setPanMorphPreset(0);
    xenakios::Envelope<64> tweaksEnv;
    tweaksEnv.addPoint({0.0, 0.0});
    tweaksEnv.addPoint({5.0, 1.0});
    tweaksEnv.addPoint({5.1, 0.0});
    tweaksEnv.addPoint({10.0, 0.5});
    xenakios::Envelope<64> filterEnv;
    filterEnv.addPoint({0.0, 1.0});
    filterEnv.addPoint({5.0, 0.2});
    filterEnv.addPoint({10.0, 1.0});

    while (outcount < outlen)
    {
        double secpos = outcount / sr;
        double tweaksVal = tweaksEnv.getValueAtPosition(secpos, sr);
        double filterVal = filterEnv.getValueAtPosition(secpos, sr);
        for (auto &v : as->m_voices)
        {
            v.setNumPartials(32);
            v.setADSRParameters(0, 0.2, 0.6, 0.5, 0.2);
            v.setADSRParameters(1, 0.2, 0.2, 0.5, 0.2);
            v.m_freq_tweaks_mode = 4;
            v.m_freq_tweaks_mix = tweaksVal;
            v.m_adsr_burst_mix = 0.0;
            v.m_base_volume = -9.0;
            v.m_filter_morph = filterVal;
            v.m_filter_mode = 2;
            v.setPartialsBalance(1.0);
            v.setPartialsPanMorph(0.0);
        }

        as->processBlock(procbuf.getView());
        choc::buffer::applyGain(procbuf, 0.9);
        writer->appendFrames(procbuf.getView());
        outcount += bufsize;
    }
}

inline void test_noise_plethora_monomode()
{
    double sr = 44100.0;
    unsigned int bufsize = 256;
    choc::audio::AudioFileProperties props;
    props.bitDepth = choc::audio::BitDepth::float32;
    props.numChannels = 2;
    props.formatName = "WAV";
    props.sampleRate = sr;
    choc::audio::WAVAudioFileFormat<true> wavformat;
    auto writer =
        wavformat.createWriter("C:/develop/xen_clap_plugins/noiseplethora_mono01.wav", props);
    choc::buffer::ChannelArrayBuffer<float> procbuf{2, bufsize};
    procbuf.clear();
    auto nps = std::make_unique<NoisePlethoraSynth>();
    nps->prepare(sr, bufsize);
    nps->applyParameter(0, 0, -1, -1, (clap_id)NoisePlethoraSynth::ParamIDs::PolyphonyMode, 1.0);
    // nps->applyParameter(0, 0, -1, -1, (clap_id)NoisePlethoraSynth::ParamIDs::PolyphonyMode, 0.0);
    nps->startNote(0, 0, 60, -1, 1.0);
    int outlen = 44100 * 5;
    int outcount = 0;
    int note_endpos = 4 * 44100;
    while (outcount < outlen)
    {
        if (note_endpos >= outcount  && note_endpos < (outcount + bufsize) )
        {
            nps->stopNote(0, 0, 60, -1, 0.0);
            std::cout << "stopped note\n";
        }
        nps->processBlock(procbuf.getView());
        writer->appendFrames(procbuf.getView());
        outcount += bufsize;
    }
}

int main()
{
    test_noise_plethora_monomode();
    // test_klangas();
    std::cout << "finished\n";
}
