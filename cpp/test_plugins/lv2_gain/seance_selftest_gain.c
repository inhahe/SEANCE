/*
    SEANCE self-test LV2 plugins: two mono gains in one bundle.

    Built by cpp/CMakeLists.txt into <exe dir>/selftest_plugins/ and used only
    by the self-tests (testPluginFolders, testPluginIdentity in self_test.cpp),
    which need real LV2 plugins living in a folder that is NOT a standard LV2
    location, and two of them to tell apart. release.bat does not package
    selftest_plugins/, so these never ship.

    Ports: 0 = gain (control in), 1 = audio in, 2 = audio out. The gain's
    default lives in seance_selftest_gain.ttl: 0.5 for "SEANCE Self-Test Gain",
    0.25 for "SEANCE Self-Test Gain B" - so the output level proves both that
    the plugin's own code ran and which of the two it was.
*/

#include <lv2/core/lv2.h>
#include <stdlib.h>

#define SELFTEST_GAIN_URI   "urn:seance:selftest:gain"
#define SELFTEST_GAIN_B_URI "urn:seance:selftest:gain-b"

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

static const LV2_Descriptor descriptors[] = {
    { SELFTEST_GAIN_URI,   instantiate, connect_port, NULL, run, NULL, cleanup, NULL },
    { SELFTEST_GAIN_B_URI, instantiate, connect_port, NULL, run, NULL, cleanup, NULL },
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index < sizeof descriptors / sizeof descriptors[0] ? &descriptors[index] : NULL;
}
