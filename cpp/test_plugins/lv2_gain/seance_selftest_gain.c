/*
    SEANCE self-test LV2 plugin: a mono gain.

    Built by cpp/CMakeLists.txt into <exe dir>/selftest_plugins/ and used only
    by testLv2PluginFolders() in self_test.cpp, which needs a real LV2 plugin
    living in a folder that is NOT a standard LV2 location - there is no other
    way to test that such plugins still load after a restart. release.bat does
    not package selftest_plugins/, so this never ships.

    Ports: 0 = gain (control in, default 0.5), 1 = audio in, 2 = audio out.
    The non-unity default lets the test prove the plugin's own code ran.
*/

#include <lv2/core/lv2.h>
#include <stdlib.h>

#define SELFTEST_GAIN_URI "urn:seance:selftest:gain"

typedef struct {
    const float* gain;
    const float* input;
    float* output;
} Gain;

static LV2_Handle instantiate(const LV2_Descriptor* descriptor, double rate,
                              const char* bundlePath, const LV2_Feature* const* features)
{
    (void) descriptor; (void) rate; (void) bundlePath; (void) features;
    return (LV2_Handle) calloc(1, sizeof(Gain));
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    Gain* g = (Gain*) instance;
    switch (port)
    {
        case 0: g->gain = (const float*) data; break;
        case 1: g->input = (const float*) data; break;
        case 2: g->output = (float*) data; break;
        default: break;
    }
}

static void run(LV2_Handle instance, uint32_t sampleCount)
{
    const Gain* g = (const Gain*) instance;
    const float gain = g->gain != NULL ? *g->gain : 1.0f;

    if (g->input == NULL || g->output == NULL)
        return;

    for (uint32_t i = 0; i < sampleCount; ++i)
        g->output[i] = g->input[i] * gain;
}

static void cleanup(LV2_Handle instance)
{
    free(instance);
}

static const LV2_Descriptor descriptor = {
    SELFTEST_GAIN_URI,
    instantiate,
    connect_port,
    NULL, /* activate */
    run,
    NULL, /* deactivate */
    cleanup,
    NULL  /* extension_data */
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
