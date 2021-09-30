#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#define M_PI 3.141592653589793238462

#include <jack/jack.h>
#include <jack/midiport.h>

#define N_VOICES 128
#define SYMPATHETIC_RESONANCE 0.05
#define BEND_RANGE 2 /* semitones */
#define N_DELAY_SAMPLES 2400
#define CUTOFF_LOOP_UNDAMPED 8000
#define CUTOFF_LOOP_DAMPED 500
#define CUTOFF_DC_BLOCKER 20
#define HAMMER_STRIKE_POSITION 0.2

static double note_frequency (double note) {

    return 440 * pow (2, (note - 69) / 12.0);
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

typedef struct voice_t {

    delay_t delay;
    filter_t filter_loop;
    filter_t filter_dc_blocker;
    double frequency;
    double output;
    bool playing;

    double rate;

} voice_t;

void voice_init (voice_t *voice, int note) {

    memset (voice, 0, sizeof (voice_t));
    voice->frequency = note_frequency (note);
    delay_init (&voice->delay, N_DELAY_SAMPLES);
    filter_init (&voice->filter_loop);
    filter_init (&voice->filter_dc_blocker);
}

void voice_terminate (voice_t *voice) {

    delay_terminate (&voice->delay);
}

void voice_update (voice_t *voice) {

    double cutoff_loop = voice->playing ? CUTOFF_LOOP_UNDAMPED : CUTOFF_LOOP_DAMPED;

    delay_period_set (&voice->delay, voice->frequency, voice->rate);
    filter_cutoff_set (&voice->filter_loop, cutoff_loop, voice->rate);
    filter_cutoff_set (&voice->filter_dc_blocker, CUTOFF_DC_BLOCKER, voice->rate);
}

void voice_rate_set (voice_t *voice, double rate) {

    voice->rate = rate;
    voice_update (voice);
}

void voice_process (voice_t *voice, double input) {

    double output = *voice->delay.buffer_pointer;
    double filter_loop = filter_process (&voice->filter_loop, output);
    double filter_dc_blocker = filter_process_high_pass (&voice->filter_dc_blocker, filter_loop);
    delay_process (&voice->delay, input + filter_dc_blocker);
    voice->output = output;
}

static void voice_excite (voice_t *voice) {

    size_t i;
    for (i = 0; i < voice->delay.n_samples; i++) {

        double position = i / (double) voice->delay.n_samples * 2;
        double sample = 1;

        if (position > 1) {

            position = 2 - position;
            sample = -1;
        }

        if (position < HAMMER_STRIKE_POSITION)
            sample *= position / HAMMER_STRIKE_POSITION;
        else
            sample *= 1 - (position - HAMMER_STRIKE_POSITION) / (1 - HAMMER_STRIKE_POSITION);

        voice->delay.buffer_head[i] = sample / 2;
    }
}

void voice_note_on (voice_t *voice, double velocity) {

    voice->playing = true;
    filter_cutoff_set (&voice->filter_loop, CUTOFF_LOOP_UNDAMPED, voice->rate);
    voice_excite (voice);
}

void voice_note_off (voice_t *voice, double velocity) {

    voice->playing = false;
    filter_cutoff_set (&voice->filter_loop, CUTOFF_LOOP_DAMPED, voice->rate);
}

typedef struct resonator_t {

    int dummy;

} resonator_t;

void resonator_init (resonator_t *resonator) {

    memset (resonator, 0, sizeof (resonator_t));
}

void resonator_terminate (resonator_t *resonator) {

}

double resonator_process (resonator_t *resonator, double input) {

    return input;
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

        /* TODO
         * learn abt coupling between transverse planes n longitudinal 
         * due to the bridge ?????? */

        double output_resonator;

        double output_sum_voices = 0;
        for (i = 0; i < N_VOICES; i++)
            output_sum_voices += synth->voices[i].output;

        output_resonator = resonator_process (&synth->resonator, output_sum_voices);

        for (i = 0; i < N_VOICES; i++)
            voice_process (&synth->voices[i], SYMPATHETIC_RESONANCE * output_resonator);

        buffer[i] = output_resonator;
    }
}

void synth_process_midi_note_off (synth_t *synth, int channel, int note, int velocity) {

    voice_note_off (&synth->voices[note], velocity);
}

void synth_process_midi_note_on (synth_t *synth, int channel, int note, int velocity) {

    voice_note_on (&synth->voices[note], velocity);
}

void synth_process_midi_cc (synth_t *synth, int channel, int controller, int value) {

    switch (controller) {

        case 1: /* modulation wheel */
            break;

        case 11: /* expression */
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
            synth_process_midi_note_off (synth, channel, data[1], data[2]);
            break;

        case 0x90: /* note on */
            synth_process_midi_note_on (synth, channel, data[1], data[2]);
            break;

        case 0xa0: /* polyphonic key pressure */
            break;

        case 0xb0: /* control change */
            synth_process_midi_cc (synth, channel, data[1], data[2]);
            break;

        case 0xc0: /* program change */
            break;

        case 0xd0: /* channel pressure */
            break;

        case 0xe0: /* pitch bend */
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

    jack_context_t context;
    memset (&context, 0, sizeof (context));

    puts ("hewwo dewe!!😊");

    if (!(context.client = jack_client_open ("synth", JackNullOption, NULL))) {

        fputs ("waaaaaa waaaaaaaa i couldnt connect to jack server 😢", stderr);
        return EXIT_FAILURE;
    }

    synth_init (&context.synth);

    jack_set_process_callback     (context.client, jack_process,  &context);
    jack_set_sample_rate_callback (context.client, jack_set_rate, &context);
    jack_on_shutdown              (context.client, jack_shutdown, &context);

    context.port_midi_in = jack_port_register (context.client, "midi_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    context.port_audio_out = jack_port_register (context.client, "audio_out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

    if (jack_activate (context.client)) {

        fputs ("*breaths heavily n upsettily* cant activate jack cwient ? ? 😭", stderr);
        return EXIT_FAILURE;
    }

    jack_connect (context.client, "synth:audio_out", "system:playback_1");
    jack_connect (context.client, "synth:audio_out", "system:playback_2");

    pause ();

    synth_terminate (&context.synth);

    jack_client_close (context.client);

    return EXIT_SUCCESS;
}
