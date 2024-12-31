#include "clap/clap.h"
#include "clap/helpers/plugin.hh"
#include "clap/helpers/plugin.hxx"
#include "clap/helpers/host-proxy.hh"
#include "clap/helpers/host-proxy.hxx"
#include "clap/plugin-features.h"
#include "containers/choc_Value.h"
#include "gui/choc_MessageLoop.h"
#include "sst/basic-blocks/params/ParamMetadata.h"
#include "containers/choc_SingleReaderSingleWriterFIFO.h"
#include "audio/choc_SampleBuffers.h"
#include <chrono>
#include <cstdint>

#include <mutex>
#include <thread>
#include <unordered_map>
#include "../../source/xap_utils.h"
#include "../../source/xapdsp.h"
#include "libMTSClient.h"
#include "text/choc_JSON.h"
#include "threading/choc_SpinLock.h"
#include <windows.h>
#include "gui/choc_WebView.h"

using ParamDesc = sst::basic_blocks::params::ParamMetaData;

class GritNoiseGUI
{
  public:
    GritNoiseGUI()

    {
        choc::ui::WebView::Options opts;
        opts.enableDebugMode = true;
        m_webview = std::make_unique<choc::ui::WebView>(opts);

        m_webview->navigate(R"(C:\develop\xen_clap_plugins\python\clapgen\gui.html)");
    }
    std::unique_ptr<choc::ui::WebView> m_webview;

  private:
};

ParamDesc make_simple_parameter_desc(std::string name, clap_id id, double minvalue, double maxvalue,
                                     double defaultvalue, std::string units)
{
    return ParamDesc()
        .withLinearScaleFormatting(units)
        .withRange(minvalue, maxvalue)
        .withDefault(defaultvalue)
        .withFlags(CLAP_PARAM_IS_AUTOMATABLE)
        .withName(name)
        .withID(id);
}

struct UiMessage
{
    UiMessage() {}
    UiMessage(int type_, clap_id parid_, float val) : type{type_}, parid{parid_}, values{val} {}
    int type = 0;
    clap_id parid = CLAP_INVALID_ID;
    float values[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
};

using CommFIFO = choc::fifo::SingleReaderSingleWriterFIFO<UiMessage>;

class GritNoiseEngine
{
  public:
    GritNoiseEngine() {}
    void setSampleRate(float s) { m_sr = s; }
    void setDecay(float d) { m_decay_rate = xenakios::mapvalue(d, 0.0f, 1.0f, 0.4f, 0.95f); }
    void setGrit(float g)
    {
        g = std::clamp(g, 0.0f, 1.0f);
        m_grit = std::pow(g, 3.0f);
    }
    float singleSamplePulses()
    {
        float result = 0.0f;
        float z = m_rng.nextFloatInRange(0.0f, 1.0f);
        if (z < m_grit)
        {
            result = m_rng.nextFloatInRange(-1.0f, 1.0f);
        }
        return result;
    }
    float getNext()
    {
        float result = m_cur_value;
        float z = m_rng.nextFloatInRange(0.0f, 1.0f);
        if (z < m_grit)
        {
            result = m_rng.nextFloatInRange(-1.0f, 1.0f);
            m_cur_value = result;
        }
        m_cur_value *= m_decay_rate;
        return result;
    }

  private:
    xenakios::Xoroshiro128Plus m_rng;
    float m_grit = 1.0f;
    float m_sr = 44100.0f;
    float m_cur_value = 0.0f;
    float m_decay_rate = 0.75f;
};

struct GritNoise : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                                clap::helpers::CheckingLevel::Maximal>
{
    std::vector<ParamDesc> paramDescriptions;
    std::vector<float> paramValues;
    std::vector<float> paramMods;
    std::unordered_map<clap_id, float *> idToParamPointerMap;
    std::unordered_map<clap_id, float *> idToParamModPointerMap;
    double sampleRate = 0.0f;
    GritNoiseEngine gritEngine;
    std::array<StereoSimperSVF, 2> filters;
    int padding[16] = {};
    MTSClient *mts = nullptr;
    enum class ParId
    {
        GritDensity = 10,
        GritDecay = 1010,
        Filt1CutOff = 2010,
        Filt1Reson = 3010,
        Filt2CutOff = 4010,
        Filt2Reson = 5010,
        MTSEnabled = 50
    };
    GritNoise(const clap_host *host, const clap_plugin_descriptor *desc)
        : clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                clap::helpers::CheckingLevel::Maximal>(desc, host)

    {
        paramDescriptions.push_back(make_simple_parameter_desc(
            "Grit density", (clap_id)ParId::GritDensity, 0.0, 1.0, 0.8, "%"));
        paramDescriptions.push_back(make_simple_parameter_desc(
            "Pulse decay", (clap_id)ParId::GritDecay, 0.0, 1.0, 0.5, "%"));
        paramDescriptions.push_back(make_simple_parameter_desc(
            "Filter 1 cutoff", (clap_id)ParId::Filt1CutOff, 24.0, 120.0, 60.0, "semitones"));
        paramDescriptions.push_back(make_simple_parameter_desc(
            "Filter 1 resonance", (clap_id)ParId::Filt1Reson, 0.0, 1.0, 0.0, "%"));
        paramDescriptions.push_back(make_simple_parameter_desc(
            "Filter 2 cutoff", (clap_id)ParId::Filt2CutOff, 24.0, 120.0, 60.0, "semitones"));
        paramDescriptions.push_back(make_simple_parameter_desc(
            "Filter 2 resonance", (clap_id)ParId::Filt2Reson, 0.0, 1.0, 0.0, "%"));
        paramDescriptions.push_back(make_simple_parameter_desc(
            "Use MTS-ESP", (clap_id)ParId::MTSEnabled, 0.0, 1.0, 1.0, ""));
        paramValues.resize(paramDescriptions.size());
        paramMods.resize(paramDescriptions.size());
        for (size_t i = 0; i < paramDescriptions.size(); ++i)
        {
            auto &pd = paramDescriptions[i];
            paramValues[i] = pd.defaultVal;
            paramMods[i] = 0.0f;
            idToParamPointerMap[pd.id] = &paramValues[i];

            idToParamModPointerMap[pd.id] = &paramMods[i];
        }
    }

    ~GritNoise()
    {
        if (mts)
            MTS_DeregisterClient(mts);
    }
    bool activate(double sampleRate_, uint32_t minFrameCount,
                  uint32_t maxFrameCount) noexcept override
    {
        if (!mts)
        {
            mts = MTS_RegisterClient();
        }

        sampleRate = sampleRate_;
        gritEngine.setSampleRate(sampleRate_);
        for (auto &f : filters)
            f.init();
        return true;
    }
    void deactivate() noexcept override
    {
        if (mts)
        {
            MTS_DeregisterClient(mts);
            mts = nullptr;
        }
    }
    clap_process_status process(const clap_process *process) noexcept override
    {
        std::lock_guard<choc::threading::SpinLock> lockGuard(spinLock);
        bool mtsAvailable = mts && MTS_HasMaster(mts);
        auto frameCount = process->frames_count;
        if (process->audio_inputs_count > 0 && process->audio_inputs)
        {
            float *ip[2];
            ip[0] = &process->audio_inputs->data32[0][0];
            ip[1] = &process->audio_inputs->data32[1][0];
            for (int i = 0; i < frameCount; ++i)
            {
                ip[0][i] = 0.0f;
                ip[1][i] = 0.0f;
            }
        }

        float *op[2];
        op[0] = &process->audio_outputs->data32[0][0];
        op[1] = &process->audio_outputs->data32[1][0];

        auto inEvents = process->in_events;
        auto inEventsSize = inEvents->size(inEvents);

        // just do a simple subchunking here without a ringbuffer etc
        const clap_event_header_t *nextEvent{nullptr};
        uint32_t nextEventIndex{0};
        if (inEventsSize != 0)
        {
            nextEvent = inEvents->get(inEvents, nextEventIndex);
        }
        uint32_t chunkSize = 32;
        uint32_t pos = 0;

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
            gritEngine.setGrit(*idToParamPointerMap[(clap_id)ParId::GritDensity]);
            gritEngine.setDecay(*idToParamPointerMap[(clap_id)ParId::GritDecay]);
            float cutOffLeft = *idToParamPointerMap[(clap_id)ParId::Filt1CutOff];
            float cutOffRight = *idToParamPointerMap[(clap_id)ParId::Filt2CutOff];

            bool usemts = *idToParamPointerMap[(clap_id)ParId::MTSEnabled] > 0.5f;

            if (usemts && mtsAvailable)
            {
                float tuning_adjustLeft = 0.0f;
                float tuning_adjustRight = 0.0f;
                tuning_adjustLeft = MTS_RetuningInSemitones(mts, (char)cutOffLeft, 0);
                cutOffLeft = (int)cutOffLeft + tuning_adjustLeft;
                cutOffLeft = std::clamp(cutOffLeft, 24.0f, 127.0f);
                tuning_adjustRight = MTS_RetuningInSemitones(mts, (char)cutOffRight, 0);
                cutOffRight = (int)cutOffRight + tuning_adjustRight;
                cutOffRight = std::clamp(cutOffRight, 24.0f, 127.0f);
            }
            float res = *idToParamPointerMap[(clap_id)ParId::Filt1Reson];
            filters[0].setCoeff(cutOffLeft, res, 1.0 / sampleRate);
            res = *idToParamPointerMap[(clap_id)ParId::Filt2Reson];
            filters[1].setCoeff(cutOffRight, res, 1.0 / sampleRate);

            for (int i = 0; i < adjChunkSize; ++i)
            {
                float outsample = gritEngine.getNext();
                float dummy = 0.0;
                float filteredLeft = outsample;
                float filteredRight = outsample;
                filters[0].step<StereoSimperSVF::LP>(filters[0], filteredLeft, dummy);
                filters[1].step<StereoSimperSVF::LP>(filters[1], filteredRight, dummy);
                op[0][pos + i] = filteredLeft * 0.5;
                op[1][pos + i] = filteredRight * 0.5;
            }
            pos += adjChunkSize;
        }
        return CLAP_PROCESS_CONTINUE;
    }
    bool implementsParams() const noexcept override { return true; }
    bool isValidParamId(clap_id paramId) const noexcept override
    {
        auto it = idToParamPointerMap.find(paramId);
        if (it != idToParamPointerMap.end())
        {
            return true;
        }
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
    bool paramsValue(clap_id paramId, double *value) noexcept override
    {
        if (!value)
            return false;
        auto it = idToParamPointerMap.find(paramId);
        if (it != idToParamPointerMap.end())
        {
            *value = *it->second;
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
    bool implementsNotePorts() const noexcept override { return false; }
    uint32_t notePortsCount(bool isInput) const noexcept override { return 0; }
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
        if (isInput)
            return 0;
        return 1;
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
            break;
        }
        case CLAP_EVENT_NOTE_ON:
        {
            auto nevt = reinterpret_cast<const clap_event_note *>(nextEvent);
            break;
        }
        case CLAP_EVENT_NOTE_EXPRESSION:
        {
            auto nexp = reinterpret_cast<const clap_event_note_expression *>(nextEvent);
            break;
        }
        case CLAP_EVENT_PARAM_VALUE:
        {
            auto pevt = reinterpret_cast<const clap_event_param_value *>(nextEvent);
            auto it = idToParamPointerMap.find(pevt->param_id);
            if (it != idToParamPointerMap.end())
            {
                *it->second = pevt->value;
            }
            break;
        }
        case CLAP_EVENT_PARAM_MOD:
        {
            auto pevt = reinterpret_cast<const clap_event_param_mod *>(nextEvent);

            break;
        }
        default:
            break;
        }
    }
    void paramsFlush(const clap_input_events *in, const clap_output_events *out) noexcept override
    {
        return;
    }

    bool implementsState() const noexcept override { return true; }
    bool stateSave(const clap_ostream *stream) noexcept override
    {
        auto stateOb = choc::value::createObject("state");
        stateOb.setMember("version", 1);
        auto paramsArr = choc::value::createEmptyArray();
        for (auto &pd : paramDescriptions)
        {
            int64_t id = pd.id;
            auto it = idToParamPointerMap.find(pd.id);
            if (it != idToParamPointerMap.end())
            {
                float val = *(it->second);
                auto paramOb = choc::value::createObject("param");
                paramOb.setMember("id", id);
                paramOb.setMember("val", val);
                paramsArr.addArrayElement(paramOb);
            }
        }
        stateOb.setMember("parameters", paramsArr);
        auto json = choc::json::toString(stateOb, true);
        if (json.size() > 0)
        {
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
            if (val.isObject() && val.hasObjectMember("version") && val["version"].get<int>() >= 1)
            {
                auto parArr = val["parameters"];
                for (int i = 0; i < parArr.size(); ++i)
                {
                    auto parOb = parArr[i];
                    int64_t id = parOb["id"].getInt64();
                    float val = parOb["val"].get<float>();
                    auto it = idToParamPointerMap.find(id);
                    if (it != idToParamPointerMap.end())
                    {
                        // the raw parameters are not atomic, so...
                        std::lock_guard<choc::threading::SpinLock> lockGuard(spinLock);
                        *(it->second) = val;
                    }
                }
            }
        }
        return true;
    }
    choc::threading::SpinLock spinLock;
    std::unique_ptr<GritNoiseGUI> m_gui;
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
        m_gui = std::make_unique<GritNoiseGUI>();
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
    int guiw = 500;
    int guih = 200;
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
};

const char *features[] = {CLAP_PLUGIN_FEATURE_GLITCH, nullptr};
clap_plugin_descriptor desc = {CLAP_VERSION,
                               "com.xenakios.gritnoise",
                               "GritNoise",
                               "Xenakios",
                               "",
                               "",
                               "",
                               "0.0.0",
                               "Xenakios GritNoise",
                               features};

static const clap_plugin *clap_create_plugin(const clap_plugin_factory *f, const clap_host *host,
                                             const char *plugin_id)
{
    // I know it looks like a leak right? but the clap-plugin-helpers basically
    // take ownership and destroy the wrapper when the host destroys the
    // underlying plugin (look at Plugin<h, l>::clapDestroy if you don't believe me!)
    auto wr = new GritNoise(host, &desc);
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
