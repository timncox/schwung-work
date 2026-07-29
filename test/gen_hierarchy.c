/*
 * Emits the FULL static ui_hierarchy for module.json — every machine's real
 * knob labels, for both slots.
 *
 * Why this exists: the Shadow UI reads its hierarchy from module.json OR from
 * get_param("ui_hierarchy"). The engine serves a compact dynamic one that
 * follows the loaded machine, but which source the host prefers is not
 * something we can determine off-device — so module.json gets a complete
 * static hierarchy too, and neither path can show a bare "A".
 *
 * It is GENERATED from PARAM_NAME[][] rather than hand-written, so adding a
 * machine in C cannot leave module.json stale. Regenerate with `make module-json`.
 */
#include <stdio.h>
#include <string.h>

#include "work_core.h"

/* Machine ids as level-name fragments: "chrono_pitch", "comb_filter", ... */
static void slug(const char *name, char *out, size_t n) {
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < n; ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[j++] = c;
        else if (j && out[j - 1] != '_') out[j++] = '_';
    }
    while (j && out[j - 1] == '_') j--;
    out[j] = '\0';
}

int main(void) {
    printf("{\n  \"levels\": {\n");

    /* root */
    printf("    \"root\": {\n      \"name\": \"Work\",\n      \"params\": [\n");
    for (int s = 0; s < 2; ++s) {
        printf("        {\"key\": \"machine%d\", \"name\": \"FX %d Machine\", \"type\": \"enum\", \"options\": [", s + 1, s + 1);
        for (int i = 0; i < WORK_FX_COUNT; ++i)
            printf("%s\"%s\"", i ? ", " : "", work_machine_name(i));
        printf("], \"default\": 0},\n");
    }
    printf("        {\"key\": \"mix\", \"name\": \"Dry/Wet\", \"type\": \"int\", \"min\": 0, \"max\": 127, \"default\": 127},\n");
    printf("        {\"level\": \"fx1sel\", \"label\": \"FX 1 Parameters\"},\n");
    printf("        {\"level\": \"fx2sel\", \"label\": \"FX 2 Parameters\"},\n");
    printf("        {\"level\": \"lfo1\", \"label\": \"FX LFO 1\"},\n");
    printf("        {\"level\": \"lfo2\", \"label\": \"FX LFO 2\"},\n");
    printf("        {\"level\": \"lfo3\", \"label\": \"FX LFO 3\"},\n");
    printf("        {\"level\": \"menv\", \"label\": \"Modulation Envelope\"}\n");
    printf("      ],\n      \"knobs\": [\"machine1\", \"machine2\", \"mix\"]\n    },\n");

    /* one selector level per slot, listing every machine */
    for (int s = 0; s < 2; ++s) {
        printf("    \"fx%dsel\": {\n      \"name\": \"FX %d Parameters\",\n      \"params\": [\n", s + 1, s + 1);
        for (int mc = 0; mc < WORK_FX_COUNT; ++mc) {
            char sl[64];
            slug(work_machine_name(mc), sl, sizeof(sl));
            printf("        {\"level\": \"fx%d_%s\", \"label\": \"%s\"}%s\n",
                   s + 1, sl, work_machine_name(mc), mc == WORK_FX_COUNT - 1 ? "" : ",");
        }
        printf("      ]\n    },\n");
    }

    /* one level per machine per slot, with the real labels */
    for (int s = 0; s < 2; ++s) {
        for (int mc = 0; mc < WORK_FX_COUNT; ++mc) {
            char sl[64];
            slug(work_machine_name(mc), sl, sizeof(sl));
            printf("    \"fx%d_%s\": {\n      \"name\": \"%s\",\n      \"params\": [\n",
                   s + 1, sl, work_machine_name(mc));

            int any = 0;
            for (int i = 0; i < WORK_PARAMS; ++i) {
                const char *nm = work_param_name(mc, i);
                if (!nm[0]) continue;
                printf("%s        {\"key\": \"fx%d_p%d\", \"name\": \"%s\", \"type\": \"int\", \"min\": 0, \"max\": 127}",
                       any ? ",\n" : "", s + 1, i + 1, nm);
                any = 1;
            }
            if (!any)
                printf("        {\"key\": \"fx%d_p1\", \"name\": \"(no parameters)\", \"type\": \"int\", \"min\": 0, \"max\": 127}", s + 1);
            printf("\n      ],\n      \"knobs\": [");
            any = 0;
            for (int i = 0; i < WORK_PARAMS; ++i) {
                if (!work_param_name(mc, i)[0]) continue;
                printf("%s\"fx%d_p%d\"", any ? ", " : "", s + 1, i + 1);
                any = 1;
            }
            printf("]\n    },\n");
        }
    }

    /* the two FX LFOs */
    for (int l = 1; l <= WORK_LFOS; ++l) {
        printf("    \"lfo%d\": {\n      \"name\": \"FX LFO %d\",\n      \"params\": [\n", l, l);
        printf("        {\"key\": \"lfo%d_dest\", \"name\": \"Destination\", \"type\": \"int\", \"min\": -1, \"max\": 15, \"default\": -1},\n", l);
        printf("        {\"key\": \"lfo%d_spd\", \"name\": \"Speed\", \"type\": \"int\", \"min\": 0, \"max\": 127, \"default\": 32},\n", l);
        printf("        {\"key\": \"lfo%d_mult\", \"name\": \"Multiplier\", \"type\": \"int\", \"min\": 0, \"max\": 127, \"default\": 64},\n", l);
        printf("        {\"key\": \"lfo%d_wave\", \"name\": \"Waveform\", \"type\": \"enum\", \"options\": [\"Triangle\", \"Sine\", \"Square\", \"Saw\", \"Ramp\", \"Exponential\", \"Random\"], \"default\": 0},\n", l);
        printf("        {\"key\": \"lfo%d_depth\", \"name\": \"Depth\", \"type\": \"int\", \"min\": 0, \"max\": 127, \"default\": 64},\n", l);
        printf("        {\"key\": \"lfo%d_phase\", \"name\": \"Start Phase\", \"type\": \"int\", \"min\": 0, \"max\": 127, \"default\": 0},\n", l);
        printf("        {\"key\": \"lfo%d_trig\", \"name\": \"Trig Mode\", \"type\": \"enum\", \"options\": [\"Free\", \"Retrig\"], \"default\": 0}\n", l);
        printf("      ],\n      \"knobs\": [\"lfo%d_dest\", \"lfo%d_spd\", \"lfo%d_mult\", \"lfo%d_wave\", \"lfo%d_depth\", \"lfo%d_phase\", \"lfo%d_trig\"]\n    }%s\n",
               l, l, l, l, l, l, l, ",");
    }

    /* modulation envelope */
    printf("    \"menv\": {\n      \"name\": \"Modulation Envelope\",\n      \"params\": [\n");
    printf("        {\"key\": \"menv_dest\", \"name\": \"Destination\", \"type\": \"int\", \"min\": -1, \"max\": 15, \"default\": -1},\n");
    printf("        {\"key\": \"menv_atk\", \"name\": \"Attack\", \"type\": \"int\", \"min\": 0, \"max\": 127, \"default\": 0},\n");
    printf("        {\"key\": \"menv_hold\", \"name\": \"Hold\", \"type\": \"int\", \"min\": 0, \"max\": 127, \"default\": 8},\n");
    printf("        {\"key\": \"menv_dec\", \"name\": \"Decay\", \"type\": \"int\", \"min\": 0, \"max\": 127, \"default\": 48},\n");
    printf("        {\"key\": \"menv_depth\", \"name\": \"Depth\", \"type\": \"int\", \"min\": 0, \"max\": 127, \"default\": 64}\n");
    printf("      ],\n      \"knobs\": [\"menv_dest\", \"menv_atk\", \"menv_hold\", \"menv_dec\", \"menv_depth\"]\n    }\n");

    printf("  }\n}\n");
    return 0;
}
