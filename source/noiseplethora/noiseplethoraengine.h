#pragma once

#include "noise-plethora/plugins/NoisePlethoraPlugin.hpp"
#include "noise-plethora/plugins/Banks.hpp"
#include "audio/choc_AudioFileFormat_WAV.h"
#include "sst/basic-blocks/dsp/PanLaws.h"
#include "sst/basic-blocks/modulators/ADSREnvelope.h"
#include "sst/basic-blocks/dsp/FollowSlewAndSmooth.h"
#include "gui/choc_DesktopWindow.h"
#include "gui/choc_MessageLoop.h"
#include "gui/choc_WebView.h"
#include "text/choc_Files.h"
#include "../xap_utils.h"
#include "../xapdsp.h"
#include "../common.h"
// #define USE_SST_VM

#ifdef USE_SST_VM
#include "sst/voicemanager/voicemanager.h"
#endif


constexpr size_t ENVBLOCKSIZE = 64;

struct SRProviderB
{
    static constexpr int BLOCK_SIZE = ENVBLOCKSIZE;
    static constexpr int BLOCK_SIZE_OS = BLOCK_SIZE * 2;
    SRProviderB() { initTables(); }
    alignas(32) float table_envrate_linear[512];
    double samplerate = 44100.0;
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
};

// this was tested to work for this particular use case, but the more generic function
// in utils should be checked/fixed too
template <typename T> inline T wrap_algo(const T minval, const T val, const T maxval)
{
    T temp = val;
    while (temp < minval || temp > maxval)
    {
        if (temp < minval)
            temp = maxval - (minval - temp) + 1;
        if (temp > maxval)
            temp = minval + (temp - maxval) - 1;
    }
    return temp;
}

class NoisePlethoraVoice
{
  public:
    struct VoiceParams
    {
        float volume = -6.0f;
        float x = 0.5f;
        float y = 0.5f;
        float filtcutoff = 120.0f;
        float filtreson = 0.01;
        float algo = 0.0f;
        float pan = 0.0f;
        float filttype = 0.0f;
    };
    VoiceParams basevalues;
    VoiceParams modvalues;
    float note_expr_pressure = 0.0f;
    float note_expr_pan = 0.0f;
    float keytrack_x_mod = 0.0f;
    float keytrack_y_mod = 0.0f;
    using EnvType = sst::basic_blocks::modulators::ADSREnvelope<SRProviderB, ENVBLOCKSIZE>;
    SRProviderB m_sr_provider;
    EnvType m_vol_env{&m_sr_provider};

    NoisePlethoraVoice()
    {
        modvalues.algo = 0;
        modvalues.filtcutoff = 0;
        modvalues.filtreson = 0;
        modvalues.pan = 0;
        modvalues.volume = 0;
        modvalues.x = 0;
        modvalues.y = 0;
        modvalues.filttype = 0.0f;
        for (int i = 0; i < numBanks; ++i)
        {
            auto &bank = getBankForIndex(i);
            for (int j = 0; j < programsPerBank; ++j)
            {
                auto progname = bank.getProgramName(j);
                auto p = MyFactory::Instance()->Create(progname);
                m_plugs.push_back(std::move(p));
            }
        }
    }
    double hipasscutoff = 24.0;
    void prepare(double sampleRate)
    {
        m_sr_provider.samplerate = sampleRate;
        m_sr_provider.initTables();
        m_sr = sampleRate;
        dcblocker.setCoeff(hipasscutoff, 0.01, 1.0 / m_sr);
        dcblocker.init();
        for (auto &f : filters)
        {
            f.init();
        }

        m_gain_slew.setParams(10.0f, 1.0f, sampleRate);
        m_pan_slew.setParams(20.0f, 1.0f, sampleRate);
        for (auto &p : m_plugs)
        {
            p->m_sr = m_sr;
            p->init();
        }
        m_cur_plugin = m_plugs[0].get();
    }
    NoisePlethoraPlugin *m_cur_plugin = nullptr;
    double velocity = 1.0;
    bool m_eg_gate = false;
    bool m_voice_active = false;
    bool m_voice_was_started = false;
    void activate(int port_, int chan_, int key_, int id_, double velo)
    {
        m_voice_was_started = true;
        velocity = velo;
        port_id = port_;
        chan = chan_;
        key = key_;
        note_id = id_;
        int curplug = basevalues.algo;
        curplug = std::clamp(curplug, 0, (int)m_plugs.size() - 1);
        m_cur_plugin = m_plugs[curplug].get();
        m_cur_plugin->init();
        m_voice_active = true;
        m_eg_gate = true;
        m_vol_env.attackFrom(0.0f, 0.0f, 0, true);
        keytrack_x_mod = 0.0f;
        keytrack_y_mod = 0.0f;
        visualizationDirty = true;

        // theoretically possible was started with key -1, although no longer endorsed by Clap
        if (key == -1)
            return;
        updateKeyTrackMods(key);
    }
    void updateKeyTrackMods(float key)
    {
        if (keytrackMode == 1 || keytrackMode == 2)
        {
            // calculate x and y mods from key, using a Lissajous style mapping
            float t = xenakios::mapvalue<float>(key, 0, 127, -M_PI, M_PI);
            float hz0 = 10.0;
            float hz1 = 11.0;
            if (keytrackMode == 2)
            {
                hz0 = 4.0;
                hz1 = 7.13;
            }
            keytrack_x_mod = 0.5 * std::sin(t * hz0);
            keytrack_y_mod = 0.5 * std::cos(t * hz1);
        }
        else if (keytrackMode >= 3 && keytrackMode <= 5)
        {
            const int gsizes[3] = {3, 5, 7};
            int gsize = gsizes[keytrackMode - 3];
            int numcells = gsize * gsize;
            int modkey = (int)key % numcells;
            int ix = modkey % gsize;
            int iy = modkey / gsize;
            keytrack_x_mod = xenakios::mapvalue<float>(ix, 0, gsize - 1, -0.5f, 0.5f);
            keytrack_y_mod = xenakios::mapvalue<float>(iy, 0, gsize - 1, -0.5f, 0.5f);
        }
    }
    void deactivate() { m_eg_gate = false; }
    int m_update_counter = 0;
    float eg_attack = 0.1f;
    float eg_decay = 0.5f;
    float eg_sustain = 0.75f;
    float eg_release = 0.5f;
    int keytrackMode = 0;
    bool isTheMonoVoice = false;
    int filterCount = 1;
    std::function<void(int, int, int, int)> DeativatedVoiceCallback;
    float totalx = 0.0f;
    float totaly = 0.0f;
    float total_gain = 0.0f;
    bool visualizationDirty = false;
    // must accumulate into the buffer, precleared by the synth before processing the first voice

    void process(choc::buffer::ChannelArrayView<float> destBuf)
    {
        if (!m_voice_active)
            return;
        // when algo is modulated, we don't want to get stuck at the first or last algo
        int safealgo =
            wrap_algo<float>(0, basevalues.algo + modvalues.algo, (int)m_plugs.size() - 1);
        assert(safealgo >= 0 && safealgo < m_plugs.size());
        auto plug = m_plugs[safealgo].get();
        m_cur_plugin = plug;
        float expr_x = xenakios::mapvalue(note_expr_pressure, 0.0f, 1.0f, -0.5f, 0.5f);
        float tempx = std::clamp(basevalues.x + modvalues.x + keytrack_x_mod, 0.0f, 1.0f);
        float tempy = std::clamp(basevalues.y + modvalues.y + keytrack_y_mod, 0.0f, 1.0f);
        if (tempx != totalx || tempy != totaly)
        {
            visualizationDirty = true;
            totalx = tempx;
            totaly = tempy;
        }
        plug->process(totalx, totaly);
        float velodb = -18.0 + 18.0 * velocity;
        float totalvol = std::clamp(basevalues.volume + modvalues.volume + velodb, -96.0f, 0.0f);
        float gain = xenakios::decibelsToGain(totalvol);

        StereoSimperSVF::Mode ftype = (StereoSimperSVF::Mode)(int)basevalues.filttype;
        filterCount = std::clamp(filterCount, 1, 16);
        double totalcutoff = std::clamp(basevalues.filtcutoff + modvalues.filtcutoff, 0.0f, 127.0f);
        double totalreson = std::clamp(basevalues.filtreson + modvalues.filtreson, 0.01f, 0.99f);
        for (int i = 0; i < filterCount; ++i)
        {
            filters[i].setCoeff(totalcutoff, totalreson, 1.0 / m_sr);
        }

        sst::basic_blocks::dsp::pan_laws::panmatrix_t panmat;
        float basepan = xenakios::mapvalue(basevalues.pan, -1.0f, 1.0f, 0.0f, 1.0f);
        float expr_pan = xenakios::mapvalue(note_expr_pan, 0.0f, 1.0f, -0.5f, 0.5f);
        float totalpan = reflect_value(0.0f, basepan + modvalues.pan, 1.0f);
        if (m_voice_was_started)
        {
            m_voice_was_started = false;
            m_gain_slew.setLast(gain);
            m_pan_slew.setLast(totalpan);
        }
        auto chansdata = destBuf.data.channels;
        for (size_t i = 0; i < destBuf.size.numFrames; ++i)
        {
            if (m_update_counter == 0)
            {
                m_vol_env.processBlock(eg_attack, eg_decay, eg_sustain, eg_release, 1, 1, 1,
                                       m_eg_gate);
            }
            float envgain = m_vol_env.outputCache[m_update_counter];
            ++m_update_counter;
            if (m_update_counter == ENVBLOCKSIZE)
                m_update_counter = 0;
            auto smoothedgain = m_gain_slew.step(gain);
            auto smoothedpan = m_pan_slew.step(totalpan);
            // does expensive calculation, so might want to use tables or something instead
            sst::basic_blocks::dsp::pan_laws::monoEqualPower(smoothedpan, panmat);
            float finalgain = smoothedgain * envgain;
            total_gain = finalgain;
            float out = plug->processGraph();
            float dummy = 0.0f;
            dcblocker.step<StereoSimperSVF::HP>(dcblocker, out, dummy);

            switch (ftype)
            {
            case StereoSimperSVF::LP:
            {
                filters[0].step<StereoSimperSVF::LP>(filters[0], out, dummy);
                break;
            }
            case StereoSimperSVF::HP:
            {
                filters[0].step<StereoSimperSVF::HP>(filters[0], out, dummy);
                break;
            }
            case StereoSimperSVF::BP:
            {
                filters[0].step<StereoSimperSVF::BP>(filters[0], out, dummy);
                break;
            }
            case StereoSimperSVF::PEAK:
            {
                filters[0].step<StereoSimperSVF::PEAK>(filters[0], out, dummy);
                break;
            }
            case StereoSimperSVF::NOTCH:
            {
                filters[0].step<StereoSimperSVF::NOTCH>(filters[0], out, dummy);
                break;
            }
            case StereoSimperSVF::ALL:
            {
                float cachedOut = out;
                for (int i = 0; i < filterCount; ++i)
                {
                    filters[i].step<StereoSimperSVF::ALL>(filters[i], out, dummy);
                }

                out = 0.5f * out + 0.5f * cachedOut;
                break;
            }
            default:
                break;
            };
            out *= finalgain;
            float outL = panmat[0] * out;
            float outR = panmat[3] * out;
            chansdata[0][i] += outL;
            chansdata[1][i] += outR;
        }
        if (m_vol_env.stage == EnvType::s_eoc)
        {
            m_voice_active = false;
            if (DeativatedVoiceCallback)
                DeativatedVoiceCallback(port_id, chan, key, note_id);
        }
    }
    int port_id = 0;
    int chan = 0;
    int key = 0;
    int note_id = -1;

  private:
    // the noise plethora code calls the algos "plugins", which is a bit confusing
    std::vector<std::unique_ptr<NoisePlethoraPlugin>> m_plugs;
    sst::basic_blocks::dsp::SlewLimiter m_gain_slew;
    sst::basic_blocks::dsp::SlewLimiter m_pan_slew;
    double m_sr = 0.0;
    StereoSimperSVF dcblocker;

    std::array<StereoSimperSVF, 16> filters;
};

class NoisePlethoraSynth
{
  public:
    using voice_t = NoisePlethoraVoice;
    static constexpr size_t maxVoiceCount = 32;
#ifdef USE_SST_VM
    struct ConcreteMonoResp
    {
        void setMIDIPitchBend(int16_t channel, int16_t pb14bit) {}
        void setMIDI1CC(int16_t channel, int16_t cc, int16_t val) {}
        void setMIDIChannelPressure(int16_t channel, int16_t pres) {}
    };
    ConcreteMonoResp monoResponder;

    sst::voicemanager::VoiceManager<NoisePlethoraSynth, NoisePlethoraSynth, ConcreteMonoResp>
        m_voice_manager;

    NoisePlethoraSynth() : m_voice_manager(*this, monoResponder)
#else
    NoisePlethoraSynth()
#endif
    {
        deactivatedNotes.reserve(1024);
        for (size_t i = 0; i < maxVoiceCount; ++i)
        {
            auto v = std::make_unique<NoisePlethoraVoice>();
            v->DeativatedVoiceCallback = [this](int port, int chan, int key, int noteid) {
                deactivatedNotes.emplace_back(port, chan, key, noteid);
            };
            m_voices.push_back(std::move(v));
        }
    }
    ~NoisePlethoraSynth()
    {
        if (voicecount > 0)
        {
            std::cout << "voices left at synth dtor " << voicecount << "\n";
        }
    }
    void setVoiceEndCallback(std::function<void(voice_t *)>) {}
    void prepare(double sampleRate, int maxBlockSize)
    {
        m_sr = sampleRate;
        m_mix_buf = choc::buffer::ChannelArrayBuffer<float>(2, (unsigned int)maxBlockSize);
        for (auto &v : m_voices)
        {
            v->prepare(sampleRate);
        }
    }
    void applyNoteExpression(int port, int ch, int key, int note_id, int net, double amt)
    {
        for (auto &v : m_voices)
        {
            if ((key == -1 || v->key == key) && (note_id == -1 || v->note_id == note_id) &&
                (port == -1 || v->port_id == port) && (ch == -1 || v->chan == ch))
            {
                if (net == 6)
                    v->note_expr_pressure = amt;
                if (net == 1)
                    v->note_expr_pan = amt;
            }
        }
    }
    void applyParameterModulation(int port, int ch, int key, int note_id, clap_id parid, double amt)
    {
        for (auto &v : m_voices)
        {
            if ((key == -1 || v->key == key) && (note_id == -1 || v->note_id == note_id) &&
                (port == -1 || v->port_id == port) && (ch == -1 || v->chan == ch))
            {
                if (parid == (clap_id)ParamIDs::Volume)
                    v->modvalues.volume = amt;
                if (parid == (clap_id)ParamIDs::X)
                    v->modvalues.x = amt;
                if (parid == (clap_id)ParamIDs::Y)
                    v->modvalues.y = amt;
                if (parid == (clap_id)ParamIDs::Algo)
                    v->modvalues.algo = amt;
                if (parid == (clap_id)ParamIDs::Pan)
                    v->modvalues.pan = amt;
                if (parid == (clap_id)ParamIDs::FiltCutoff)
                    v->modvalues.filtcutoff = amt;
                if (parid == (clap_id)ParamIDs::FiltResonance)
                    v->modvalues.filtreson = amt;
            }
        }
    }
    void applyParameter(int port, int ch, int key, int note_id, clap_id parid, double value)
    {
        // std::cout << "par " << parid << " " << value << "\n";
        if (parid == (clap_id)ParamIDs::PolyphonyMode)
        {
            if (!monoMode && (bool)value)
            {
                for (auto &v : m_voices)
                {
                    v->deactivate();
                }
                m_voices.front()->isTheMonoVoice = true;
                std::cout << "turned on mono mode\n";
            }
            monoMode = value;
            if (!monoMode)
            {
                m_voices.front()->deactivate();
                m_voices.front()->isTheMonoVoice = false;
                std::cout << "turned on poly mode\n";
            }
            return;
        }
        for (auto &v : m_voices)
        {
            if ((key == -1 || v->key == key) && (note_id == -1 || v->note_id == note_id) &&
                (port == -1 || v->port_id == port) && (ch == -1 || v->chan == ch))
            {
                // std::cout << "applying par " << parid << " to " << port << " " << ch << " " <<
                // key
                //           << " " << note_id << " " << value << "\n";
                if (parid == (clap_id)ParamIDs::Volume)
                    v->basevalues.volume = value;
                if (parid == (clap_id)ParamIDs::X)
                    v->basevalues.x = value;
                if (parid == (clap_id)ParamIDs::Y)
                    v->basevalues.y = value;
                if (parid == (clap_id)ParamIDs::Algo)
                    v->basevalues.algo = value;
                if (parid == (clap_id)ParamIDs::Pan)
                    v->basevalues.pan = value;
                if (parid == (clap_id)ParamIDs::FiltCutoff)
                    v->basevalues.filtcutoff = value;
                if (parid == (clap_id)ParamIDs::FiltResonance)
                    v->basevalues.filtreson = value;
                if (parid == (clap_id)ParamIDs::FiltType)
                    v->basevalues.filttype = value;
                if (parid == (clap_id)ParamIDs::EGAttack)
                    v->eg_attack = value;
                if (parid == (clap_id)ParamIDs::EGDecay)
                    v->eg_decay = value;
                if (parid == (clap_id)ParamIDs::EGSustain)
                    v->eg_sustain = value;
                if (parid == (clap_id)ParamIDs::EGRelease)
                    v->eg_release = value;
                if (parid == (clap_id)ParamIDs::KeyTrackMode)
                    v->keytrackMode = value;
                if (parid == (clap_id)ParamIDs::FilterCount)
                    v->filterCount = value;
            }
        }
    }
    std::vector<std::tuple<int, int, int, int>> deactivatedNotes;
    bool monoMode = false;
    int voicecount = 0;
    void startNote(int port, int ch, int key, int note_id, double velo)
    {
        if (monoMode)
        {
            //if (!m_voices.front()->m_voice_active)
            {
                ++voicecount;
                m_voices[0]->activate(port, ch, key, note_id, velo);
            }
            return;
        }
        for (auto &v : m_voices)
        {
            if (!v->m_voice_active)
            {
                ++voicecount;
                // std::cout << "activated " << ch << " " << key << " " << note_id << "\n";
                v->activate(port, ch, key, note_id, velo);
                return;
            }
        }
        std::cout << "could not activate " << ch << " " << key << " " << note_id << "\n";
    }
    void stopNote(int port, int ch, int key, int note_id, double velo)
    {
        if (monoMode)
        {
            if (m_voices.front()->m_voice_active)
            {

            }
            return;
        }
        for (auto &v : m_voices)
        {
            if (v->port_id == port && v->chan == ch && v->key == key &&
                (note_id == -1 || v->note_id == note_id))
            {
                // std::cout << "deactivated " << v->chan << " " << v->key << " " << v->note_id
                //          << "\n";
                --voicecount;
                v->deactivate();
            }
        }
    }
    void processBlock(choc::buffer::ChannelArrayView<float> destBuf)
    {

        auto mixbufView =
            m_mix_buf.getSection(choc::buffer::ChannelRange{0, 2}, {0, destBuf.getNumFrames()});
        mixbufView.clear();
        for (size_t i = 0; i < m_voices.size(); ++i)
        {
            if (m_voices[i]->m_voice_active)
            {
                m_voices[i]->process(mixbufView);
            }
        }
        choc::buffer::applyGain(mixbufView, 0.5);
        choc::buffer::copy(destBuf, mixbufView);
    }

    enum class ParamIDs
    {
        Volume,
        X,
        Y,
        FiltCutoff,
        FiltResonance,
        FiltType,
        Algo,
        Pan,
        EGAttack,
        EGDecay,
        EGSustain,
        EGRelease,
        KeyTrackMode,
        FilterCount,
        PolyphonyMode
    };
    int m_polyphony = 1;
    double m_pan_spread = 0.0;
    double m_x_spread = 0.0;
    double m_y_spread = 0.0;
    NoisePlethoraVoice::VoiceParams renderparams;
    void showGUIBlocking(std::function<void(void)> clickhandler)
    {
        choc::ui::setWindowsDPIAwareness();
        choc::ui::DesktopWindow window({100, 100, (int)600, (int)300});
        choc::ui::WebView webview;
        window.setWindowTitle("Noise Plethora");
        window.setResizable(true);
        window.setMinimumSize(300, 300);
        window.setMaximumSize(1500, 1200);
        window.windowClosed = [this, clickhandler] { choc::messageloop::stop(); };
        webview.bind("onRenderClicked",
                     [clickhandler, this,
                      &webview](const choc::value::ValueView &args) -> choc::value::Value {
                         webview.evaluateJavascript(R"(
                            updateUI("Rendering...");
                            )");
                         clickhandler();
                         webview.evaluateJavascript(R"(
                            updateUI("Finished!");
                            )");
                         return choc::value::Value{};
                     });
        webview.bind("onSliderMoved",
                     [this](const choc::value::ValueView &args) -> choc::value::Value {
                         // note that things could get messed up here because the choc functions can
                         // throw exceptions, so we should maybe have a try catch block here...but
                         // we should just know this will work, really.
                         auto parid = args[0]["id"].get<int>();
                         auto value = args[0]["value"].get<double>();
                         // std::cout << "par " << parid << " changed to " << value << std::endl;
                         if (parid == 0)
                             renderparams.volume = value;
                         if (parid == 1)
                             renderparams.x = value;
                         if (parid == 2)
                             renderparams.y = value;
                         if (parid == 3)
                             renderparams.filtcutoff = value;
                         if (parid == 4)
                             renderparams.filtreson = value;
                         if (parid == 5)
                             renderparams.algo = value;
                         if (parid == 6)
                             renderparams.pan = value;
                         if (parid == 100)
                             m_polyphony = value;
                         if (parid == 101)
                             m_pan_spread = value;
                         if (parid == 200)
                             m_x_spread = value;
                         if (parid == 201)
                             m_y_spread = value;
                         return choc::value::Value{};
                     });
        auto html = choc::file::loadFileAsString(R"(C:\develop\AudioPluginHost_mk2\htmltest.html)");
        window.setContent(webview.getViewHandle());
        webview.setHTML(html);

        window.toFront();
        choc::messageloop::run();
    }
    std::vector<std::unique_ptr<NoisePlethoraVoice>> m_voices;

  private:
    choc::buffer::ChannelArrayBuffer<float> m_mix_buf;
    double m_sr = 0;
    int m_update_counter = 0;
    int m_update_len = 32;
};
