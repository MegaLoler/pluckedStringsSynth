#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#define M_PI 3.141592653589793238462

#include <jack/jack.h>
#include <jack/midiport.h>

#define BEND_RANGE 2 /* semitones */

static double note_frequency (double note) {

    return 440 * pow (2, (note - 69) / 12.0);
}

typedef struct synth_t {

    double phase;
    double phase_delta;
    double amplitude;

    int note;
    double velocity;
    double bend;
    double delta_time;

} synth_t;

typedef struct jack_context_t {

    jack_client_t *client;
    jack_port_t *port_midi_in;
    jack_port_t *port_audio_out;
    
    synth_t synth;

} jack_context_t;

void synth_init (synth_t *synth) {

    memset (synth, 0, sizeof (synth_t));
}

void synth_terminate (synth_t *synth) {

}

static void synth_process_update (synth_t *synth) {

    synth->phase_delta = 2 * M_PI * synth->delta_time * note_frequency (synth->note + synth->bend);
    synth->amplitude = synth->velocity;
}

void synth_process_audio (synth_t *synth,
                          jack_nframes_t n_frames,
                          jack_default_audio_sample_t *buffer) {

    size_t i;

    for (i = 0; i < n_frames; i++) {

        buffer[i] = sin (synth->phase) * synth->amplitude;
        synth->phase += synth->phase_delta;
    }
}

void synth_process_midi_note_off (synth_t *synth, int channel, int note, int velocity) {

    if (synth->note == note)
        synth->phase_delta = 0;
}

void synth_process_midi_note_on (synth_t *synth, int channel, int note, int velocity) {

    synth->note = note;
    synth->velocity = velocity / 127.0;
    synth_process_update (synth);
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

    if (synth->phase_delta > 0)
        synth_process_update (synth);
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

    context->synth.delta_time = 1.0 / rate;

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
