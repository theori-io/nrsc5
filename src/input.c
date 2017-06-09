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

#include <assert.h>
#include <math.h>
#include <string.h>

#include "defines.h"
#include "input.h"

static float filter_taps[] = {
    -3.1840741598898603e-07, -1.4398128769244067e-06, -3.6516169075184735e-06, -5.033695742895361e-06,
    -1.6002641132217832e-06, 8.511941814504098e-06, 2.092276918119751e-05, 2.599440995254554e-05,
    1.658140536164865e-05, -2.73751288659696e-06, -1.494094522058731e-05, -4.428476586326724e-06,
    2.1577205188805237e-05, 2.419546763121616e-05, -4.45509867859073e-05, -0.00019140906806569546,
    -0.00033925892785191536, -0.0003410702047403902, -7.116221240721643e-05, 0.00044868135591968894,
    0.0009848196059465408, 0.0011756004532799125, 0.0007568727596662939, -0.00020314345601946115,
    -0.0012503924081102014, -0.0017776219174265862, -0.0014411613810807467, -0.0004716941330116242,
    0.0004106566484551877, 0.0005164188332855701, -0.0001815646537579596, -0.0008261324837803841,
    -0.00019443688506726176, 0.0022477414458990097, 0.0054923356510698795, 0.007147735450416803,
    0.0048399437218904495, -0.0018647494725883007, -0.010383015498518944, -0.01598448120057583,
    -0.014497561380267143, -0.005301022902131081, 0.00754120759665966, 0.017228176817297935,
    0.01842351257801056, 0.011025095358490944, 0.000631955626886338, -0.005190723575651646,
    -0.0026981299743056297, 0.004348477348685265, 0.006455086637288332, -0.004562899004667997,
    -0.027618085965514183, -0.049557339400053024, -0.05114372819662094, -0.019698400050401688,
    0.03912949562072754, 0.09913070499897003, 0.12474612891674042, 0.09123805910348892,
    0.0024822356645017862, -0.10541539639234543, -0.17930030822753906, -0.17645931243896484,
    -0.08956720679998398, 0.04578504338860512, 0.1667986363172531, 0.21501903235912323,
    0.1667986363172531, 0.045785047113895416, -0.08956720679998398, -0.17645931243896484,
    -0.17930030822753906, -0.10541539639234543, 0.0024822354316711426, 0.09123806655406952,
    0.12474613636732101, 0.09913071244955063, 0.03912949934601784, -0.01969839632511139,
    -0.05114372819662094, -0.04955732822418213, -0.027618087828159332, -0.004562899004667997,
    0.006455087102949619, 0.004348477814346552, -0.0026981295086443424, -0.005190724041312933,
    0.0006319556850939989, 0.01102509256452322, 0.01842351444065571, 0.017228178679943085,
    0.007541206199675798, -0.005301022902131081, -0.014497562311589718, -0.015984486788511276,
    -0.01038301270455122, -0.0018647491233423352, 0.004839944187551737, 0.0071477326564490795,
    0.005492334719747305, 0.002247741911560297, -0.0001944369578268379, -0.0008261321345344186,
    -0.00018156461010221392, 0.0005164191243238747, 0.0004106564447283745, -0.0004716940165963024,
    -0.0014411617303267121, -0.0017776231979951262, -0.0012503918260335922, -0.00020314344146754593,
    0.0007568733999505639, 0.001175599405542016, 0.0009848191402852535, 0.0004486811230890453,
    -7.116223423508927e-05, -0.00034107023384422064, -0.00033925872412510216, -0.00019140943186357617,
    -4.455095768207684e-05, 2.4195489459089004e-05, 2.157725793949794e-05, -4.428477495821426e-06,
    -1.4940917935746256e-05, -2.737505610639346e-06, 1.6581456293351948e-05, 2.599446270323824e-05,
    2.092281101795379e-05, 8.511918167641852e-06, -1.6001998801584705e-06, -5.033375146012986e-06,
    -3.6515630199573934e-06, -1.4396895267054788e-06, -3.1840741598898603e-07
};

static void *input_worker(void *arg)
{
    input_t *st = arg;

    while (1)
    {
        pthread_mutex_lock(&st->mutex);
        while (st->avail - st->used < K)
            pthread_cond_wait(&st->cond, &st->mutex);

        if (st->skip)
        {
            if (st->skip > st->avail - st->used)
            {
                st->skip -= st->avail - st->used;
                st->used = st->avail;
            }
            else
            {
                st->used += st->skip;
                st->skip = 0;
            }
        }

        st->used += acquire_push(&st->acq, &st->buffer[st->used], st->avail - st->used);
        acquire_process(&st->acq);
        pthread_mutex_unlock(&st->mutex);
        pthread_cond_signal(&st->cond);
    }

    return NULL;
}

void input_pdu_push(input_t *st, uint8_t *pdu, unsigned int len)
{
    output_push(st->output, pdu, len);
}

void input_rate_adjust(input_t *st, float adj)
{
    st->resamp_rate += adj;
}

void input_set_skip(input_t *st, unsigned int skip)
{
    st->skip = skip;
}

void input_wait(input_t *st)
{
    pthread_mutex_lock(&st->mutex);
    while (st->avail - st->used > K)
        pthread_cond_wait(&st->cond, &st->mutex);
    pthread_mutex_unlock(&st->mutex);
}

void input_cb(uint8_t *buf, uint32_t len, void *arg)
{
    unsigned int i, new_avail, cnt = len / 4;
    input_t *st = arg;

    if (st->outfp)
        fwrite(buf, 1, len, st->outfp);

    pthread_mutex_lock(&st->mutex);
    if (cnt + st->avail > INPUT_BUF_LEN)
    {
        if (st->avail > st->used)
        {
            memmove(&st->buffer[0], &st->buffer[st->used], (st->avail - st->used) * sizeof(st->buffer[0]));
            st->avail -= st->used;
            st->used = 0;
        }
        else
        {
            st->avail = 0;
            st->used = 0;
        }
    }
    new_avail = st->avail;
    resamp_crcf_set_rate(st->resamp, st->resamp_rate);
    pthread_mutex_unlock(&st->mutex);

    if (cnt + new_avail > INPUT_BUF_LEN)
        ERR("input buffer overflow!\n");
    assert(len % 4 == 0);

#define U8_F(x) ( (((float)(x)) - 127) / 128 )
    for (i = 0; i < cnt; i++)
    {
        float complex x[2], y;
        unsigned int nw;
        x[0] = CMPLXF(U8_F(buf[i * 4 + 0]), U8_F(buf[i * 4 + 1]));
        x[1] = CMPLXF(U8_F(buf[i * 4 + 2]), U8_F(buf[i * 4 + 3]));
        firdecim_crcf_execute(st->filter, x, &y);
        // agc_crcf_execute(st->agc, y, &y);
        resamp_crcf_execute(st->resamp, y, &st->buffer[new_avail], &nw);
        new_avail += nw;
    }

    pthread_mutex_lock(&st->mutex);
    st->avail = new_avail;
    pthread_mutex_unlock(&st->mutex);
    pthread_cond_signal(&st->cond);
}

void input_reset(input_t *st)
{
    st->avail = 0;
    st->used = 0;
    st->skip = 0;
    st->resamp_rate = 1.0f;
}

void input_init(input_t *st, output_t *output, unsigned int program, FILE *outfp)
{
    st->buffer = malloc(sizeof(float complex) * INPUT_BUF_LEN);
    st->output = output;
    st->outfp = outfp;

    st->agc = agc_crcf_create();
    st->filter = firdecim_crcf_create(2, filter_taps, sizeof(filter_taps) / sizeof(filter_taps[0]));
    st->resamp = resamp_crcf_create(1.0f, 4, 0.45f, 60.0f, 16);

    input_reset(st);

    pthread_cond_init(&st->cond, NULL);
    pthread_mutex_init(&st->mutex, NULL);
    pthread_create(&st->worker_thread, NULL, input_worker, st);

    acquire_init(&st->acq, st);
    decode_init(&st->decode, st);
    frame_init(&st->frame, st);
    frame_set_program(&st->frame, program);
    sync_init(&st->sync, st);
}
