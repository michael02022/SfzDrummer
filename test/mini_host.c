/* Mini host CLAP para sfzdrummer: carga el .clap, instancia el plugin,
 * procesa audio con 8 salidas stereo (sin input - es un instrumento),
 * inyecta un kit de prueba via clap.state (un par de percusiones reales, sin
 * necesitar drag-and-drop/click real - ver memoria del proyecto sobre por
 * que no se puede simular input en este sandbox) y comprueba que cada
 * percusion suena en el output= que se le asigno y en ningun otro.
 * Opcionalmente abre la GUI standalone para verificacion visual.
 * Uso: mini_host <ruta al .clap> [ruta a un .wav] [gui]
 * Adaptado del mini_host.c de SoloSampler (sibling project). */
#include <clap/clap.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NUM_OUTPUTS 8

static const void* host_get_extension(const struct clap_host* h, const char* id) {
    (void)h; (void)id;
    return NULL;
}
static void host_request_restart(const struct clap_host* h) { (void)h; }
static void host_request_process(const struct clap_host* h) { (void)h; }
static void host_request_callback(const struct clap_host* h) { (void)h; }

typedef struct {
    const clap_event_header_t* events[4];
    uint32_t count;
} EvCtx;

static uint32_t ev_size(const struct clap_input_events* l) {
    return ((EvCtx*)l->ctx)->count;
}
static const clap_event_header_t* ev_get(const struct clap_input_events* l, uint32_t i) {
    return ((EvCtx*)l->ctx)->events[i];
}
static bool ev_push(const struct clap_output_events* l, const clap_event_header_t* e) {
    (void)l; (void)e;
    return true;
}

/* stream de estado en memoria (mismo patron que SoloSampler/NeoLooper) */
typedef struct {
    uint8_t buf[1 << 20];
    uint64_t len, pos;
} MemStream;

static int64_t mem_write(const struct clap_ostream* s, const void* d, uint64_t n) {
    MemStream* m = (MemStream*)s->ctx;
    if (m->len + n > sizeof(m->buf)) n = sizeof(m->buf) - m->len;
    memcpy(m->buf + m->len, d, n);
    m->len += n;
    return (int64_t)n;
}
static int64_t mem_read(const struct clap_istream* s, void* d, uint64_t n) {
    MemStream* m = (MemStream*)s->ctx;
    uint64_t left = m->len - m->pos;
    if (n > left) n = left;
    memcpy(d, m->buf + m->pos, n);
    m->pos += n;
    return (int64_t)n;
}
static void write_string(MemStream* m, const char* s) {
    uint32_t len = s ? (uint32_t)strlen(s) : 0;
    clap_ostream_t os = {.ctx = m, .write = mem_write};
    mem_write(&os, &len, 4);
    if (len) mem_write(&os, s, len);
}

/* v3 Sample tab fields, ver plugin.cpp's stateSave/stateLoad: volume, pan,
 * width, quality, polyphony, notePolyphony, disableNoteSelfmask,
 * loopModeIndex, reverse, offset, vel2offset, exclusiveClass, group,
 * offbyEnabled, offby, transpose, tune. Escribe valores DISTINTOS de los
 * defaults de shared.hpp (no todo-cero) para que un bug de desplazamiento de
 * campos en el (de)serializador no pase desapercibido silenciosamente -
 * PERO exclusiveClass se deja en 0: Kick/Snare/HiHat se disparan juntos en
 * el mismo bloque en el test de abajo, y un group=/off_by= COMPARTIDO entre
 * los tres los silenciaria entre si (cada uno chocaria al anterior) - eso
 * seria el comportamiento SFZ correcto, no un bug, asi que no tiene sentido
 * ejercitarlo aqui. group/offby igual llevan un valor no-cero para probar
 * que el formato de estado los guarda/carga bien (solo que DrumSfzBuilder
 * no los escribe en el sfz mientras exclusiveClass sea falso). offset se
 * mantiene chico (rango 0..4096) para no saltarse la mayor parte del wav de
 * prueba (corto) y dejar el test de routing sin señal; vel2offset es
 * negativo (rango -4096..0 - resta del offset base segun la velocidad de la
 * nota, en vez de sumar, ya que este build de sfizz no soporta
 * offset_curvecc para invertir la curva - ver DrumSfzBuilder.cpp). */
static void write_sample_tab_v3(MemStream* m) {
    clap_ostream_t os = {.ctx = m, .write = mem_write};
    int32_t volume = -6, pan = 15, width = 80, quality = 6, polyphony = 64, notePolyphony = 64;
    uint8_t disableNoteSelfmask = 1;
    int32_t loopModeIndex = 1; /* one_shot */
    uint8_t reverse = 0;
    int32_t offset = 50, vel2offset = -100;
    uint8_t exclusiveClass = 0;
    int32_t group = 3;
    uint8_t offbyEnabled = 1;
    int32_t offby = 5, transpose = -5, tune = 12;
    mem_write(&os, &volume, 4);
    mem_write(&os, &pan, 4);
    mem_write(&os, &width, 4);
    mem_write(&os, &quality, 4);
    mem_write(&os, &polyphony, 4);
    mem_write(&os, &notePolyphony, 4);
    mem_write(&os, &disableNoteSelfmask, 1);
    mem_write(&os, &loopModeIndex, 4);
    mem_write(&os, &reverse, 1);
    mem_write(&os, &offset, 4);
    mem_write(&os, &vel2offset, 4);
    mem_write(&os, &exclusiveClass, 1);
    mem_write(&os, &group, 4);
    mem_write(&os, &offbyEnabled, 1);
    mem_write(&os, &offby, 4);
    mem_write(&os, &transpose, 4);
    mem_write(&os, &tune, 4);
}

/* v4 Amp tab: sparse amp_velcurve_<velocity>=<gain> points, ver plugin.cpp's
 * stateSave/stateLoad y shared.hpp's AmpVelCurvePoint. count=0 escribe solo
 * el conteo (sin puntos) - ejercita el caso vacio, que es el mas comun (una
 * percusion que nunca toco la pestaña Amp). */
static void write_amp_curve_v4(MemStream* m, const int32_t* velocities, const float* gains,
                               uint32_t count) {
    clap_ostream_t os = {.ctx = m, .write = mem_write};
    mem_write(&os, &count, 4);
    for (uint32_t i = 0; i < count; ++i) {
        mem_write(&os, &velocities[i], 4);
        mem_write(&os, &gains[i], 4);
    }
}

/* v5 Amp tab: amp_veltrack/amp_random/eg01_* envelope/vel2attack/
 * vel2sustain/vel2volume - ver plugin.cpp's stateSave/stateLoad y
 * shared.hpp's DrumItem fields. Todos los valores DISTINTOS de los defaults
 * (mismo motivo que write_sample_tab_v3: para que un bug de desplazamiento
 * de campos no pase desapercibido). ampAttackTime/ampHoldTime/ampDecayTime
 * son POSITIVOS (rango 0.00001..0.1/0.5 - un valor negativo rompe la
 * transicion de nivel del flexEG, ver shared.hpp) pero cortos (<=50ms), asi
 * que dentro de la ventana de 8 bloques del test de routing de mas abajo el
 * envelope ya llego (o esta por llegar) al nivel maximo - no introduce
 * silencio real. ampStartLevel YA NO EXISTE (v6 elimino el slider "Start
 * Level" - eg01_level0/1 quedaron fijos en 0 en DrumSfzBuilder.cpp). */
static void write_amp_envelope_v6(MemStream* m) {
    clap_ostream_t os = {.ctx = m, .write = mem_write};
    int32_t ampVeltrack = 80, ampRandom = -3, ampVel2Volume = -6;
    float ampAttackTime = 0.05f, ampHoldTime = 0.02f,
          ampDecayTime = 0.03f, ampSustainLevel = 0.75f, ampReleaseTime = 0.5f,
          ampAttackShape = 2.5f, ampDecayShape = -1.5f, ampReleaseShape = 3.0f,
          ampVel2Attack = -0.05f, ampVel2Sustain = 0.3f;
    mem_write(&os, &ampVeltrack, 4);
    mem_write(&os, &ampRandom, 4);
    mem_write(&os, &ampAttackTime, 4);
    mem_write(&os, &ampHoldTime, 4);
    mem_write(&os, &ampDecayTime, 4);
    mem_write(&os, &ampSustainLevel, 4);
    mem_write(&os, &ampReleaseTime, 4);
    mem_write(&os, &ampAttackShape, 4);
    mem_write(&os, &ampDecayShape, 4);
    mem_write(&os, &ampReleaseShape, 4);
    mem_write(&os, &ampVel2Attack, 4);
    mem_write(&os, &ampVel2Sustain, 4);
    mem_write(&os, &ampVel2Volume, 4);
}

/* v7 Sample tab: panRandom/panAlternate (Pan Random/Alternate, ported de la
 * pestaña Pan de SoloSampler sin LFO ni "x2" - ver plugin.cpp's
 * stateSave/stateLoad y shared.hpp). Valor no-default, mismo motivo de
 * siempre. */
static void write_pan_random_v7(MemStream* m) {
    clap_ostream_t os = {.ctx = m, .write = mem_write};
    int32_t panRandom = 60;
    uint8_t panAlternate = 1;
    mem_write(&os, &panRandom, 4);
    mem_write(&os, &panAlternate, 1);
}

/* v8 Fil tab: filterTypeIndex/filterCutoff/filterResonance/
 * filterRandomCutoff/filVeltrack/resoVeltrack/filterEgEnabled/filDepth/
 * eg02_* (Filter tab, ported de SoloSampler sin fil_keycenter=/
 * fil_keytrack=/LFO - ver plugin.cpp's stateSave/stateLoad y shared.hpp).
 * eg_enabled distingue dos casos: 0 (Kick/Snare) deja el bloque eg02_* en
 * sus defaults - DrumSfzBuilder no lo escribe de todas formas mientras
 * filterEgEnabled sea falso, pero el formato de estado siempre serializa
 * los 18 campos - y 1 (HiHat) ejercita esa ruta de codigo con valores
 * no-default, tiempos cortos (<=5ms, suman <10ms) para no silenciar el
 * test de routing de mas abajo. */
static void write_filter_v8(MemStream* m, int eg_enabled) {
    clap_ostream_t os = {.ctx = m, .write = mem_write};
    int32_t filterTypeIndex = 3, filterCutoff = 18000, filterRandomCutoff = 5000,
            filVeltrack = 2000, resoVeltrack = 5, filDepth = eg_enabled ? 3000 : 0;
    uint8_t filterEgEnabled = eg_enabled ? 1 : 0;
    float filterResonance = 3.0f;
    float filEgStartLevel, filEgDelayTime, filEgAttackTime, filEgAttackShape, filEgHoldTime,
          filEgDecayTime, filEgDecayShape, filEgSustainLevel, filEgReleaseTime, filEgReleaseShape;
    if (eg_enabled) {
        filEgStartLevel = 0.05f;
        filEgDelayTime = 0.001f;
        filEgAttackTime = 0.002f;
        filEgAttackShape = 1.5f;
        filEgHoldTime = 0.003f;
        filEgDecayTime = 0.004f;
        filEgDecayShape = -1.5f;
        filEgSustainLevel = 0.8f;
        filEgReleaseTime = 0.05f;
        filEgReleaseShape = 2.0f;
    } else {
        filEgStartLevel = 0.0f;
        filEgDelayTime = 0.00001f;
        filEgAttackTime = 0.00001f;
        filEgAttackShape = 0.00001f;
        filEgHoldTime = 0.00001f;
        filEgDecayTime = 0.00001f;
        filEgDecayShape = 0.00001f;
        filEgSustainLevel = 1.0f;
        filEgReleaseTime = 0.00001f;
        filEgReleaseShape = 0.00001f;
    }
    mem_write(&os, &filterTypeIndex, 4);
    mem_write(&os, &filterCutoff, 4);
    mem_write(&os, &filterResonance, 4);
    mem_write(&os, &filterRandomCutoff, 4);
    mem_write(&os, &filVeltrack, 4);
    mem_write(&os, &resoVeltrack, 4);
    mem_write(&os, &filterEgEnabled, 1);
    mem_write(&os, &filDepth, 4);
    mem_write(&os, &filEgStartLevel, 4);
    mem_write(&os, &filEgDelayTime, 4);
    mem_write(&os, &filEgAttackTime, 4);
    mem_write(&os, &filEgAttackShape, 4);
    mem_write(&os, &filEgHoldTime, 4);
    mem_write(&os, &filEgDecayTime, 4);
    mem_write(&os, &filEgDecayShape, 4);
    mem_write(&os, &filEgSustainLevel, 4);
    mem_write(&os, &filEgReleaseTime, 4);
    mem_write(&os, &filEgReleaseShape, 4);
}

/* v9 Pitch tab: pitchVeltrack/pitchRandom/pitchEgEnabled/pitchDepth/
 * pitchVel2Depth (vel2pitch, nuevo - no existe en SoloSampler)/eg03_*
 * (Pitch tab, ported de SoloSampler sin pitch_keytrack=/portamento/LFO -
 * ver plugin.cpp's stateSave/stateLoad y shared.hpp). Mismo patron que
 * write_filter_v8: eg_enabled=0 (Kick/Snare) deja el bloque eg03_* en sus
 * defaults, eg_enabled=1 (HiHat) ejercita esa ruta con valores no-default
 * y tiempos cortos que no silencian el test de routing de mas abajo. */
static void write_pitch_v9(MemStream* m, int eg_enabled) {
    clap_ostream_t os = {.ctx = m, .write = mem_write};
    int32_t pitchVeltrack = 1200, pitchRandom = 300,
            pitchDepth = eg_enabled ? 200 : 0, pitchVel2Depth = eg_enabled ? -100 : 0;
    uint8_t pitchEgEnabled = eg_enabled ? 1 : 0;
    float pitchEgStartLevel, pitchEgDelayTime, pitchEgAttackTime, pitchEgAttackShape,
          pitchEgHoldTime, pitchEgDecayTime, pitchEgDecayShape, pitchEgSustainLevel,
          pitchEgReleaseTime, pitchEgReleaseShape;
    if (eg_enabled) {
        pitchEgStartLevel = 0.05f;
        pitchEgDelayTime = 0.001f;
        pitchEgAttackTime = 0.002f;
        pitchEgAttackShape = 1.5f;
        pitchEgHoldTime = 0.003f;
        pitchEgDecayTime = 0.004f;
        pitchEgDecayShape = -1.5f;
        pitchEgSustainLevel = 0.8f;
        pitchEgReleaseTime = 0.05f;
        pitchEgReleaseShape = 2.0f;
    } else {
        pitchEgStartLevel = 0.0f;
        pitchEgDelayTime = 0.00001f;
        pitchEgAttackTime = 0.00001f;
        pitchEgAttackShape = 0.00001f;
        pitchEgHoldTime = 0.00001f;
        pitchEgDecayTime = 0.00001f;
        pitchEgDecayShape = 0.00001f;
        pitchEgSustainLevel = 1.0f;
        pitchEgReleaseTime = 0.00001f;
        pitchEgReleaseShape = 0.00001f;
    }
    mem_write(&os, &pitchVeltrack, 4);
    mem_write(&os, &pitchRandom, 4);
    mem_write(&os, &pitchEgEnabled, 1);
    mem_write(&os, &pitchDepth, 4);
    mem_write(&os, &pitchVel2Depth, 4);
    mem_write(&os, &pitchEgStartLevel, 4);
    mem_write(&os, &pitchEgDelayTime, 4);
    mem_write(&os, &pitchEgAttackTime, 4);
    mem_write(&os, &pitchEgAttackShape, 4);
    mem_write(&os, &pitchEgHoldTime, 4);
    mem_write(&os, &pitchEgDecayTime, 4);
    mem_write(&os, &pitchEgDecayShape, 4);
    mem_write(&os, &pitchEgSustainLevel, 4);
    mem_write(&os, &pitchEgReleaseTime, 4);
    mem_write(&os, &pitchEgReleaseShape, 4);
}

/* v10 Pitch tab: pitchVel2Invert ("Invert vel2pitch" checkbox, writes
 * eg03_pitch_curvecc131=2 when on - ver plugin.cpp's stateSave/stateLoad
 * y shared.hpp). */
static void write_pitch_invert_v10(MemStream* m, int invert) {
    clap_ostream_t os = {.ctx = m, .write = mem_write};
    uint8_t pitchVel2Invert = invert ? 1 : 0;
    mem_write(&os, &pitchVel2Invert, 1);
}

/* v11: ampDecayTimeExtra/filEgDecayTimeExtra/pitchEgDecayTimeExtra ("Decay
 * Time (Extra)" sliders, 0.0..12.0, summed onto the base Decay Time at
 * DrumSfzBuilder.cpp - ver plugin.cpp's stateSave/stateLoad y shared.hpp).
 * Un valor fijo no-default por campo, sin distincion Kick/Snare/HiHat (a
 * diferencia de write_filter_v8/write_pitch_v9): Amp EG siempre esta
 * activo asi que su eg01_time4= siempre se ejercita, y aunque
 * filEgDecayTimeExtra/pitchEgDecayTimeExtra solo importan cuando el EG
 * respectivo esta encendido (ya cubierto por eg_enabled en otros lados),
 * el formato de estado siempre serializa los 3 campos igual. */
static void write_decay_extra_v11(MemStream* m) {
    clap_ostream_t os = {.ctx = m, .write = mem_write};
    float ampDecayTimeExtra = 0.5f, filEgDecayTimeExtra = 0.3f, pitchEgDecayTimeExtra = 0.2f;
    mem_write(&os, &ampDecayTimeExtra, 4);
    mem_write(&os, &filEgDecayTimeExtra, 4);
    mem_write(&os, &pitchEgDecayTimeExtra, 4);
}

/* v12 Opcodes tab: customOpcodesText, texto libre escrito verbatim dentro
 * del <master> de ESTA percusion (ver plugin.cpp's stateSave/stateLoad y
 * shared.hpp/DrumSfzBuilder.cpp). Solo un comentario SFZ (no un opcode
 * real) para no alterar el audio del test de routing de mas abajo - lo
 * que se prueba aca es el round-trip del string en el drum correcto, no
 * un efecto DSP. Incluye el label del drum en el texto para poder notar
 * si alguna vez terminara en el <master> equivocado. */
static void write_custom_opcodes_v12(MemStream* m, const char* label) {
    char text[128];
    snprintf(text, sizeof(text), "// custom opcode test for %s", label);
    write_string(m, text);
}

/* Escribe el mismo formato que plugin.cpp's stateSave/stateLoad (ver
 * kStateMagic/kStateVersion=14): magic, version, count, luego por
 * percusion: rootNote, outputIndex, hasSource, isSfz, label, sourcePath,
 * sampleRelativePath, regionsText, regionCount, drumKitModeEnabled,
 * drumKitGroupIndex, drumKitGroupCount (0 aqui - Drum Kit Mode no se
 * ejercita en este test), los campos v3 del Sample tab, el ampVelCurve (v4,
 * vacio aqui), el resto de la pestaña Amp (v5/v6: veltrack/random/
 * envelope/vel2*, sin ampStartLevel), panRandom/panAlternate (v7), la
 * pestaña Fil completa (v8), la pestaña Pitch completa (v9),
 * pitchVel2Invert (v10), los 3 sliders Decay Time (Extra) (v11),
 * customOpcodesText (v12) y, despues de TODAS las percusiones (no es
 * per-drum), mpeEnabled (v13) y bendUpCents/bendDownCents (v14). */
static void write_drum_no_kit(MemStream* m, int32_t rn, int32_t oi, uint8_t isSfz,
                              const char* label, const char* samplePath, const char* rel) {
    clap_ostream_t os = {.ctx = m, .write = mem_write};
    int32_t rc = 1;
    uint8_t hasSource = 1;
    uint8_t drumKitModeEnabled = 0;
    int32_t drumKitGroupIndex = 0;
    uint32_t drumKitGroupCount = 0;
    mem_write(&os, &rn, 4);
    mem_write(&os, &oi, 4);
    mem_write(&os, &hasSource, 1);
    mem_write(&os, &isSfz, 1);
    write_string(m, label);
    write_string(m, samplePath);
    write_string(m, rel);
    write_string(m, "");
    mem_write(&os, &rc, 4);
    mem_write(&os, &drumKitModeEnabled, 1);
    mem_write(&os, &drumKitGroupIndex, 4);
    mem_write(&os, &drumKitGroupCount, 4);
    write_sample_tab_v3(m);
    write_amp_curve_v4(m, NULL, NULL, 0);
    write_amp_envelope_v6(m);
    write_pan_random_v7(m);
    write_filter_v8(m, 0);
    write_pitch_v9(m, 0);
    write_pitch_invert_v10(m, 0);
    write_decay_extra_v11(m);
    write_custom_opcodes_v12(m, label);
}

/* Percusion en Drum Kit Mode (ver shared.hpp's DrumItem::drumKitGroups):
 * dos grupos, el 0 apunta a un sample senuelo que no existe, el 1 (el que
 * drumKitGroupIndex selecciona) apunta al sample real - si DrumSfzBuilder
 * eligiera el grupo equivocado (o ignorara el modo por completo) esta
 * percusion sonaria en silencio en vez de con el sample real, asi que esto
 * prueba la seleccion de indice de verdad, no solo "algo suena". */
static void write_drum_kit_mode(MemStream* m, int32_t rn, int32_t oi, const char* label,
                                const char* rel) {
    clap_ostream_t os = {.ctx = m, .write = mem_write};
    int32_t rc = 1;
    uint8_t hasSource = 1, isSfz = 1, drumKitModeEnabled = 1;
    int32_t drumKitGroupIndex = 1;
    uint32_t drumKitGroupCount = 2;
    char decoyRegion[256], realRegion[256];
    snprintf(decoyRegion, sizeof(decoyRegion), "<region>\nsample=nonexistent_decoy.wav\n");
    snprintf(realRegion, sizeof(realRegion), "<region>\nsample=%s\n", rel);

    mem_write(&os, &rn, 4);
    mem_write(&os, &oi, 4);
    mem_write(&os, &hasSource, 1);
    mem_write(&os, &isSfz, 1);
    write_string(m, label);
    write_string(m, ""); /* sourcePath - not exercised here */
    write_string(m, ""); /* sampleRelativePath - isSfz, unused */
    write_string(m, "(unused - Drum Kit Mode is on)"); /* regionsText fallback */
    mem_write(&os, &rc, 4);
    mem_write(&os, &drumKitModeEnabled, 1);
    mem_write(&os, &drumKitGroupIndex, 4);
    mem_write(&os, &drumKitGroupCount, 4);

    int32_t key0 = 10, regCount0 = 1;
    mem_write(&os, &key0, 4);
    write_string(m, decoyRegion);
    mem_write(&os, &regCount0, 4);

    int32_t key1 = 20, regCount1 = 1;
    mem_write(&os, &key1, 4);
    write_string(m, realRegion);
    mem_write(&os, &regCount1, 4);

    write_sample_tab_v3(m);
    /* dos puntos, para ejercitar el round-trip de un ampVelCurve NO vacio
     * (el caso 0-puntos ya lo cubre write_drum_no_kit arriba) - valores
     * elegidos para no ser triviales (ni 0/1 en ninguno de los dos campos),
     * asi un bug de desplazamiento de campos entre puntos no pasa
     * desapercibido. */
    int32_t ampVelocities[2] = {40, 100};
    float ampGains[2] = {0.2f, 0.9f};
    write_amp_curve_v4(m, ampVelocities, ampGains, 2);
    write_amp_envelope_v6(m);
    write_pan_random_v7(m);
    write_filter_v8(m, 1);
    write_pitch_v9(m, 1);
    write_pitch_invert_v10(m, 1);
    write_decay_extra_v11(m);
    write_custom_opcodes_v12(m, label);
}

static void build_state(MemStream* m, const char* samplePath, int rootNoteA, int outIdxA,
                        int rootNoteB, int outIdxB, int rootNoteC, int outIdxC) {
    clap_ostream_t os = {.ctx = m, .write = mem_write};
    uint32_t magic = 0x44465A53, version = 14, count = 3;
    mem_write(&os, &magic, 4);
    mem_write(&os, &version, 4);
    mem_write(&os, &count, 4);

    const char* rel = (samplePath && samplePath[0] == '/') ? samplePath + 1 : samplePath;

    /* percusion A */
    write_drum_no_kit(m, rootNoteA, outIdxA, 0, "Kick", samplePath, rel);

    /* percusion B - mismo sample, otra root note y otro output, para probar
     * que cada una suena solo en su propio output= */
    write_drum_no_kit(m, rootNoteB, outIdxB, 0, "Snare", samplePath, rel);

    /* percusion C - Drum Kit Mode, ver write_drum_kit_mode arriba */
    write_drum_kit_mode(m, rootNoteC, outIdxC, "HiHat", rel);

    /* v13: mpeEnabled, whole-instrument (no es per-drum, va despues de las
     * 3 percusiones) - 1 (no-default) para ejercitar sfizz_set_mpe_enabled
     * de verdad en el motor. */
    uint8_t mpeEnabled = 1;
    mem_write(&os, &mpeEnabled, 1);

    /* v14: bendUpCents/bendDownCents, whole-instrument - 4800/-4800
     * (no-default, consistente con mpeEnabled=1 arriba) para ejercitar el
     * <global> bendup=4800/benddown=-4800 real en el sfz generado. */
    int32_t bendUpCents = 4800, bendDownCents = -4800;
    mem_write(&os, &bendUpCents, 4);
    mem_write(&os, &bendDownCents, 4);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "uso: %s <plugin.clap> [sample.wav] [gui]\n", argv[0]);
        return 2;
    }
    const char* samplePath = NULL;
    int wantGui = 0;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "gui")) wantGui = 1;
        else samplePath = argv[i];
    }

    void* dl = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!dl) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    const clap_plugin_entry_t* entry = (const clap_plugin_entry_t*)dlsym(dl, "clap_entry");
    if (!entry) {
        fprintf(stderr, "sin clap_entry\n");
        return 1;
    }
    if (!entry->init(argv[1])) {
        fprintf(stderr, "entry->init fallo\n");
        return 1;
    }
    const clap_plugin_factory_t* factory =
        (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factory || factory->get_plugin_count(factory) < 1) {
        fprintf(stderr, "factory vacia\n");
        return 1;
    }
    const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
    printf("plugin: %s (%s) v%s\n", desc->name, desc->id, desc->version);

    clap_host_t host = {
        .clap_version = CLAP_VERSION_INIT,
        .host_data = NULL,
        .name = "mini_host",
        .vendor = "test",
        .url = "",
        .version = "1.0",
        .get_extension = host_get_extension,
        .request_restart = host_request_restart,
        .request_process = host_request_process,
        .request_callback = host_request_callback,
    };

    const clap_plugin_t* plug = factory->create_plugin(factory, &host, desc->id);
    if (!plug || !plug->init(plug)) {
        fprintf(stderr, "no se pudo crear/init\n");
        return 1;
    }
    if (!plug->activate(plug, 48000.0, 32, 512)) {
        fprintf(stderr, "activate fallo\n");
        return 1;
    }
    if (!plug->start_processing(plug)) {
        fprintf(stderr, "start_processing fallo\n");
        return 1;
    }

    /* Verifica el conteo/forma de los audio ports: 8 salidas stereo, 0
     * entradas. */
    const clap_plugin_audio_ports_t* ports =
        (const clap_plugin_audio_ports_t*)plug->get_extension(plug, CLAP_EXT_AUDIO_PORTS);
    if (!ports || ports->count(plug, false) != NUM_OUTPUTS || ports->count(plug, true) != 0) {
        fprintf(stderr, "audio ports incorrectos (esperaba %d salidas, 0 entradas)\n",
               NUM_OUTPUTS);
        return 1;
    }
    for (uint32_t i = 0; i < NUM_OUTPUTS; ++i) {
        clap_audio_port_info_t info;
        if (!ports->get(plug, i, false, &info) || info.channel_count != 2) {
            fprintf(stderr, "output port %u incorrecto\n", i);
            return 1;
        }
    }
    printf("audio ports: OK (%d salidas stereo)\n", NUM_OUTPUTS);

    enum { N = 256 };
    float outBufs[NUM_OUTPUTS][2][N];
    float* outChans[NUM_OUTPUTS][2];
    clap_audio_buffer_t outPorts[NUM_OUTPUTS];
    for (int o = 0; o < NUM_OUTPUTS; ++o) {
        outChans[o][0] = outBufs[o][0];
        outChans[o][1] = outBufs[o][1];
        outPorts[o] = (clap_audio_buffer_t){.data32 = outChans[o], .channel_count = 2};
    }
    EvCtx evCtx = {0};
    clap_input_events_t inEv = {.ctx = &evCtx, .size = ev_size, .get = ev_get};
    clap_output_events_t outEv = {.ctx = NULL, .try_push = ev_push};
    clap_process_t proc = {
        .steady_time = 0,
        .frames_count = N,
        .transport = NULL,
        .audio_inputs = NULL,
        .audio_outputs = outPorts,
        .audio_inputs_count = 0,
        .audio_outputs_count = NUM_OUTPUTS,
        .in_events = &inEv,
        .out_events = &outEv,
    };

    clap_event_midi_t noteOn = {
        .header = {.size = sizeof(clap_event_midi_t), .time = 0,
                   .space_id = CLAP_CORE_EVENT_SPACE_ID, .type = CLAP_EVENT_MIDI, .flags = 0},
        .port_index = 0,
        .data = {0x90, 60, 100},
    };
    clap_event_midi_t noteOff = {
        .header = {.size = sizeof(clap_event_midi_t), .time = 0,
                   .space_id = CLAP_CORE_EVENT_SPACE_ID, .type = CLAP_EVENT_MIDI, .flags = 0},
        .port_index = 0,
        .data = {0x80, 60, 0},
    };

    /* Sin percusiones cargadas todavia: no debe crashear ni dar NaN. */
    for (int b = 0; b < 8; ++b) {
        for (int o = 0; o < NUM_OUTPUTS; ++o)
            for (int i = 0; i < N; ++i) outBufs[o][0][i] = outBufs[o][1][i] = 1.f;
        evCtx.count = 0;
        if (b == 0) evCtx.events[evCtx.count++] = &noteOn.header;
        if (b == 4) evCtx.events[evCtx.count++] = &noteOff.header;
        clap_process_status st = plug->process(plug, &proc);
        if (st == CLAP_PROCESS_ERROR) {
            fprintf(stderr, "process devolvio error\n");
            return 1;
        }
        for (int o = 0; o < NUM_OUTPUTS; ++o)
            for (int i = 0; i < N; ++i)
                if (outBufs[o][0][i] != outBufs[o][0][i] || outBufs[o][1][i] != outBufs[o][1][i]) {
                    fprintf(stderr, "NaN en la salida %d (bloque %d, frame %d)\n", o, b, i);
                    return 1;
                }
    }
    printf("process (kit vacio): OK (%d bloques con note-on/off, sin crash, sin NaN)\n", 8);

    plug->stop_processing(plug);

    if (samplePath) {
        const clap_plugin_state_t* state =
            (const clap_plugin_state_t*)plug->get_extension(plug, CLAP_EXT_STATE);
        if (!state) {
            fprintf(stderr, "el plugin no expone clap.state\n");
            return 1;
        }

        /* Kick: root 60 -> Out 1 (indice 0). Snare: root 62 -> Out 4 (indice
         * 3). HiHat: root 65 -> Out 7 (indice 6), en Drum Kit Mode (ver
         * write_drum_kit_mode). Comprueba que cada nota suena SOLO en su
         * propio output=. */
        MemStream ms = {0};
        build_state(&ms, samplePath, 60, 0, 62, 3, 65, 6);
        clap_istream_t is = {.ctx = &ms, .read = mem_read};
        if (!state->load(plug, &is)) {
            fprintf(stderr, "state->load fallo\n");
            return 1;
        }
        printf("state->load OK (Kick key=60 out=1, Snare key=62 out=4, HiHat key=65 out=7 "
              "drum-kit-mode)\n");

        /* clap.note-name: Reaper's piano roll reads this to show "Kick"/
         * "Snare"/"HiHat" instead of raw note numbers - verifies count()/
         * get() report exactly the three loaded drums with their assigned
         * keys. */
        const clap_plugin_note_name_t* noteNames =
            (const clap_plugin_note_name_t*)plug->get_extension(plug, CLAP_EXT_NOTE_NAME);
        if (!noteNames) {
            fprintf(stderr, "el plugin no expone clap.note-name\n");
            return 1;
        }
        uint32_t nnCount = noteNames->count(plug);
        if (nnCount != 3) {
            fprintf(stderr, "note-name count=%u (esperaba 3)\n", nnCount);
            return 1;
        }
        int sawKick = 0, sawSnare = 0, sawHiHat = 0;
        for (uint32_t i = 0; i < nnCount; ++i) {
            clap_note_name_t nn;
            if (!noteNames->get(plug, i, &nn)) {
                fprintf(stderr, "note-name get(%u) fallo\n", i);
                return 1;
            }
            if (!strcmp(nn.name, "Kick") && nn.key == 60) sawKick = 1;
            if (!strcmp(nn.name, "Snare") && nn.key == 62) sawSnare = 1;
            if (!strcmp(nn.name, "HiHat") && nn.key == 65) sawHiHat = 1;
        }
        if (!sawKick || !sawSnare || !sawHiHat) {
            fprintf(stderr, "note-name entries incorrectas (Kick@60=%d Snare@62=%d HiHat@65=%d)\n",
                   sawKick, sawSnare, sawHiHat);
            return 1;
        }
        printf("clap.note-name: OK (Kick@60, Snare@62, HiHat@65)\n");

        if (!plug->start_processing(plug)) {
            fprintf(stderr, "start_processing (2) fallo\n");
            return 1;
        }

        clap_event_midi_t kickOn = noteOn; /* key 60 ya coincide */
        clap_event_midi_t snareOn = noteOn;
        snareOn.data[1] = 62;
        clap_event_midi_t hihatOn = noteOn;
        hihatOn.data[1] = 65;

        float maxAbs[NUM_OUTPUTS] = {0};
        for (int b = 0; b < 20; ++b) {
            for (int o = 0; o < NUM_OUTPUTS; ++o)
                for (int i = 0; i < N; ++i) outBufs[o][0][i] = outBufs[o][1][i] = 0.f;
            evCtx.count = 0;
            if (b == 0) {
                evCtx.events[evCtx.count++] = &kickOn.header;
                evCtx.events[evCtx.count++] = &snareOn.header;
                evCtx.events[evCtx.count++] = &hihatOn.header;
            }
            clap_process_status st = plug->process(plug, &proc);
            if (st == CLAP_PROCESS_ERROR) {
                fprintf(stderr, "process(2) devolvio error\n");
                return 1;
            }
            for (int o = 0; o < NUM_OUTPUTS; ++o)
                for (int i = 0; i < N; ++i) {
                    float a = fabsf(outBufs[o][0][i]);
                    if (a > maxAbs[o]) maxAbs[o] = a;
                    a = fabsf(outBufs[o][1][i]);
                    if (a > maxAbs[o]) maxAbs[o] = a;
                }
        }
        for (int o = 0; o < NUM_OUTPUTS; ++o)
            printf("  Out %d maxAbs=%.6f\n", o + 1, (double)maxAbs[o]);

        int samplePathExists = access(samplePath, F_OK) == 0;
        if (samplePathExists) {
            if (maxAbs[0] <= 1e-4f) {
                fprintf(stderr, "SILENCIO en Out 1 (Kick) - posible fallo de routing\n");
                return 1;
            }
            if (maxAbs[3] <= 1e-4f) {
                fprintf(stderr, "SILENCIO en Out 4 (Snare) - posible fallo de routing\n");
                return 1;
            }
            if (maxAbs[6] <= 1e-4f) {
                fprintf(stderr, "SILENCIO en Out 7 (HiHat) - Drum Kit Mode no selecciono el "
                               "grupo/indice correcto\n");
                return 1;
            }
            for (int o = 0; o < NUM_OUTPUTS; ++o) {
                if (o == 0 || o == 3 || o == 6) continue;
                if (maxAbs[o] > 1e-4f) {
                    fprintf(stderr, "Out %d deberia estar en silencio pero no lo esta\n", o + 1);
                    return 1;
                }
            }
            printf("routing multi-output: OK (Kick solo en Out 1, Snare solo en Out 4, HiHat "
                  "solo en Out 7 via Drum Kit Mode)\n");
        } else {
            printf("(samplePath no existe - silencio esperado, routing no verificado)\n");
        }
        plug->stop_processing(plug);

        /* roundtrip save -> load */
        MemStream ms2 = {0};
        clap_ostream_t os2 = {.ctx = &ms2, .write = mem_write};
        if (!state->save(plug, &os2)) {
            fprintf(stderr, "state->save fallo\n");
            return 1;
        }
        printf("state->save OK (%llu bytes)\n", (unsigned long long)ms2.len);
        clap_istream_t is2 = {.ctx = &ms2, .read = mem_read};
        if (!state->load(plug, &is2)) {
            fprintf(stderr, "roundtrip save->load fallo\n");
            return 1;
        }
        printf("roundtrip save->load OK\n");
    }

    if (wantGui) {
        const clap_plugin_gui_t* gui =
            (const clap_plugin_gui_t*)plug->get_extension(plug, CLAP_EXT_GUI);
        if (gui && gui->is_api_supported(plug, CLAP_WINDOW_API_X11, false)) {
            if (!gui->create(plug, CLAP_WINDOW_API_X11, false)) {
                fprintf(stderr, "gui create fallo\n");
                return 1;
            }
            uint32_t w = 0, h = 0;
            gui->get_size(plug, &w, &h);
            gui->show(plug);
            printf("gui creada y visible (%ux%u), renderizando 4 s...\n", w, h);
            fflush(stdout);
            sleep(4);
            gui->destroy(plug);
            printf("gui destruida (OK)\n");
        }
    }

    plug->deactivate(plug);
    plug->destroy(plug);
    entry->deinit();
    printf("todo OK\n");
    return 0;
}
