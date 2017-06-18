/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include <rtl-sdr.h>

#include "defines.h"
#include "input.h"

#define RADIO_BUFFER (512 * 1024)

static void help(const char *progname)
{
    fprintf(stderr, "Usage: %s [-d device-index] [-g gain] [-p ppm-error] [-r samples-input] [-w samples-output] [-o audio-output -f adts|wav] frequency program\n", progname); 
}

int main(int argc, char *argv[])
{
    int err, opt, device_index = 0, gain = 240, ppm_error = 0;
    unsigned int count, i, frequency = 0, program = 0;
    char *input_name = NULL, *output_name = NULL, *audio_name = NULL, *format_name = NULL;
    FILE *infp = NULL, *outfp = NULL;
    input_t input;
    output_t output;

    while ((opt = getopt(argc, argv, "r:w:d:p:o:f:g:")) != -1)
    {
        switch (opt)
        {
        case 'r':
            input_name = optarg;
            break;
        case 'w':
            output_name = optarg;
            break;
        case 'd':
            device_index = atoi(optarg);
            break;
        case 'p':
            ppm_error = atoi(optarg);
            break;
        case 'o':
            audio_name = optarg;
            break;
        case 'f':
            format_name = optarg;
            break;
        case 'g':
            gain = atoi(optarg);
            break;
        default:
            help(argv[0]);
            return 0;
        }
    }

    if (input_name == NULL)
    {
        if (optind + 2 != argc)
        {
            help(argv[0]);
            return 0;
        }
        frequency = strtoul(argv[optind], NULL, 0);
        program = strtoul(argv[optind+1], NULL, 0);

        count = rtlsdr_get_device_count();
        if (count == 0)
        {
            ERR("No devices found!\n");
            return 1;
        }

        for (i = 0; i < count; ++i)
            printf("[%d] %s\n", i, rtlsdr_get_device_name(i));
        printf("\n");

        if (device_index >= count)
        {
            ERR("Selected device does not exist.\n");
            return 1;
        }
    }
    else
    {
        if (optind + 1 != argc)
        {
            help(argv[0]);
            return 0;
        }
        program = strtoul(argv[optind], NULL, 0);

        if (strcmp(input_name, "-") == 0)
            infp = stdin;
        else
            infp = fopen(input_name, "rb");

        if (infp == NULL)
        {
            ERR("Unable to open input file.\n");
            return 1;
        }
    }

    if (output_name != NULL)
    {
        outfp = fopen(output_name, "wb");
        if (outfp == NULL)
        {
            ERR("Unable to open output file.\n");
            return 1;
        }
    }

    if (audio_name != NULL)
    {
        if (format_name == NULL)
        {
            ERR("Must specify an output format.\n");
            return 1;
        }
        else if (strcmp(format_name, "wav") == 0)
        {
            output_init_wav(&output, audio_name);
        }
        else if (strcmp(format_name, "adts") == 0)
        {
            output_init_adts(&output, audio_name);
        }
        else
        {
            ERR("Unknown output format.\n");
            return 1;
        }
    }
    else
    {
        output_init_live(&output);
    }

    input_init(&input, &output, frequency, program, outfp);

    if (infp)
    {
        while (!feof(infp))
        {
            uint8_t tmp[RADIO_BUFFER];
            size_t cnt;
            cnt = fread(tmp, 1, sizeof(tmp), infp);
            if (cnt > 0)
                input_cb(tmp, cnt, &input);
            input_wait(&input, 0);
        }
        input_wait(&input, 1);
    }
    else
    {
        rtlsdr_dev_t *dev;

        err = rtlsdr_open(&dev, 0);
        if (err) ERR_FAIL("rtlsdr_open error: %d\n", err);
        err = rtlsdr_set_sample_rate(dev, 1488375);
        if (err) ERR_FAIL("rtlsdr_set_sample_rate error: %d\n", err);
        err = rtlsdr_set_tuner_gain_mode(dev, 1);
        if (err) ERR_FAIL("rtlsdr_set_tuner_gain_mode error: %d\n", err);
        err = rtlsdr_set_tuner_gain(dev, gain);
        if (err) ERR_FAIL("rtlsdr_set_tuner_gain error: %d\n", err);
        err = rtlsdr_set_freq_correction(dev, ppm_error);
        if (err && err != -2) ERR_FAIL("rtlsdr_set_freq_correction error: %d\n", err);
        err = rtlsdr_set_center_freq(dev, frequency);
        if (err) ERR_FAIL("rtlsdr_set_center_freq error: %d\n", err);
        err = rtlsdr_reset_buffer(dev);
        if (err) ERR_FAIL("rtlsdr_reset_buffer error: %d\n", err);
        err = rtlsdr_read_async(dev, input_cb, &input, 15, RADIO_BUFFER);
        if (err) ERR_FAIL("rtlsdr error: %d\n", err);
        err = rtlsdr_close(dev);
        if (err) ERR_FAIL("rtlsdr error: %d\n", err);
    }

    return 0;
}
