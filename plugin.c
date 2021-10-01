#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

#define M_PI 3.141592653589793238462

#include <jack/jack.h>
#include <jack/midiport.h>

#define PATH_IMPULSE_RESPONSE "ir.pcm"

#define N_VOICES 128
#define VOICE_MIN 36
#define VOICE_MAX 97
#define SYMPATHETIC_RESONANCE /*10*/5
#define BEND_RANGE 2 /* semitones */
#define N_DELAY_SAMPLES 8000
#define BRIDGE_COEFFICIENT_BYPASS_MIN 0.00/*0.0025*/
#define BRIDGE_COEFFICIENT_BYPASS_MAX 0.00/*0.0025*/
#define RESONANCE_BODY 1
#define CUTOFF_DC_BLOCKER 20
#define CUTOFF_BRIDGE_MIN 500/*16000*/
#define CUTOFF_BRIDGE_MAX 24000/*16000*/
#define CUTOFF_DAMPER 600
#define CUTOFF_FINGER 500
#define COEFFICIENT_TRANSITION_FINGER_INTERPOLATION_EXPONENT 15
#define COEFFICIENT_TRANSITION_FINGER_MAX 100000
#define COEFFICIENT_TRANSITION_FINGER_MIN 10
#define COEFFICIENT_TRANSITION_FINGER_NOTE_OFF 20
#define COEFFICIENT_TRANSITION_DAMPER 10
#define HAMMER_STRIKE_POSITION_CENTER /*0.15*/0.5
#define HAMMER_STRIKE_POSITION_VARIATION 0.05 /* plus or minus */
#define VOLUME /*5*/ 2

static double noise () {

    return rand () / (double) RAND_MAX;
}

static double lerp (double x, double a, double b) {

    return a + x * (b - a);
}

static double interpolate_exponential (double x, double exp, double a, double b) {

    return lerp (pow (x, exp), a, b);
}

typedef struct buffer_t {

    size_t n_samples;
    double *data;

} buffer_t;

void buffer_init (buffer_t *buffer, size_t n_samples, double *data) {

    buffer->n_samples = n_samples;
    buffer->data = data;
}

void buffer_load (buffer_t *buffer, char *path) {

    FILE *stream;
    size_t n_samples;
    double *data;

    if (!(stream = fopen (path, "rb"))) {

        fprintf (stderr, "cldnt open da file %s... 😭\n", path);
        exit (EXIT_FAILURE);
    }

    fseek (stream, 0, SEEK_END);
    n_samples = ftell (stream) / sizeof (double);
    rewind (stream);

    data = calloc (n_samples, sizeof (double));
    fread (data, sizeof (double), n_samples, stream);

    fclose (stream);

    buffer_init (buffer, n_samples, data);
}

void buffer_terminate (buffer_t *buffer) {

    free (buffer->data);
}

typedef struct filter_t {

    double state;
    double coefficient;

} filter_t;

void filter_init (filter_t *filter) {

    memset (filter, 0, sizeof (filter_t));
}

void filter_terminate (filter_t *filter) {

}

void filter_cutoff_set (filter_t *filter, double cutoff, double rate) {

    double dc_constant = 1.0 / (2 * M_PI * cutoff);
    filter->coefficient = 1 - exp (-1 / rate / dc_constant);
}

double filter_process (filter_t *filter, double input) {

    return filter->state += filter->coefficient * (input - filter->state);
}

double filter_process_high_pass (filter_t *filter, double input) {

    return input - filter_process (filter, input);
}

typedef struct delay_t {

    double *buffer_head;
    double *buffer_tail;
    double *buffer_pointer;

    size_t n_samples;

} delay_t;

void delay_init (delay_t *delay, size_t n_samples) {

    delay->buffer_head = calloc (n_samples, sizeof (double));
    delay->buffer_tail = delay->buffer_head + n_samples;
    delay->buffer_pointer = delay->buffer_head;
    delay->n_samples = n_samples;
}

void delay_terminate (delay_t *delay) {

    free (delay->buffer_head);
}

void delay_length_set (delay_t *delay, size_t n_samples) {

    double *buffer_tail_old = delay->buffer_tail;
    delay->buffer_tail = delay->buffer_head + n_samples;
    if (delay->buffer_tail > buffer_tail_old)
        memset (buffer_tail_old, 0, sizeof (double) * (delay->buffer_tail - buffer_tail_old));
    delay->n_samples = n_samples;
}

void delay_period_set (delay_t *delay, double frequency, double rate) {

    delay_length_set (delay, rate / frequency);
}

void delay_process (delay_t *delay, double input) {

    *(delay->buffer_pointer++) = input;
    if (delay->buffer_pointer >= delay->buffer_tail)
        delay->buffer_pointer -= delay->buffer_tail - delay->buffer_head;
}

typedef struct convolver_t {

    size_t i_sample;
    buffer_t impulse_response;
    delay_t memory;

} convolver_t;

void convolver_init (convolver_t *convolver, char *path_impulse_response) {

    memset (convolver, 0, sizeof (convolver_t));
    buffer_load (&convolver->impulse_response, path_impulse_response);
    delay_init (&convolver->memory, convolver->impulse_response.n_samples);
}

void convolver_terminate (convolver_t *convolver) {

    buffer_terminate (&convolver->impulse_response);
    delay_terminate (&convolver->memory);
}

double convolver_process (convolver_t *convolver, double input) {

    size_t i;
    double output = 0;

    delay_process (&convolver->memory, input);

    for (i = 0; i < convolver->impulse_response.n_samples; i++) {

        int i_sample = convolver->i_sample - i;
        while (i_sample < 0) {
            i_sample += convolver->impulse_response.n_samples;
        }
        output += convolver->impulse_response.data[i]
                * convolver->memory.buffer_head[i_sample];
    }

    convolver->i_sample = (convolver->i_sample + 1) % convolver->memory.n_samples;

    return output;
}

typedef struct bridge_t {

    filter_t filter;
    double coefficient_bypass;

} bridge_t;

void bridge_init (bridge_t *bridge, double coefficient_bypass) {

    filter_init (&bridge->filter);
    bridge->coefficient_bypass = coefficient_bypass;
}

void bridge_terminate (bridge_t *bridge) {

    filter_terminate (&bridge->filter);
}

void bridge_cutoff_set (bridge_t *bridge, double cutoff, double rate) {

    filter_cutoff_set (&bridge->filter, cutoff, rate);
}

double bridge_process (bridge_t *bridge, double input) {

    double bypass = bridge->coefficient_bypass * input;
    return filter_process (&bridge->filter, input - bypass);
}

typedef struct voice_t {

    delay_t delay;
    filter_t filter_dc_blocker;
    filter_t filter_damper;
    filter_t filter_finger;
    filter_t filter_transition_damper;
    filter_t filter_transition_finger;
    bridge_t bridge_input;
    bridge_t bridge_output;
    double frequency;
    double cutoff_bridge;
    double output;
    double target_coefficient_damper;
    double target_coefficient_finger;
    double coefficient_transition_finger;
    double sustain;

    double rate;

} voice_t;

void voice_init (voice_t *voice, int note) {

    double bypass;
    memset (voice, 0, sizeof (voice_t));
    voice->frequency = 440 * pow (2, (note - 69) / 12.0);
    bypass = interpolate_exponential (note / 127.0,
                                      2,
                                      BRIDGE_COEFFICIENT_BYPASS_MIN,
                                      BRIDGE_COEFFICIENT_BYPASS_MAX);
    voice->cutoff_bridge = interpolate_exponential (note / 127.0,
                                                    2,
                                                    CUTOFF_BRIDGE_MIN,
                                                    CUTOFF_BRIDGE_MAX);
    delay_init (&voice->delay, N_DELAY_SAMPLES);
    filter_init (&voice->filter_dc_blocker);
    filter_init (&voice->filter_damper);
    filter_init (&voice->filter_finger);
    filter_init (&voice->filter_transition_damper);
    filter_init (&voice->filter_transition_finger);
    bridge_init (&voice->bridge_input, bypass);
    bridge_init (&voice->bridge_output, bypass);
    voice->target_coefficient_damper = 0;
    voice->target_coefficient_finger = 0;
    voice->coefficient_transition_finger = COEFFICIENT_TRANSITION_FINGER_MAX;
    voice->sustain = 1;
}

void voice_terminate (voice_t *voice) {

    delay_terminate (&voice->delay);
    filter_terminate (&voice->filter_dc_blocker);
    filter_terminate (&voice->filter_damper);
    filter_terminate (&voice->filter_finger);
    filter_terminate (&voice->filter_transition_damper);
    filter_terminate (&voice->filter_transition_finger);
    bridge_terminate (&voice->bridge_input);
    bridge_terminate (&voice->bridge_output);
}

void voice_update (voice_t *voice) {

    delay_period_set (&voice->delay, voice->frequency, voice->rate);
    filter_cutoff_set (&voice->filter_dc_blocker, CUTOFF_DC_BLOCKER, voice->rate);
    filter_cutoff_set (&voice->filter_damper, CUTOFF_DAMPER, voice->rate);
    filter_cutoff_set (&voice->filter_finger, CUTOFF_FINGER, voice->rate);
    filter_cutoff_set (&voice->filter_transition_damper, COEFFICIENT_TRANSITION_DAMPER, voice->rate);
    filter_cutoff_set (&voice->filter_transition_finger, voice->coefficient_transition_finger, voice->rate);
    bridge_cutoff_set (&voice->bridge_input, voice->cutoff_bridge, voice->rate);
    bridge_cutoff_set (&voice->bridge_output, voice->cutoff_bridge, voice->rate);
}

void voice_rate_set (voice_t *voice, double rate) {

    voice->rate = rate;
    voice_update (voice);
}

void voice_process (voice_t *voice, double input) {

    double target_coefficient_finger;
    double coefficient_damper;
    double coefficient_finger;
    double delay;
    double dc_blocker;
    double damper_damped;
    double damper_undamped;
    double reflection_damper;
    double pre_termination;
    double finger_damped;
    double finger_undamped;
    double reflection_finger;
    double termination;
    double reflection_bridge_output;
    double transmission_bridge_input;

    /* transitions */
    target_coefficient_finger = voice->sustain * voice->target_coefficient_finger;
    coefficient_damper = filter_process (&voice->filter_transition_damper,
                                         voice->target_coefficient_damper);
    coefficient_finger = filter_process (&voice->filter_transition_finger,
                                         target_coefficient_finger);

    /* dc blocker */
    delay = *voice->delay.buffer_pointer;
    dc_blocker = filter_process_high_pass (&voice->filter_dc_blocker, delay);

    /* damper */
    damper_damped = coefficient_damper * dc_blocker;
    damper_undamped = dc_blocker - damper_damped;
    reflection_damper = filter_process (&voice->filter_damper, damper_damped);
    pre_termination = reflection_damper + damper_undamped;

    /* finger */
    finger_damped = coefficient_finger * pre_termination;
    finger_undamped = pre_termination - finger_damped;
    reflection_finger = filter_process (&voice->filter_finger, finger_damped);
    termination = reflection_finger + finger_undamped;

    /* termination */
    reflection_bridge_output = bridge_process (&voice->bridge_output, termination);
    voice->output = termination - reflection_bridge_output;
    transmission_bridge_input = bridge_process (&voice->bridge_input, input);
    delay_process (&voice->delay, transmission_bridge_input + reflection_bridge_output);
}

static void voice_excite (voice_t *voice, double velocity) {

    double hammer_strike_position = HAMMER_STRIKE_POSITION_CENTER
                                  + HAMMER_STRIKE_POSITION_VARIATION
                                  * (noise () * 2 - 1);

    size_t i;
    for (i = 0; i < voice->delay.n_samples; i++) {

        double position = i / (double) voice->delay.n_samples * 2;
        double sample = velocity;

        if (position > 1) {

            position = 2 - position;
            sample = -velocity;
        }

        if (position < hammer_strike_position)
            sample *= position / hammer_strike_position;
        else
            sample *= 1 - (position - hammer_strike_position) / (1 - hammer_strike_position);

        delay_process (&voice->delay, *voice->delay.buffer_pointer + sample / 2);
    }
}

void voice_note_on (voice_t *voice, double velocity) {

    double velocity_normalized = velocity / 127.0;
    voice->target_coefficient_finger = 0;
    voice->filter_transition_finger.state = 1;
    voice_excite (voice, velocity_normalized);
    voice->coefficient_transition_finger
        = interpolate_exponential (velocity_normalized,
                                   COEFFICIENT_TRANSITION_FINGER_INTERPOLATION_EXPONENT,
                                   COEFFICIENT_TRANSITION_FINGER_MIN,
                                   COEFFICIENT_TRANSITION_FINGER_MAX);
    voice_update (voice);
}

void voice_note_off (voice_t *voice, double velocity) {

    voice->target_coefficient_finger = 1;
    voice->coefficient_transition_finger = COEFFICIENT_TRANSITION_FINGER_NOTE_OFF;
    voice_update (voice);
}

void voice_damper_set (voice_t *voice, double damper) {

    voice->target_coefficient_damper = damper;
}

void voice_sustain_set (voice_t *voice, double sustain) {

    voice->sustain = sustain;
}

typedef struct resonator_t {

    convolver_t convolver;

} resonator_t;

void resonator_init (resonator_t *resonator) {

    memset (resonator, 0, sizeof (resonator_t));
    convolver_init (&resonator->convolver, PATH_IMPULSE_RESPONSE);
}

void resonator_terminate (resonator_t *resonator) {

    convolver_terminate (&resonator->convolver);
}

double resonator_process (resonator_t *resonator, double input) {

    return lerp (RESONANCE_BODY, input, convolver_process (&resonator->convolver, input));
}

typedef struct synth_t {

    voice_t voices[N_VOICES];
    resonator_t resonator;

    double bend;

    double rate;
    double delta_time;

} synth_t;

void synth_init (synth_t *synth) {

    size_t i;

    memset (synth, 0, sizeof (synth_t));

    for (i = 0; i < N_VOICES; i++)
        voice_init (&synth->voices[i], i);

    resonator_init (&synth->resonator);
}

void synth_terminate (synth_t *synth) {

    size_t i;

    for (i = 0; i < N_VOICES; i++)
        voice_terminate (&synth->voices[i]);

    resonator_terminate (&synth->resonator);
}

static void synth_update (synth_t *synth) {

    size_t i;

    for (i = 0; i < N_VOICES; i++)
        voice_rate_set (&synth->voices[i], synth->rate);
}

void synth_process_audio (synth_t *synth,
                          jack_nframes_t n_frames,
                          jack_default_audio_sample_t *buffer) {

    size_t i;

    for (i = 0; i < n_frames; i++) {

        size_t j;

        /* TODO
         * learn abt coupling between transverse planes n longitudinal 
         * due to the bridge ?????? */

        double output_resonator;
        double reflection;
        double distributed;

        double output_sum_voices = 0;
        for (j = VOICE_MIN; j < VOICE_MAX; j++)
            output_sum_voices += synth->voices[j].output;

        reflection = SYMPATHETIC_RESONANCE * output_sum_voices;
        distributed = reflection / N_VOICES;

        for (j = VOICE_MIN; j < VOICE_MAX; j++)
            voice_process (&synth->voices[j], distributed);

        output_resonator = resonator_process (&synth->resonator, output_sum_voices);
        buffer[i] = VOLUME * output_resonator;
    }
}

void synth_process_midi_note_off (synth_t *synth, int channel, int note, int velocity) {

    voice_note_off (&synth->voices[note], velocity);
}

void synth_process_midi_note_on (synth_t *synth, int channel, int note, int velocity) {

    voice_note_on (&synth->voices[note], velocity);
}

void synth_process_midi_cc (synth_t *synth, int channel, int controller, int value) {

    size_t i;

    switch (controller) {

        case 1: /* modulation wheel */
            for (i = 0; i < N_VOICES; i++)
                voice_damper_set (&synth->voices[i], value / 127.0);
            break;

        case 11: /* expression */
            break;

        case 64: /* sustain pedal */
            for (i = 0; i < N_VOICES; i++)
                voice_sustain_set (&synth->voices[i], value / 127.0);
            break;
    }
}

void synth_process_midi_bend (synth_t *synth, int channel, int lsb, int msb) {

    int value = (msb << 7) | lsb;
    synth->bend = (value / (double) 0x2000 - 1) * BEND_RANGE;
}

void synth_process_midi (synth_t *synth, jack_midi_data_t *data) {

    int status  = data[0] & 0xf0;
    int channel = data[0] & 0x0f;

    switch (status) {

        case 0x80: /* note off */
            printf ("note off: %x %x\n", data[1], data[2]);
            synth_process_midi_note_off (synth, channel, data[1], data[2]);
            break;

        case 0x90: /* note on */
            printf ("note on: %x %x\n", data[1], data[2]);
            synth_process_midi_note_on (synth, channel, data[1], data[2]);
            break;

        case 0xa0: /* polyphonic key pressure */
            break;

        case 0xb0: /* control change */
            printf ("control change: %x %x\n", data[1], data[2]);
            synth_process_midi_cc (synth, channel, data[1], data[2]);
            break;

        case 0xc0: /* program change */
            break;

        case 0xd0: /* channel pressure */
            break;

        case 0xe0: /* pitch bend */
            printf ("pitch bend: %x %x\n", data[1], data[2]);
            synth_process_midi_bend (synth, channel, data[1], data[2]);
            break;
    }
}

typedef struct jack_context_t {

    jack_client_t *client;
    jack_port_t *port_midi_in;
    jack_port_t *port_audio_out;
    
    synth_t synth;

} jack_context_t;

int jack_process (jack_nframes_t n_frames, void *arg) {

    size_t i;

    jack_context_t *context = (jack_context_t *) arg;

    jack_default_audio_sample_t *buffer_audio_out =
        (jack_default_audio_sample_t *) jack_port_get_buffer (context->port_audio_out, n_frames);
    void *buffer_midi_in =              jack_port_get_buffer (context->port_midi_in, n_frames);

    jack_nframes_t n_events = jack_midi_get_event_count (buffer_midi_in);

    size_t i_frame = 0;

    for (i = 0; i < n_events; i++) {

        jack_midi_event_t event;
        jack_midi_event_get (&event, buffer_midi_in, i);

        /* process audio frames up to the time of this event */
        synth_process_audio (&context->synth,
                             event.time - i_frame,
                             buffer_audio_out + i_frame);
        i_frame = event.time;
        
        /* process the midi event */
        synth_process_midi (&context->synth, event.buffer);
    }

    /* process remaining audio frames */
    synth_process_audio (&context->synth,
                         n_frames - i_frame,
                         buffer_audio_out + i_frame);

    /* process audio */
    return 0;
}

int jack_set_rate (jack_nframes_t rate, void *arg) {

    jack_context_t *context = (jack_context_t *) arg;

    context->synth.rate = rate;
    context->synth.delta_time = 1.0 / rate;
    synth_update (&context->synth);

    return 0;
}

void jack_shutdown (void *arg) {

    fputs ("what...... jack shutdown apparently..... bye bye.... x_x", stderr);
    exit (EXIT_FAILURE);
}

int main (int argc, char **argv) {

    srand (time (NULL));

    jack_context_t *context = malloc (sizeof (jack_context_t));
    memset (context, 0, sizeof (jack_context_t));

    puts ("hewwo dewe!!😊");

    if (!(context->client = jack_client_open ("synth", JackNullOption, NULL))) {

        fputs ("waaaaaa waaaaaaaa i couldnt connect to jack server 😢", stderr);
        return EXIT_FAILURE;
    }

    synth_init (&context->synth);

    jack_set_process_callback     (context->client, jack_process,  context);
    jack_set_sample_rate_callback (context->client, jack_set_rate, context);
    jack_on_shutdown              (context->client, jack_shutdown, context);

    context->port_midi_in = jack_port_register (context->client, "midi_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    context->port_audio_out = jack_port_register (context->client, "audio_out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

    if (jack_activate (context->client)) {

        fputs ("*breaths heavily n upsettily* cant activate jack cwient ? ? 😭", stderr);
        return EXIT_FAILURE;
    }

    jack_connect (context->client, "synth:audio_out", "system:playback_1");
    jack_connect (context->client, "synth:audio_out", "system:playback_2");

    pause ();

    synth_terminate (&context->synth);

    jack_client_close (context->client);

    return EXIT_SUCCESS;
}
