#!/usr/bin/env python3

import argparse
import logging
import os
import queue
import signal
import sys
import threading
import wave

import pyaudio

import nrsc5


class NRSC5CLI:
    def __init__(self):
        self.radio = nrsc5.NRSC5(lambda evt_type, evt: self.callback(evt_type, evt))
        self.nrsc5_version = self.radio.get_version()
        self.parse_args()
        self.audio_queue = queue.Queue(maxsize=16)
        self.device_condition = threading.Condition()
        self.interrupted = False
        self.iq_output = None
        self.wav_output = None
        self.raw_output = None
        self.hdc_output = None
        self.audio_packets = 0
        self.audio_bytes = 0
        self.audio_errors = 0
        signal.signal(signal.SIGINT, self._signal_handler)

    def _signal_handler(self, sig, frame):
        logging.info("Stopping...")
        self.interrupted = True
        with self.device_condition:
            self.device_condition.notify()

    def parse_args(self):
        parser = argparse.ArgumentParser(description="Receive NRSC-5 signals.")
        input_group = parser.add_mutually_exclusive_group()
        parser.add_argument("-v", action="version", version="nrsc5 revision " + self.nrsc5_version)
        parser.add_argument("-q", action="store_true")
        parser.add_argument("--am", action="store_true")
        parser.add_argument("-l", metavar="log-level", type=int, default=2)
        parser.add_argument("-d", metavar="device-index", type=int, default=0)
        parser.add_argument("-H", metavar="rtltcp-host")
        parser.add_argument("-p", metavar="ppm-error", type=int)
        parser.add_argument("-g", metavar="gain", type=float)
        input_group.add_argument("-r", metavar="iq-input")
        parser.add_argument("--iq-input-format", choices=["cu8", "cs16"], default="cu8")
        parser.add_argument("-w", metavar="iq-output")
        parser.add_argument("-o", metavar="audio-output")
        parser.add_argument("-t", choices=["wav", "raw"], default="wav")
        parser.add_argument("-T", action="store_true")
        parser.add_argument("-D", metavar="direct-sampling-mode", type=int, default=-1)
        parser.add_argument("--dump-hdc", metavar="hdc-output")
        parser.add_argument("--dump-aas-files", metavar="directory")
        input_group.add_argument("frequency", nargs="?", type=float)
        parser.add_argument("program", type=int)
        self.args = parser.parse_args()

        if self.args.frequency and self.args.frequency < 10000:
            self.args.frequency *= 1e6

    def run(self):
        logging.basicConfig(level=self.args.l * 10,
                            format="%(asctime)s %(message)s",
                            datefmt="%H:%M:%S")
        if self.args.q:
            logging.disable(logging.CRITICAL)

        if self.args.r:
            iq_input = sys.stdin.buffer if self.args.r == "-" else open(self.args.r, "rb")
            self.radio.open_pipe()
        elif self.args.H:
            host = self.args.H
            port = "1234"
            if ':' in host:
                host, port = host.split(':')
            self.radio.open_rtltcp(host, int(port))
            self.radio.set_frequency(self.args.frequency)
            if self.args.g:
                self.radio.set_gain(self.args.g)
        else:
            self.radio.open(self.args.d)
            self.radio.set_frequency(self.args.frequency)
            if self.args.g:
                self.radio.set_gain(self.args.g)

        if self.args.am:
            self.radio.set_mode(nrsc5.Mode.AM)

        self.radio.set_bias_tee(self.args.T)
        if self.args.D != -1:
            self.radio.set_direct_sampling(self.args.D)

        if self.args.p is not None:
            self.radio.set_freq_correction(self.args.p)

        if self.args.w:
            self.iq_output = sys.stdout.buffer if self.args.w == "-" else open(self.args.w, "wb")

        if self.args.o:
            if self.args.t == "wav":
                self.wav_output = wave.open(sys.stdout.buffer if self.args.o == "-" else self.args.o, "wb")
                self.wav_output.setnchannels(2)
                self.wav_output.setsampwidth(2)
                self.wav_output.setframerate(nrsc5.SAMPLE_RATE_AUDIO)
                self.wav_output.setnframes((1 << 30) - 64)
            elif self.args.t == "raw":
                self.raw_output = sys.stdout.buffer if self.args.o == "-" else open(self.args.o, "wb")
        else:
            audio_thread = threading.Thread(target=self.audio_worker)
            audio_thread.start()

        if self.args.dump_hdc:
            self.hdc_output = sys.stdout.buffer if self.args.dump_hdc == "-" else open(self.args.dump_hdc, "wb")

        self.radio.start()

        try:
            if self.args.r:
                while not self.interrupted:
                    data = iq_input.read(32768)
                    if not data:
                        break
                    if self.args.iq_input_format == "cu8":
                        self.radio.pipe_samples_cu8(data)
                    elif self.args.iq_input_format == "cs16":
                        self.radio.pipe_samples_cs16(data)
            else:
                with self.device_condition:
                    self.device_condition.wait()
        except nrsc5.NRSC5Error as err:
            logging.error(err)

        self.radio.stop()
        self.radio.set_bias_tee(0)
        self.radio.close()

        if self.args.r:
            iq_input.close()

        if self.args.w:
            self.iq_output.close()

        if self.args.o:
            if self.args.t == "wav":
                self.wav_output.close()
            elif self.args.t == "raw":
                self.raw_output.close()
        else:
            self.audio_queue.put(None)
            audio_thread.join()

        if self.args.dump_hdc:
            self.hdc_output.close()

    def audio_worker(self):
        audio = pyaudio.PyAudio()
        try:
            index = audio.get_default_output_device_info()["index"]
            stream = audio.open(format=pyaudio.paInt16,
                                channels=2,
                                rate=nrsc5.SAMPLE_RATE_AUDIO,
                                output_device_index=index,
                                output=True)
        except OSError:
            logging.warning("No audio output device available.")
            stream = None

        while True:
            samples = self.audio_queue.get()
            if samples is None:
                break
            if stream:
                stream.write(samples)
            self.audio_queue.task_done()

        if stream:
            stream.stop_stream()
            stream.close()
        audio.terminate()

    def adts_header(self, length):
        length += 7
        return bytes([
            0xff,
            0xf1,
            0x5c,
            0x80 | (length >> 11),
            (length >> 3) & 0xff,
            0x1f | ((length & 0x07) << 5),
            0xfc
        ])

    def callback(self, evt_type, evt):
        if evt_type == nrsc5.EventType.LOST_DEVICE:
            logging.info("Lost device")
            with self.device_condition:
                self.device_condition.notify()
        elif evt_type == nrsc5.EventType.AGC:
            if evt.is_final:
                logging.info("Best gain: %.1f dB, Peak amplitude: %.1f dBFS", evt.gain_db, evt.peak_dbfs)
            else:
                logging.debug("Gain: %.1f dB, Peak amplitude: %.1f dBFS", evt.gain_db, evt.peak_dbfs)
        elif evt_type == nrsc5.EventType.IQ:
            if self.args.w:
                self.iq_output.write(evt.data)
        elif evt_type == nrsc5.EventType.SYNC:
            logging.info("Synchronized")
            logging.info("Frequency offset: %.0f Hz", evt.freq_offset)
            logging.info("Primary service mode: %d", evt.psmi)
        elif evt_type == nrsc5.EventType.LOST_SYNC:
            logging.info("Lost synchronization")
        elif evt_type == nrsc5.EventType.MER:
            logging.info("MER: %.1f dB (lower), %.1f dB (upper)", evt.lower, evt.upper)
        elif evt_type == nrsc5.EventType.BER:
            logging.info("BER: %.6f", evt.cber)
        elif evt_type == nrsc5.EventType.HDC:
            if evt.program == self.args.program:
                if self.args.dump_hdc:
                    self.hdc_output.write(self.adts_header(len(evt.data)))
                    self.hdc_output.write(evt.data)

                self.audio_packets += 1
                self.audio_bytes += len(evt.data)
                if evt.flags & nrsc5.PacketFlags.CRC_ERROR:
                    self.audio_errors += 1

                if self.audio_packets >= 32:
                    logging.info("Audio bit rate: %.1f kbps", self.audio_bytes * 8 * nrsc5.SAMPLE_RATE_AUDIO
                                 / nrsc5.AUDIO_FRAME_SAMPLES / self.audio_packets / 1000)
                    if self.audio_errors > 0:
                        logging.warning("Audio packet CRC mismatches: %d", self.audio_errors)
                    self.audio_packets = 0
                    self.audio_bytes = 0
                    self.audio_errors = 0
        elif evt_type == nrsc5.EventType.AUDIO:
            if evt.program == self.args.program:
                if self.args.o:
                    if self.args.t == "wav":
                        try:
                            self.wav_output.writeframes(evt.data)
                        except OSError:
                            pass
                    elif self.args.t == "raw":
                        self.raw_output.write(evt.data)
                else:
                    self.audio_queue.put(evt.data)
        elif evt_type == nrsc5.EventType.ID3:
            if evt.program == self.args.program:
                if evt.title:
                    logging.info("Title: %s", evt.title)
                if evt.artist:
                    logging.info("Artist: %s", evt.artist)
                if evt.album:
                    logging.info("Album: %s", evt.album)
                if evt.genre:
                    logging.info("Genre: %s", evt.genre)
                for comment in evt.comments:
                    logging.info("Comment: lang=%s %s %s", comment.lang, comment.short_content_desc, comment.full_text)
                if evt.ufid:
                    logging.info("Unique file identifier: %s %s", evt.ufid.owner, evt.ufid.id)
                if evt.xhdr:
                    logging.info("XHDR: param=%s mime=%s lot=%s",
                                 evt.xhdr.param, evt.xhdr.mime.name, evt.xhdr.lot)
        elif evt_type == nrsc5.EventType.SIG:
            for service in evt:
                logging.info("SIG Service: type=%s number=%s name=%s",
                             service.type.name, service.number, service.name)
                for component in service.components:
                    if component.type == nrsc5.ComponentType.AUDIO:
                        logging.info("  Audio component: id=%s port=%04X type=%s mime=%s",
                                     component.id, component.audio.port,
                                     component.audio.type.name, component.audio.mime.name)
                    elif component.type == nrsc5.ComponentType.DATA:
                        logging.info("  Data component: id=%s port=%04X service_data_type=%s type=%s mime=%s",
                                     component.id, component.data.port,
                                     component.data.service_data_type.name,
                                     component.data.type.name, component.data.mime.name)
        elif evt_type == nrsc5.EventType.STREAM:
            logging.debug("Stream data: port=%04X seq=%04X mime=%s size=%s",
                          evt.component.data.port, evt.seq, evt.component.data.mime.name, len(evt.data))
        elif evt_type == nrsc5.EventType.PACKET:
            logging.debug("Packet data: port=%04X seq=%04X mime=%s size=%s",
                          evt.component.data.port, evt.seq, evt.component.data.mime.name, len(evt.data))
        elif evt_type == nrsc5.EventType.LOT:
            if self.args.dump_aas_files:
                path = os.path.join(self.args.dump_aas_files, evt.name)
                try:
                    with open(path, "wb") as file:
                        file.write(evt.data)
                except OSError as e:
                    logging.warning(f"Failed to write AAS file: {e}")
            time_str = evt.expiry_utc.strftime("%Y-%m-%dT%H:%M:%SZ")
            logging.info("LOT file: port=%04X lot=%s name=%s size=%s mime=%s expiry=%s",
                         evt.component.data.port, evt.lot, evt.name, len(evt.data), evt.mime.name, time_str)
        elif evt_type == nrsc5.EventType.LOT_HEADER:
            time_str = evt.expiry_utc.strftime("%Y-%m-%dT%H:%M:%SZ")
            logging.debug("LOT header: port=%04X lot=%s name=%s size=%s mime=%s expiry=%s",
                          evt.component.data.port, evt.lot, evt.name, evt.size, evt.mime.name, time_str)
        elif evt_type == nrsc5.EventType.LOT_FRAGMENT:
            if not evt.is_duplicate:
                logging.debug("LOT fragment: port=%04X lot=%d seq=%d repeat=%d size=%d bytes_so_far=%d",
                              evt.component.data.port, evt.lot, evt.seq, evt.repeat, len(evt.data), evt.bytes_so_far)
        elif evt_type == nrsc5.EventType.STATION_ID:
            logging.info("Country: %s, FCC facility ID: %s", evt.country_code, evt.fcc_facility_id)
        elif evt_type == nrsc5.EventType.STATION_NAME:
            logging.info("Station name: %s", evt.name)
        elif evt_type == nrsc5.EventType.STATION_SLOGAN:
            logging.info("Slogan: %s", evt.slogan)
        elif evt_type == nrsc5.EventType.STATION_MESSAGE:
            logging.info("Message: %s", evt.message)
        elif evt_type == nrsc5.EventType.STATION_LOCATION:
            logging.info("Station location: %.4f, %.4f, %dm", evt.latitude, evt.longitude, evt.altitude)
        elif evt_type == nrsc5.EventType.AUDIO_SERVICE_DESCRIPTOR:
            logging.info("Audio program %s: %s, type: %s, sound experience %s",
                         evt.program,
                         evt.access.name,
                         self.radio.program_type_name(evt.type),
                         evt.sound_exp)
        elif evt_type == nrsc5.EventType.DATA_SERVICE_DESCRIPTOR:
            logging.info("Data service: %s, type: %s, MIME type %03x",
                         evt.access.name,
                         self.radio.service_data_type_name(evt.type),
                         evt.mime_type)
        elif evt_type == nrsc5.EventType.EMERGENCY_ALERT:
            if evt.message is not None:
                categories = ", ".join(self.radio.alert_category_name(category) for category in evt.categories)
                logging.info("Alert: Category=[%s] %s=%s %s", categories, evt.location_format.name, str(evt.locations), evt.message)
            else:
                logging.info("Alert ended")
        elif evt_type == nrsc5.EventType.AUDIO_SERVICE:
            logging.info("Audio service %s: %s, type: %s, codec: %d, blend: %s, gain: %d dB, delay: %d, latency: %d",
                         evt.program,
                         evt.access.name,
                         self.radio.program_type_name(evt.type),
                         evt.codec_mode,
                         evt.blend_control.name,
                         evt.digital_audio_gain,
                         evt.common_delay,
                         evt.latency)
        elif evt_type == nrsc5.EventType.HERE_IMAGE:
            if self.args.dump_aas_files:
                time_int = int(evt.time_utc.timestamp())
                path = os.path.join(self.args.dump_aas_files, f"{time_int}_{evt.name}")
                try:
                    with open(path, "wb") as file:
                        file.write(evt.data)
                except OSError as e:
                    logging.warning(f"Failed to write HERE image: {e}")
            time_str = evt.time_utc.strftime("%Y-%m-%dT%H:%M:%SZ")
            logging.info("HERE Image: type=%s, seq=%d, n1=%d, n2=%d, time=%s, lat1=%.5f, lon1=%.5f, lat2=%.5f, lon2=%.5f, name=%s, size=%d",
                         evt.image_type.name, evt.seq, evt.n1, evt.n2, time_str, evt.latitude1, evt.longitude1,
                         evt.latitude2, evt.longitude2, evt.name, len(evt.data))


if __name__ == "__main__":
    cli = NRSC5CLI()
    cli.run()
