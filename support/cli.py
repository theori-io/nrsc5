#!/usr/bin/env python3

import argparse
import json
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
        self.packet_output = None
        self.navteq_alternate_frequencies = {}
        self.audio_packets_valid = 0
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
        parser.add_argument("--iq-input-format", choices=["cu8", "cs16", "cf32"])
        parser.add_argument("-w", metavar="iq-output")
        parser.add_argument("-o", metavar="audio-output")
        parser.add_argument("-t", choices=["wav", "raw"], default="wav")
        parser.add_argument("-T", action="store_true")
        parser.add_argument("-D", metavar="direct-sampling-mode", type=int, default=-1)
        parser.add_argument("--dump-hdc", metavar="hdc-output")
        parser.add_argument("--dump-packets", metavar="packet-output",
                            help="write packet and stream data as JSON Lines")
        parser.add_argument("--decode-weather", action="store_true",
                            help="write decoded weather records to stdout")
        parser.add_argument("--decode-traffic", action="store_true",
                            help="write decoded traffic records to stdout")
        parser.add_argument("--location-table", metavar="file",
                            help="resolve NAVTEQ locations from a location-table TSV file")
        parser.add_argument("--dump-aas-files", metavar="directory")
        input_group.add_argument("frequency", nargs="?", type=float)
        parser.add_argument("program", type=int)
        self.args = parser.parse_args()

        if self.args.frequency and self.args.frequency < 10000:
            self.args.frequency *= 1e6

        if self.args.r and not self.args.iq_input_format:
            if self.args.r.endswith(".cs16"):
                self.args.iq_input_format = "cs16"
            elif self.args.r.endswith(".cf32"):
                self.args.iq_input_format = "cf32"
            else:
                self.args.iq_input_format = "cu8"

    def run(self):
        logging.basicConfig(level=self.args.l * 10,
                            format="%(asctime)s %(message)s",
                            datefmt="%H:%M:%S")
        if self.args.q:
            logging.disable(logging.CRITICAL)

        if self.args.location_table:
            self.radio.open_location_table(self.args.location_table)

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

        if self.args.dump_packets:
            self.packet_output = sys.stdout if self.args.dump_packets == "-" else open(self.args.dump_packets, "w", encoding="ascii")

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
                    elif self.args.iq_input_format == "cf32":
                        self.radio.pipe_samples_cf32(data)
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

        if self.args.dump_packets:
            self.packet_output.close()
        self.radio.close_location_table()

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

    def format_am_flags(self, evt):
        parts = [f'Digital bandwidth: {"reduced" if evt.rdbi else "full"}']
        if not evt.rdbi:
            if evt.psmi != 2:
                parts.append(f'analog bandwidth: {"8 kHz" if evt.aabi else "5 kHz"}')
                parts.append(f'secondary/tertiary power: {"high" if evt.pli else "low"}')
            parts.append(f'PIDS power: {"high" if evt.hppi else "low"}')
        return ", ".join(parts)

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
            if evt.pli != -1:
                logging.info(self.format_am_flags(evt))
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
                else:
                    self.audio_packets_valid += 1

                if self.audio_packets_valid >= 32:
                    logging.info("Audio bit rate: %.1f kbps", self.audio_bytes * 8 * nrsc5.SAMPLE_RATE_AUDIO
                                 / nrsc5.AUDIO_FRAME_SAMPLES / self.audio_packets_valid / 1000)
                    self.audio_packets_valid = 0
                    self.audio_bytes = 0
                if self.audio_packets >= 32:
                    if self.audio_errors > 0:
                        logging.warning("Audio packet CRC mismatches: %d", self.audio_errors)
                    self.audio_packets = 0
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
                    blocking_audio_output = bool(self.args.r)
                    try:
                        self.audio_queue.put(evt.data, block=blocking_audio_output)
                    except queue.Full:
                        logging.warning("Audio output queue full, dropping samples")
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
                if evt.commercial:
                    date_str = evt.commercial.valid_until.strftime("%Y-%m-%d")
                    logging.info("Commercial: price=%s until=%s url=\"%s\" seller=\"%s\" desc=\"%s\" received_as=%d",
                                 evt.commercial.price, date_str, evt.commercial.contact_url, evt.commercial.seller,
                                 evt.commercial.description, evt.commercial.received_as)
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
            logging.debug("Stream data: port=%04X seq=%04X mime=%s size=%s data_hex=%s",
                          evt.component.data.port, evt.seq, evt.component.data.mime.name,
                          len(evt.data), evt.data.hex())
            self.dump_packets_data(evt_type, evt)
        elif evt_type == nrsc5.EventType.PACKET:
            logging.debug("Packet data: port=%04X seq=%04X mime=%s size=%s data_hex=%s",
                          evt.component.data.port, evt.seq, evt.component.data.mime.name,
                          len(evt.data), evt.data.hex())
            self.dump_packets_data(evt_type, evt)
        elif evt_type == nrsc5.EventType.NAVTEQ_DIGITAL_TRAFFIC:
            logging.debug("NAVTEQ Digital Traffic: port=%04X seq=%04X generation=%d terminal=%s entries=%d",
                          evt.port, evt.seq, evt.generation, evt.is_terminal, len(evt.entries))
            for entry in evt.entries:
                self.log_navteq_digital_traffic_entry(entry)
                if self.args.decode_traffic:
                    point = self.radio.location_table_lookup(entry.country_code,
                                                             entry.location_table_number,
                                                             entry.location)
                    record = {"kind": "navteq_digital_traffic", "port": evt.port,
                              "seq": evt.seq, "event": entry.event,
                              "location": entry.location, "extent": entry.extent,
                              "description": self.radio.alert_c_event_description(
                                  entry.event, entry.quantifier)}
                    if point is not None:
                        record.update({"location_name": point.name,
                                       "latitude": point.latitude,
                                       "longitude": point.longitude})
                    print(json.dumps(record), flush=True)
        elif evt_type == nrsc5.EventType.NAVTEQ_ALTERNATE_FREQUENCIES:
            for entry in evt.entries:
                if self.navteq_alternate_frequencies.get(entry.index) != entry.frequency_hz:
                    self.navteq_alternate_frequencies[entry.index] = entry.frequency_hz
                    logging.info("NAVTEQ alternate frequency: index=%d frequency=%.1f MHz",
                                 entry.index, entry.frequency_hz / 1000000)
                    if self.args.decode_traffic:
                        print(json.dumps({"kind": "navteq_alternate_frequency", "port": evt.port,
                                          "seq": evt.seq, "index": entry.index,
                                          "frequency_hz": entry.frequency_hz}), flush=True)
        elif evt_type == nrsc5.EventType.TTN_TEC:
            logging.info("TTN TEC: message=%d location=%d effect=%d cause=%d",
                         evt.message_id, evt.location, evt.effect_code, evt.cause_code)
            if self.args.decode_traffic:
                record = {"kind": "ttn_tec", "message_id": evt.message_id,
                          "version": evt.version, "expiry_time": evt.expiry_time,
                          "cancel": evt.cancel,
                          "location": evt.location, "country_code": evt.country_code,
                          "location_table_number": evt.location_table_number,
                          "effect": evt.effect_code, "cause": evt.cause_code,
                          "warning_level": evt.warning_level,
                          "direction_positive": evt.direction_positive,
                          "both_directions": evt.both_directions, "extent": evt.extent}
                if evt.location_resolved:
                    record.update({"location_name": evt.resolved_location.name,
                                   "latitude": evt.resolved_location.latitude,
                                   "longitude": evt.resolved_location.longitude})
                print(json.dumps(record), flush=True)
        elif evt_type == nrsc5.EventType.HERE_TFP:
            logging.info("HERE TFP: message=%d location=%d LOS=%d speed=%d km/h",
                         evt.message_id, evt.location, evt.level_of_service,
                         evt.average_speed)
            if self.args.decode_traffic:
                record = {"kind": "here_tfp", "message_id": evt.message_id,
                          "version": evt.version, "expiry_time": evt.expiry_time,
                          "cancel": evt.cancel, "start_time": evt.start_time,
                          "duration": evt.duration,
                          "spatial_resolution": evt.spatial_resolution,
                          "polygon_index": evt.polygon_index,
                          "level_of_service": evt.level_of_service,
                          "average_speed_kph": evt.average_speed,
                          "free_flow_travel_time": evt.free_flow_travel_time,
                          "delay": evt.delay, "location": evt.location,
                          "country_code": evt.country_code,
                          "location_table_number": evt.location_table_number,
                          "direction_positive": evt.direction_positive,
                          "both_directions": evt.both_directions, "extent": evt.extent}
                if evt.location_resolved:
                    record.update({"location_name": evt.resolved_location.name,
                                   "latitude": evt.resolved_location.latitude,
                                   "longitude": evt.resolved_location.longitude})
                print(json.dumps(record), flush=True)
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
        elif evt_type == nrsc5.EventType.EXCITER_INFO:
            logging.debug("Exciter manuf. \"%s\", core version %d.%d.%d.%d, core status %d, manuf. version %d.%d.%d.%d, manuf. status %d, importer connected? %s",
                          evt.manufacturer_id, evt.core_version[0], evt.core_version[1], evt.core_version[2],
                          evt.core_version[3], evt.core_status,
                          evt.manufacturer_version[0], evt.manufacturer_version[1], evt.manufacturer_version[2],
                          evt.manufacturer_version[3], evt.manufacturer_status,
                          "yes" if evt.importer_connected else "no")
        elif evt_type == nrsc5.EventType.IMPORTER_INFO:
            logging.debug("Importer manuf. \"%s\", core version %d.%d.%d.%d, core status %d, manuf. version %d.%d.%d.%d, manuf. status %d",
                          evt.manufacturer_id, evt.core_version[0], evt.core_version[1], evt.core_version[2],
                          evt.core_version[3], evt.core_status,
                          evt.manufacturer_version[0], evt.manufacturer_version[1], evt.manufacturer_version[2],
                          evt.manufacturer_version[3], evt.manufacturer_status)
        elif evt_type == nrsc5.EventType.LEAP_SECOND_OFFSET:
            logging.debug("Leap second offset: pending=%d, current=%d, ALFN of pending adjustment=%d",
                          evt.pending_offset, evt.current_offset,
                          evt.pending_alfn)
        elif evt_type == nrsc5.EventType.LOCAL_TIME:
            logging.debug("Local time: UTC offset=%d minutes, DST schedule=%d, DST in effect regionally? %s, DST practiced locally? %s",
                          evt.utc_offset,
                          evt.dst_schedule,
                          "yes" if evt.dst_regional else "no", "yes" if evt.dst_local else "no")
        elif evt_type == nrsc5.EventType.TTN_CITY_DATABASE:
            logging.info("TTN city database: version=%d timestamp=%d cities=%d",
                         evt.database_version, evt.timestamp, len(evt.cities))
            if self.args.decode_weather:
                print(json.dumps({"kind": "ttn_city_database", "version": evt.database_version,
                                  "timestamp": evt.timestamp, "cities": len(evt.cities)}))
        elif evt_type == nrsc5.EventType.TTN_WEATHER:
            logging.info("TTN weather: timestamp=%d cities=%d",
                         evt.timestamp, len(evt.cities))
            if self.args.decode_weather:
                for city in evt.cities:
                    short_forecasts = [f._asdict() for f in city.short_forecasts]
                    long_forecasts = [f._asdict() for f in city.long_forecasts]
                    for forecast in short_forecasts + long_forecasts:
                        forecast["weather_condition_name"] = self.radio.ttn_weather_condition_name(
                            forecast["weather_condition"])
                    for forecast in short_forecasts:
                        forecast["wind_speed_mph"] = forecast.pop("wind_speed")
                        forecast["temperature_f"] = forecast.pop("temperature")
                        forecast["feels_like_temperature_f"] = forecast.pop("feels_like_temperature")
                        forecast["humidity_percent"] = forecast.pop("humidity")
                        forecast["chance_of_precipitation_percent"] = forecast.pop("chance_of_precipitation")
                    for forecast in long_forecasts:
                        forecast["wind_speed_mph"] = forecast.pop("wind_speed")
                        forecast["high_temperature_f"] = forecast.pop("high_temperature")
                        forecast["low_temperature_f"] = forecast.pop("low_temperature")
                        forecast["chance_of_precipitation_percent"] = forecast.pop("chance_of_precipitation")
                        forecast["external_feels_like_temperature_f"] = forecast.pop("external_feels_like_temperature")
                        forecast["maximum_humidity_percent"] = forecast.pop("maximum_humidity")
                    print(json.dumps({"kind": "ttn_weather", "timestamp": evt.timestamp,
                                      "city_id": city.city.city_id,
                                      "provider_city_id": city.city.provider_city_id,
                                      "name": city.city.name,
                                      "latitude": city.city.latitude,
                                      "longitude": city.city.longitude,
                                      "short_forecasts": short_forecasts,
                                      "long_forecasts": long_forecasts}), flush=True)

    def dump_packets_data(self, evt_type, evt):
        if not self.packet_output:
            return

        record = {
            "kind": "packet" if evt_type == nrsc5.EventType.PACKET else "stream",
            "port": evt.port,
            "seq": evt.seq,
            "mime": evt.mime.name,
            "mime_id": f"{evt.mime.value:08x}",
            "service": evt.service.number,
            "service_name": evt.service.name,
            "service_data_type": evt.component.data.service_data_type.name,
            "aas_type": evt.component.data.type.name,
            "data_hex": evt.data.hex(),
        }
        self.packet_output.write(json.dumps(record, separators=(",", ":")) + "\n")
        self.packet_output.flush()

    def log_navteq_digital_traffic_entry(self, entry):
        event_description = self.radio.alert_c_event_description(entry.event,
                                                                 entry.quantifier)
        point = self.radio.location_table_lookup(entry.country_code,
                                                 entry.location_table_number,
                                                 entry.location)
        direction = "negative" if entry.direction else "positive"
        if point is None:
            logging.debug("  NAVTEQ Digital Traffic entry: type=%d country=%d reserved=%d ltn=%d location=%d direction=%s extent=%d bidirectional=%d diversion=%d duration_type=%d control_code=%d event=%d quantifier=%d description=\"%s\"",
                          entry.record_type, entry.country_code, entry.reserved,
                          entry.location_table_number, entry.location, direction,
                          entry.extent, entry.bidirectional, entry.diversion,
                          entry.duration_type, entry.control_code, entry.event,
                          entry.quantifier, event_description)
            return

        endpoint = point
        resolved_extent = 0
        visited = {point.location}
        while resolved_extent < entry.extent:
            next_location = endpoint.negative if entry.direction else endpoint.positive
            if not next_location or next_location in visited:
                break
            next_point = self.radio.location_table_lookup(entry.country_code,
                                                          entry.location_table_number,
                                                          next_location)
            if next_point is None:
                break
            endpoint = next_point
            visited.add(endpoint.location)
            resolved_extent += 1

        logging.debug("  NAVTEQ Digital Traffic entry: type=%d country=%d reserved=%d ltn=%d location=%d direction=%s extent=%d bidirectional=%d diversion=%d duration_type=%d control_code=%d event=%d quantifier=%d description=\"%s\" name=\"%s\" lat=%.5f lon=%.5f span_to=%d span_name=\"%s\" span_lat=%.5f span_lon=%.5f resolved_extent=%d/%d",
                      entry.record_type, entry.country_code, entry.reserved,
                      entry.location_table_number, entry.location, direction,
                      entry.extent, entry.bidirectional, entry.diversion,
                      entry.duration_type, entry.control_code, entry.event,
                      entry.quantifier, event_description, point.name,
                      point.latitude, point.longitude, endpoint.location,
                      endpoint.name, endpoint.latitude, endpoint.longitude,
                      resolved_extent, entry.extent)


if __name__ == "__main__":
    cli = NRSC5CLI()
    cli.run()
