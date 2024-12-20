#include "clap/clap.h"
#include "clap/helpers/plugin.hh"
#include "clap/helpers/plugin.hxx"

#include "clap/helpers/host-proxy.hh"
#include "clap/helpers/host-proxy.hxx"
#include "noiseplethoraengine.h"
#include "sst/basic-blocks/params/ParamMetadata.h"
#include "gui/choc_WebView.h"
#include "containers/choc_SingleReaderSingleWriterFIFO.h"

using ParamDesc = sst::basic_blocks::params::ParamMetaData;

struct UiMessage
{
    UiMessage() {}
    UiMessage(int type_, clap_id parid_, float val) : type{type_}, parid{parid_}, values{val} {}
    int type = 0;
    clap_id parid = CLAP_INVALID_ID;
    float values[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
};

using CommFIFO = choc::fifo::SingleReaderSingleWriterFIFO<UiMessage>;

class NoisePlethoraGUI
{
  public:
    std::vector<ParamDesc> m_paramDescs;
    std::unordered_map<clap_id, ParamDesc *> m_idToParamdesc;
    CommFIFO &m_to_proc_fifo;
    CommFIFO &m_from_proc_fifo;
    NoisePlethoraGUI(std::vector<ParamDesc> paramDescs, CommFIFO &outfifo, CommFIFO &infifo,
                     std::function<float(int)> pargetfunc)
        : m_paramDescs(paramDescs), m_to_proc_fifo(outfifo), m_from_proc_fifo(infifo)
    {
        for (auto &pd : m_paramDescs)
        {
            m_idToParamdesc[pd.id] = &pd;
        }
        choc::ui::WebView::Options opts;
        opts.enableDebugMode = true;
        m_webview = std::make_unique<choc::ui::WebView>(opts);
        m_webview->bind("getParameterText",
                        [this](const choc::value::ValueView &args) -> choc::value::Value {
                            auto parid = args[0]["id"].get<int>();
                            auto value = args[0]["value"].get<double>();
                            auto pd = m_idToParamdesc[parid];
                            auto str = pd->valueToString(value);
                            if (str)
                                return choc::value::Value(*str);
                            return choc::value::Value{};
                        });
        m_webview->bind("getParameterUpdates",
                        [this](const choc::value::ValueView &args) -> choc::value::Value {
                            auto result = choc::value::createEmptyArray();
                            UiMessage msg;
                            while (m_from_proc_fifo.pop(msg))
                            {
                                if (msg.type == CLAP_EVENT_PARAM_VALUE)
                                {
                                    auto info = choc::value::createObject("parupdate");
                                    info.setMember("id", (int64_t)msg.parid);
                                    info.setMember("val", msg.values[0]);
                                    auto pd = m_idToParamdesc[msg.parid];
                                    auto str = pd->valueToString(msg.values[0]);
                                    if (str)
                                        info.setMember("str", *str);
                                    result.addArrayElement(info);
                                }
                                if (msg.type >= 10000)
                                {
                                    auto info = choc::value::createObject("xy");
                                    info.setMember("id", msg.type);
                                    info.setMember("x", msg.values[0]);
                                    info.setMember("y", msg.values[1]);
                                    info.setMember("gain", msg.values[2]);
                                    result.addArrayElement(info);
                                }
                            }
                            return result;
                        });
        m_webview->bind("onSliderMoved",
                        [this](const choc::value::ValueView &args) -> choc::value::Value {
                            // note that things could get messed up here because the choc functions
                            // can throw exceptions, so we should maybe have a try catch block
                            // here...but we should just know this will work, really.
                            auto parid = args[0]["id"].get<int>();
                            auto value = args[0]["value"].get<float>();
                            UiMessage msg{CLAP_EVENT_PARAM_VALUE, (clap_id)parid, value};
                            m_to_proc_fifo.push(msg);
                            auto pd = m_idToParamdesc[msg.parid];
                            auto str = pd->valueToString(msg.values[0]);
                            if (str)
                                return choc::value::Value{*str};
                            return choc::value::Value{};
                        });
        m_webview->bind(
            "getParameters",
            [this, pargetfunc](const choc::value::ValueView &args) -> choc::value::Value {
                auto result = choc::value::createObject("paraminfos");
                for (int i = 0; i < m_paramDescs.size(); ++i)
                {
                    auto info = choc::value::createObject("paraminfo");
                    info.setMember("name", m_paramDescs[i].name);
                    info.setMember("id", (int64_t)m_paramDescs[i].id);
                    info.setMember("minval", m_paramDescs[i].minVal);
                    info.setMember("maxval", m_paramDescs[i].maxVal);
                    info.setMember("defaultval", m_paramDescs[i].defaultVal);
                    info.setMember("val", pargetfunc(m_paramDescs[i].id));
                    if (m_paramDescs[i].type == ParamDesc::INT)
                        info.setMember("step", 1.0);
                    else
                        info.setMember("step", 0.01);
                    result.setMember(std::to_string(m_paramDescs[i].id), info);
                }
                return result;
            });

        m_webview->navigate(R"(C:\develop\xen_clap_plugins\source\noiseplethora2.html)");
    }
    std::unique_ptr<choc::ui::WebView> m_webview;

  private:
};

struct xen_noise_plethora
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Maximal>
{
    std::vector<ParamDesc> paramDescriptions;
    std::unique_ptr<NoisePlethoraGUI> m_gui;
    CommFIFO m_from_ui_fifo;
    CommFIFO m_to_ui_fifo;
    float sampleRate = 44100.0f;
    NoisePlethoraSynth m_synth;

    xen_noise_plethora(const clap_host *host, const clap_plugin_descriptor *desc)
        : clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                clap::helpers::CheckingLevel::Maximal>(desc, host)

    {
        host->request_callback(host);
        m_from_ui_fifo.reset(1024);
        m_to_ui_fifo.reset(1024);
        paramDescriptions.push_back(ParamDesc()
                                        .withLinearScaleFormatting("dB")
                                        .withRange(-24.0, 0.0)
                                        .withDefault(-6.0)
                                        .withFlags(CLAP_PARAM_IS_AUTOMATABLE |
                                                   CLAP_PARAM_IS_MODULATABLE |
                                                   CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID)
                                        .withName("Volume")
                                        .withID((clap_id)NoisePlethoraSynth::ParamIDs::Volume));
        paramDescriptions.push_back(ParamDesc()
                                        .asPercentBipolar()
                                        .withFlags(CLAP_PARAM_IS_AUTOMATABLE |
                                                   CLAP_PARAM_IS_MODULATABLE |
                                                   CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID)
                                        .withName("Pan")
                                        .withID((clap_id)NoisePlethoraSynth::ParamIDs::Pan));
        paramDescriptions.push_back(ParamDesc()
                                        .withLinearScaleFormatting("")
                                        .withRange(0.0, 1.0)
                                        .withDefault(0.5)
                                        .withFlags(CLAP_PARAM_IS_AUTOMATABLE |
                                                   CLAP_PARAM_IS_MODULATABLE |
                                                   CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID)
                                        .withName("X")
                                        .withID((clap_id)NoisePlethoraSynth::ParamIDs::X));
        paramDescriptions.push_back(ParamDesc()
                                        .withLinearScaleFormatting("")
                                        .withRange(0.0, 1.0)
                                        .withDefault(0.5)
                                        .withFlags(CLAP_PARAM_IS_AUTOMATABLE |
                                                   CLAP_PARAM_IS_MODULATABLE |
                                                   CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID)
                                        .withName("Y")
                                        .withID((clap_id)NoisePlethoraSynth::ParamIDs::Y));
        std::unordered_map<int, std::string> algonamemap;
        int k = 0;
        for (int i = 0; i < numBanks; ++i)
        {
            auto &bank = getBankForIndex(i);
            for (int j = 0; j < programsPerBank; ++j)
            {
                algonamemap[k] = bank.getProgramName(j);
                ++k;
            }
        }
        paramDescriptions.push_back(
            ParamDesc()
                .withUnorderedMapFormatting(algonamemap, true)
                .withDefault(0.0)
                .withFlags(CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE |
                           CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_STEPPED)
                .withName("Algorithm")
                .withID((clap_id)NoisePlethoraSynth::ParamIDs::Algo));
        paramDescriptions.push_back(ParamDesc()
                                        .withLinearScaleFormatting("")
                                        .withRange(0.0, 127.0)
                                        .withDefault(127)
                                        .withFlags(CLAP_PARAM_IS_AUTOMATABLE |
                                                   CLAP_PARAM_IS_MODULATABLE |
                                                   CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID)
                                        .withName("Filter cut off")
                                        .withID((clap_id)NoisePlethoraSynth::ParamIDs::FiltCutoff));
        paramDescriptions.push_back(
            ParamDesc()
                .withLinearScaleFormatting("")
                .withRange(0.01, 0.99)
                .withDefault(0.01)
                .withFlags(CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE |
                           CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID)
                .withName("Filter resonance")
                .withID((clap_id)NoisePlethoraSynth::ParamIDs::FiltResonance));
        paramDescriptions.push_back(
            ParamDesc()
                .withUnorderedMapFormatting({{0, "Lowpass"},
                                             {1, "Highpass"},
                                             {2, "Bandpass"},
                                             {3, "Peak"},
                                             {4, "Notch"},
                                             {5, "Allpass"}},
                                            true)
                .withDefault(0.0)

                .withFlags(CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE |
                           CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_STEPPED)
                .withName("Filter type")
                .withID((clap_id)NoisePlethoraSynth::ParamIDs::FiltType));
        paramDescriptions.push_back(ParamDesc()
                                        .withLinearScaleFormatting("%", 100.0)
                                        .withRange(0.0, 1.0)
                                        .withDefault(0.1f)
                                        .withFlags(CLAP_PARAM_IS_AUTOMATABLE)
                                        .withName("EG Attack")
                                        .withID((clap_id)NoisePlethoraSynth::ParamIDs::EGAttack));
        paramDescriptions.push_back(ParamDesc()
                                        .withLinearScaleFormatting("%", 100.0)
                                        .withRange(0.0, 1.0)
                                        .withDefault(0.1f)
                                        .withFlags(CLAP_PARAM_IS_AUTOMATABLE)
                                        .withName("EG Decay")
                                        .withID((clap_id)NoisePlethoraSynth::ParamIDs::EGDecay));
        paramDescriptions.push_back(ParamDesc()
                                        .withLinearScaleFormatting("%", 100.0)
                                        .withRange(0.0, 1.0)
                                        .withDefault(0.75f)
                                        .withFlags(CLAP_PARAM_IS_AUTOMATABLE)
                                        .withName("EG Sustain")
                                        .withID((clap_id)NoisePlethoraSynth::ParamIDs::EGSustain));
        paramDescriptions.push_back(ParamDesc()
                                        .withLinearScaleFormatting("%", 100.0)
                                        .withRange(0.0, 1.0)
                                        .withDefault(0.1f)
                                        .withFlags(CLAP_PARAM_IS_AUTOMATABLE)
                                        .withName("EG Release")
                                        .withID((clap_id)NoisePlethoraSynth::ParamIDs::EGRelease));
        paramDescriptions.push_back(
            ParamDesc()
                .withUnorderedMapFormatting({{0, "No tracking"},
                                             {1, "Lissajous 1"},
                                             {2, "Lissajous 2"},
                                             {3, "Grid 3x3"},
                                             {4, "Grid 5x5"},
                                             {5, "Grid 7x7"}},
                                            true)
                .withDefault(0.0)

                .withFlags(CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE |
                           CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_STEPPED)
                .withName("Key tracking mode")
                .withID((clap_id)NoisePlethoraSynth::ParamIDs::KeyTrackMode));
        paramDescriptions.push_back(
            ParamDesc()
                .asInt()
                .withRange(1.0, 16.0)
                .withDefault(0.1f)
                .withLinearScaleFormatting("")
                .withFlags(CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED)
                .withName("Filter count")
                .withID((clap_id)NoisePlethoraSynth::ParamIDs::FilterCount));

        paramDescriptions.push_back(
            ParamDesc()
                .withUnorderedMapFormatting({{0, "Poly"}, {1, "Mono"}}, true)
                .withDefault(0.0)
                .withFlags(CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED)
                .withName("Polyphony mode")
                .withID((clap_id)NoisePlethoraSynth::ParamIDs::PolyphonyMode));

        paramValues[0] = m_synth.m_voices[0]->basevalues.volume;
        paramValues[1] = m_synth.m_voices[0]->basevalues.x;
        paramValues[2] = m_synth.m_voices[0]->basevalues.y;
        paramValues[3] = m_synth.m_voices[0]->basevalues.filtcutoff;
        paramValues[4] = m_synth.m_voices[0]->basevalues.filtreson;
        paramValues[5] = m_synth.m_voices[0]->basevalues.filttype;
        paramValues[6] = m_synth.m_voices[0]->basevalues.algo;
        paramValues[7] = m_synth.m_voices[0]->basevalues.pan;
        paramValues[8] = m_synth.m_voices[0]->eg_attack;
        paramValues[9] = m_synth.m_voices[0]->eg_decay;
        paramValues[10] = m_synth.m_voices[0]->eg_sustain;
        paramValues[11] = m_synth.m_voices[0]->eg_release;
        paramValues[12] = m_synth.m_voices[0]->keytrackMode;
        paramValues[13] = m_synth.m_voices[0]->filterCount;
        paramValues[14] = m_synth.monoMode;
        auto host_par_ext = (clap_host_params *)host->get_extension(host, CLAP_EXT_PARAMS);
        host_par_ext->rescan(host, CLAP_PARAM_RESCAN_ALL);
    }
    void onMainThread() noexcept override { std::cout << "plug : on main thread\n"; }
    bool activate(double sampleRate_, uint32_t minFrameCount,
                  uint32_t maxFrameCount) noexcept override
    {
        m_synth.prepare(sampleRate_, maxFrameCount);
        return true;
    }

  protected:
    bool implementsParams() const noexcept override { return true; }
    bool isValidParamId(clap_id paramId) const noexcept override
    {
        for (auto &e : paramDescriptions)
            if (e.id == paramId)
                return true;
        return false;
    }
    uint32_t paramsCount() const noexcept override { return paramDescriptions.size(); }
    bool paramsInfo(uint32_t paramIndex, clap_param_info *info) const noexcept override
    {
        if (paramIndex >= paramDescriptions.size())
            return false;
        paramDescriptions[paramIndex].toClapParamInfo<CLAP_NAME_SIZE>(info);
        return true;
    }
    static constexpr size_t numParams = 15;
    std::array<float, numParams> paramValues;
    bool paramsValue(clap_id paramId, double *value) noexcept override
    {
        if (value && paramId >= 0 && paramId < numParams)
        {
            *value = paramValues[paramId];
            return true;
        }
        return false;
    }
    bool paramsValueToText(clap_id paramId, double value, char *display,
                           uint32_t size) noexcept override
    {
        for (auto &e : paramDescriptions)
        {
            if (e.id == paramId)
            {
                auto s = e.valueToString(value);
                if (s)
                {
                    strcpy_s(display, size, s->c_str());
                    return true;
                }
            }
        }
        return false;
    }
    bool implementsNotePorts() const noexcept override { return true; }
    uint32_t notePortsCount(bool isInput) const noexcept override
    {
        if (isInput)
            return 1;
        return 0;
    }
    bool notePortsInfo(uint32_t index, bool isInput,
                       clap_note_port_info *info) const noexcept override
    {
        if (isInput)
        {
            info->id = 5012;
            strcpy_s(info->name, "Note input");
            info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
            info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        }
        return false;
    }
    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override
    {
        if (!isInput)
            return 1;
        return 0;
    }
    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info *info) const noexcept override
    {
        // port id can be a "random" number
        info->id = isInput ? 2112 : 90210;
        strcpy_s(info->name, "main");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        return true;
    }
    void handleNextEvent(const clap_event_header_t *nextEvent, bool is_from_ui)
    {
        if (nextEvent->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return;

        switch (nextEvent->type)
        {
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE:
        {
            auto nevt = reinterpret_cast<const clap_event_note *>(nextEvent);
            m_synth.stopNote(nevt->port_index, nevt->channel, nevt->key, nevt->note_id,
                             nevt->velocity);
            break;
        }
        case CLAP_EVENT_NOTE_ON:
        {
            auto nevt = reinterpret_cast<const clap_event_note *>(nextEvent);
            m_synth.startNote(nevt->port_index, nevt->channel, nevt->key, nevt->note_id,
                              nevt->velocity);
            break;
        }
        case CLAP_EVENT_NOTE_EXPRESSION:
        {
            auto nexp = reinterpret_cast<const clap_event_note_expression *>(nextEvent);
            m_synth.applyNoteExpression(nexp->port_index, nexp->channel, nexp->key, nexp->note_id,
                                        nexp->expression_id, nexp->value);
            break;
        }
        case CLAP_EVENT_PARAM_VALUE:
        {
            auto pevt = reinterpret_cast<const clap_event_param_value *>(nextEvent);
            if (pevt->param_id >= 0 && pevt->param_id < numParams)
            {
                paramValues[pevt->param_id] = pevt->value;
                m_synth.applyParameter(pevt->port_index, pevt->channel, pevt->key, pevt->note_id,
                                       pevt->param_id, pevt->value);
                if (m_gui && !is_from_ui)
                {
                    UiMessage msg;
                    msg.type = CLAP_EVENT_PARAM_VALUE;
                    msg.parid = pevt->param_id;
                    msg.values[0] = pevt->value;
                    m_to_ui_fifo.push(msg);
                }
            }

            break;
        }
        case CLAP_EVENT_PARAM_MOD:
        {
            auto pevt = reinterpret_cast<const clap_event_param_mod *>(nextEvent);
            m_synth.applyParameterModulation(pevt->port_index, pevt->channel, pevt->key,
                                             pevt->note_id, pevt->param_id, pevt->amount);
            break;
        }
        default:
            break;
        }
    }
    void paramsFlush(const clap_input_events *in, const clap_output_events *out) noexcept override
    {
        handleUIMessages(out);
        auto sz = in->size(in);

        // This pointer is the sentinel to our next event which we advance once an event is
        // processed
        for (auto e = 0U; e < sz; ++e)
        {
            auto nextEvent = in->get(in, e);
            handleNextEvent(nextEvent, false);
        }
    }
    void handleUIMessages(const clap_output_events *oevts)
    {
        UiMessage msg;
        while (m_from_ui_fifo.pop(msg))
        {
            if (msg.type == CLAP_EVENT_PARAM_VALUE)
            {
                clap_event_param_value evt;
                evt.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                evt.header.flags = 0;
                evt.header.size = sizeof(clap_event_param_value);
                evt.header.type = CLAP_EVENT_PARAM_VALUE;
                evt.header.time = 0;
                evt.channel = -1;
                evt.port_index = -1;
                evt.key = -1;
                evt.note_id = -1;
                evt.param_id = msg.parid;
                evt.value = msg.values[0];
                handleNextEvent((const clap_event_header *)&evt, true);
                oevts->try_push(oevts, (const clap_event_header *)&evt);
            }
        }
    }
    clap_process_status process(const clap_process *process) noexcept override
    {
        auto frameCount = process->frames_count;
        float *op[2];
        op[0] = &process->audio_outputs->data32[0][0];
        op[1] = &process->audio_outputs->data32[1][0];

        auto inEvents = process->in_events;
        auto inEventsSize = inEvents->size(inEvents);

        handleUIMessages(process->out_events);

        // just do a simple subchunking here without a ringbuffer etc
        const clap_event_header_t *nextEvent{nullptr};
        uint32_t nextEventIndex{0};
        if (inEventsSize != 0)
        {
            nextEvent = inEvents->get(inEvents, nextEventIndex);
        }
        uint32_t chunkSize = ENVBLOCKSIZE;
        uint32_t pos = 0;
        m_synth.deactivatedNotes.clear();
        while (pos < frameCount)
        {
            uint32_t adjChunkSize = std::min(chunkSize, frameCount - pos);
            while (nextEvent && nextEvent->time < pos + adjChunkSize)
            {
                auto iev = inEvents->get(inEvents, nextEventIndex);
                handleNextEvent(iev, false);
                nextEventIndex++;
                if (nextEventIndex >= inEventsSize)
                    nextEvent = nullptr;
                else
                    nextEvent = inEvents->get(inEvents, nextEventIndex);
            }
            choc::buffer::SeparateChannelLayout<float> layout(process->audio_outputs->data32, pos);
            choc::buffer::ChannelArrayView<float> bufview(layout, {2, adjChunkSize});
            bufview.clear();
            m_synth.processBlock(bufview);
            pos += adjChunkSize;
        }

        if (m_gui)
        {
            for (size_t i = 0; i < m_synth.m_voices.size(); ++i)
            {
                auto &v = m_synth.m_voices[i];
                if (v->m_voice_active)
                {
                    UiMessage msg;
                    msg.type = 10000 + i;
                    msg.values[0] = v->totalx;
                    msg.values[1] = v->totaly;
                    msg.values[2] = v->total_gain;
                    m_to_ui_fifo.push(msg);
                    v->visualizationDirty = false;
                }
            }
        }
        visThrottleCounter += process->frames_count;
        if (visThrottleCounter >= 44100)
            visThrottleCounter = 0;
        for (const auto &denote : m_synth.deactivatedNotes)
        {
            clap_event_note nevt;
            nevt.header.flags = 0;
            nevt.header.size = sizeof(clap_event_note);
            nevt.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            nevt.header.time = 0;
            nevt.header.type = CLAP_EVENT_NOTE_END;
            nevt.port_index = std::get<0>(denote);
            nevt.channel = std::get<1>(denote);
            nevt.key = std::get<2>(denote);
            nevt.note_id = std::get<3>(denote);
            nevt.velocity = 0.0;
            process->out_events->try_push(process->out_events, (const clap_event_header *)&nevt);
        }
        return CLAP_PROCESS_CONTINUE;
    }
    int visThrottleCounter = 0;
    bool implementsState() const noexcept override { return true; }
    bool stateSave(const clap_ostream *stream) noexcept override
    {
        choc::value::Value v = choc::value::createObject("");
        v.addMember("version", 0);
        auto fvalues = choc::value::createEmptyArray();
        for (int i = 0; i < numParams; ++i)
        {
            fvalues.addArrayElement(paramValues[i]);
        }
        v.addMember("floatvalues", fvalues);
        auto json = choc::json::toString(v, true);

        if (json.size() > 0)
        {
            // DBG("volume serialized json is\n" << json);
            if (stream->write(stream, json.data(), json.size()) != -1)
                return true;
        }
        return false;
    }
    bool stateLoad(const clap_istream *stream) noexcept override
    {
        std::string json;
        constexpr size_t bufsize = 4096;
        json.reserve(bufsize);
        unsigned char buf[bufsize];
        memset(buf, 0, bufsize);
        while (true)
        {
            auto read = stream->read(stream, buf, bufsize);
            if (read == 0)
                break;
            if (read < 0)
                return false;
            for (size_t i = 0; i < read; ++i)
                json.push_back(buf[i]);
        }
        // DBG("volume deserialized json is\n" << json);
        if (json.size() > 0)
        {
            auto val = choc::json::parseValue(json);
            if (val.isObject() && val.hasObjectMember("version") && val["version"].get<int>() >= 0)
            {
                // these are not thread safe!
                // but interestingly, the SST Conduit plugin seems to be doing things
                // similarly, where they just update the plugin parameter states directly
                auto fvalues = val["floatvalues"];
                for (int i = 0; i < fvalues.size(); ++i)
                {
                    if (i < numParams)
                    {
                        paramValues[i] = fvalues[i].get<float>();
                        m_synth.applyParameter(-1, -1, -1, -1, i, paramValues[i]);
                    }
                }
            }
            return true;
        }
        return false;
    }
    bool implementsGui() const noexcept override { return true; }
    bool guiIsApiSupported(const char *api, bool isFloating) noexcept override
    {
        if (strcmp(api, "win32") == 0)
            return true;
        return false;
    }
    // virtual bool guiGetPreferredApi(const char **api, bool *is_floating) noexcept { return false;
    // }
    bool guiCreate(const char *api, bool isFloating) noexcept override
    {
        m_gui =
            std::make_unique<NoisePlethoraGUI>(paramDescriptions, m_from_ui_fifo, m_to_ui_fifo,
                                               [this](int parid) { return paramValues[parid]; });
        return true;
    }
    void guiDestroy() noexcept override { m_gui = nullptr; }
    // virtual bool guiSetScale(double scale) noexcept { return false; }
    bool guiShow() noexcept override
    {
        if (!m_gui)
            return false;
        return true;
    }
    bool guiHide() noexcept override
    {
        if (!m_gui)
            return false;
        return true;
    }
    int guiw = 700;
    int guih = 580;
    bool guiGetSize(uint32_t *width, uint32_t *height) noexcept override
    {
        if (!m_gui)
            return false;
        *width = guiw;
        *height = guih;
        return true;
    }
    // virtual bool guiCanResize() const noexcept { return false; }
    // virtual bool guiGetResizeHints(clap_gui_resize_hints_t *hints) noexcept { return false; }
    bool guiAdjustSize(uint32_t *width, uint32_t *height) noexcept override
    {
        return guiGetSize(width, height);
    }
    // virtual bool guiSetSize(uint32_t width, uint32_t height) noexcept { return false; }
    // virtual void guiSuggestTitle(const char *title) noexcept {}
    bool guiSetParent(const clap_window *window) noexcept override
    {
        if (!m_gui)
            return false;
        SetParent((HWND)m_gui->m_webview->getViewHandle(), (HWND)window->win32);
        ShowWindow((HWND)m_gui->m_webview->getViewHandle(), SW_SHOWNA);
        SetWindowPos((HWND)m_gui->m_webview->getViewHandle(), NULL, 0, 0, guiw, guih,
                     SWP_SHOWWINDOW);

        return true;
    }
    // virtual bool guiSetTransient(const clap_window *window) noexcept { return false; }
};

const char *features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, nullptr};
clap_plugin_descriptor desc = {CLAP_VERSION,
                               "com.xenakios.noiseplethora",
                               "xenakios noiseplethora",
                               "Unsupported",
                               "https://xenakios.com",
                               "",
                               "",
                               "0.0.0",
                               "xenakios noise plethora",
                               features};

static const clap_plugin *clap_create_plugin(const clap_plugin_factory *f, const clap_host *host,
                                             const char *plugin_id)
{
    // I know it looks like a leak right? but the clap-plugin-helpers basically
    // take ownership and destroy the wrapper when the host destroys the
    // underlying plugin (look at Plugin<h, l>::clapDestroy if you don't believe me!)
    auto wr = new xen_noise_plethora(host, &desc);
    return wr->clapPlugin();
}

uint32_t get_plugin_count(const struct clap_plugin_factory *factory) { return 1; }

const clap_plugin_descriptor *get_plugin_descriptor(const clap_plugin_factory *f, uint32_t w)
{
    return &desc;
}

const CLAP_EXPORT struct clap_plugin_factory the_factory = {
    get_plugin_count,
    get_plugin_descriptor,
    clap_create_plugin,
};

static const void *get_factory(const char *factory_id) { return &the_factory; }

// clap_init and clap_deinit are required to be fast, but we have nothing we need to do here
bool clap_init(const char *p) { return true; }

void clap_deinit() {}

extern "C"
{

    // clang-format off
const CLAP_EXPORT struct clap_plugin_entry clap_entry = {
        CLAP_VERSION,
        clap_init,
        clap_deinit,
        get_factory
};
    // clang-format on
}
